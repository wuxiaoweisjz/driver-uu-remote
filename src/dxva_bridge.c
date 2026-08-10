#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxva.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dxva_bridge.h"
#include "helper_protocol.h"

#define DXVA_BUFFER_COUNT 9
#define DXVA_RUNTIME_COUNT 64
#define HELPER_DECODE_IO_TIMEOUT_MS 2000
#define HELPER_PROBE_CACHE_MS 10000
#define DXVA_RETRY_BACKOFF_MS 1000
#define DXVA_MAX_SAMPLE_WIDTH 2560
#define DXVA_MAX_SAMPLE_HEIGHT 1440

typedef struct DxvaRuntime DxvaRuntime;

typedef struct DecoderBuffer {
    uint8_t *data;
    UINT capacity;
    UINT offset;
    UINT size;
} DecoderBuffer;

typedef struct BridgeDecoder {
    ID3D11VideoDecoder iface;
    LONG refs;
    ID3D11Device *device;
    D3D11_VIDEO_DECODER_DESC desc;
    D3D11_VIDEO_DECODER_CONFIG config;
    DecoderBuffer buffers[DXVA_BUFFER_COUNT];
    ID3D11VideoDecoderOutputView *output;
    uint8_t *frame;
    UINT frame_capacity;
    uint8_t *upload;
    UINT upload_capacity;
    DxvaRuntime *runtime;
    SOCKET helper;
    ULONGLONG retry_after;
    CRITICAL_SECTION lock;
} BridgeDecoder;

typedef struct BridgeOutputView {
    ID3D11VideoDecoderOutputView iface;
    LONG refs;
    ID3D11Device *device;
    ID3D11Resource *resource;
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC desc;
} BridgeOutputView;

struct DxvaRuntime {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11DeviceVtbl device_vtbl;
    ID3D11DeviceContextVtbl context_vtbl;
    HRESULT (STDMETHODCALLTYPE *device_query)(ID3D11Device *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *context_query)(ID3D11DeviceContext *, REFIID, void **);
    ID3D11VideoDeviceVtbl video_device_vtbl;
    ID3D11VideoContextVtbl video_context_vtbl;
};

static DxvaRuntime runtimes[DXVA_RUNTIME_COUNT];
static LONG runtime_count;
static LONG runtime_lock;
static LONG module_pinned;
static LONG helper_probe_result;
static ULONGLONG helper_probe_time;

static DxvaRuntime *runtime_from_device(ID3D11Device *device)
{
    LONG i;
    for (i = 0; i < runtime_count; ++i)
        if (device->lpVtbl == &runtimes[i].device_vtbl) return &runtimes[i];
    return NULL;
}

static DxvaRuntime *runtime_from_context(ID3D11DeviceContext *context)
{
    LONG i;
    for (i = 0; i < runtime_count; ++i)
        if (context->lpVtbl == &runtimes[i].context_vtbl) return &runtimes[i];
    return NULL;
}

static DxvaRuntime *runtime_from_video_device(ID3D11VideoDevice *device)
{
    LONG i;
    for (i = 0; i < runtime_count; ++i)
        if (device->lpVtbl == &runtimes[i].video_device_vtbl) return &runtimes[i];
    return NULL;
}

typedef struct BitWriter {
    uint8_t data[256];
    UINT bits;
} BitWriter;

static void bw_bit(BitWriter *writer, UINT value)
{
    UINT byte = writer->bits >> 3;
    UINT shift = 7 - (writer->bits & 7);
    if (!(writer->bits & 7)) writer->data[byte] = 0;
    writer->data[byte] |= (uint8_t)((value & 1) << shift);
    ++writer->bits;
}

static void bw_bits(BitWriter *writer, UINT value, UINT count)
{
    while (count) bw_bit(writer, value >> --count);
}

static void bw_ue(BitWriter *writer, UINT value)
{
    UINT code = value + 1;
    UINT bits = 0;
    UINT scan = code;
    while (scan) { ++bits; scan >>= 1; }
    if (bits > 1) bw_bits(writer, 0, bits - 1);
    bw_bits(writer, code, bits);
}

static void bw_se(BitWriter *writer, int value)
{
    bw_ue(writer, value <= 0 ? (UINT)(-value * 2) : (UINT)(value * 2 - 1));
}

static void bw_trailing(BitWriter *writer)
{
    bw_bit(writer, 1);
    while (writer->bits & 7) bw_bit(writer, 0);
}

static UINT append_nal(uint8_t *output, UINT offset, uint8_t header, const BitWriter *writer)
{
    UINT input_size = (writer->bits + 7) / 8;
    UINT i;
    UINT zeros = 0;
    output[offset++] = 0;
    output[offset++] = 0;
    output[offset++] = 0;
    output[offset++] = 1;
    output[offset++] = header;
    for (i = 0; i < input_size; ++i) {
        uint8_t value = writer->data[i];
        if (zeros >= 2 && value <= 3) {
            output[offset++] = 3;
            zeros = 0;
        }
        output[offset++] = value;
        zeros = value == 0 ? zeros + 1 : 0;
    }
    return offset;
}

static UINT build_h264_parameter_sets(const BridgeDecoder *decoder, uint8_t *output)
{
    const DecoderBuffer *parameters =
        &decoder->buffers[D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS];
    const DXVA_PicParams_H264 *pic;
    BitWriter sps;
    BitWriter pps;
    UINT coded_width;
    UINT coded_height;
    UINT crop_right;
    UINT crop_bottom;
    UINT offset = 0;
    if (!parameters->data || parameters->size < sizeof(*pic) ||
        parameters->offset > parameters->capacity - sizeof(*pic)) return 0;
    pic = (const DXVA_PicParams_H264 *)(parameters->data + parameters->offset);
    memset(&sps, 0, sizeof(sps));
    bw_bits(&sps, 100, 8);
    bw_bits(&sps, 0, 8);
    bw_bits(&sps, 51, 8);
    bw_ue(&sps, 0);
    bw_ue(&sps, pic->chroma_format_idc ? pic->chroma_format_idc : 1);
    if (pic->chroma_format_idc == 3) bw_bit(&sps, pic->residual_colour_transform_flag);
    bw_ue(&sps, pic->bit_depth_luma_minus8);
    bw_ue(&sps, pic->bit_depth_chroma_minus8);
    bw_bit(&sps, 0);
    bw_bit(&sps, 0);
    bw_ue(&sps, pic->log2_max_frame_num_minus4);
    bw_ue(&sps, pic->pic_order_cnt_type);
    if (pic->pic_order_cnt_type == 0) {
        bw_ue(&sps, pic->log2_max_pic_order_cnt_lsb_minus4);
    } else if (pic->pic_order_cnt_type == 1) {
        bw_bit(&sps, pic->delta_pic_order_always_zero_flag);
        bw_se(&sps, 0);
        bw_se(&sps, 0);
        bw_ue(&sps, 0);
    }
    bw_ue(&sps, pic->num_ref_frames);
    bw_bit(&sps, 0);
    bw_ue(&sps, pic->wFrameWidthInMbsMinus1);
    bw_ue(&sps, pic->wFrameHeightInMbsMinus1);
    bw_bit(&sps, pic->frame_mbs_only_flag);
    if (!pic->frame_mbs_only_flag) bw_bit(&sps, pic->MbaffFrameFlag);
    bw_bit(&sps, pic->direct_8x8_inference_flag);
    coded_width = ((UINT)pic->wFrameWidthInMbsMinus1 + 1) * 16;
    coded_height = ((UINT)pic->wFrameHeightInMbsMinus1 + 1) * 16 *
                   (pic->frame_mbs_only_flag ? 1 : 2);
    crop_right = coded_width > decoder->desc.SampleWidth
        ? (coded_width - decoder->desc.SampleWidth) / 2 : 0;
    crop_bottom = coded_height > decoder->desc.SampleHeight
        ? (coded_height - decoder->desc.SampleHeight) /
          (2 * (pic->frame_mbs_only_flag ? 1 : 2)) : 0;
    bw_bit(&sps, crop_right || crop_bottom);
    if (crop_right || crop_bottom) {
        bw_ue(&sps, 0);
        bw_ue(&sps, crop_right);
        bw_ue(&sps, 0);
        bw_ue(&sps, crop_bottom);
    }
    bw_bit(&sps, 0);
    bw_trailing(&sps);

    memset(&pps, 0, sizeof(pps));
    bw_ue(&pps, 0);
    bw_ue(&pps, 0);
    bw_bit(&pps, pic->entropy_coding_mode_flag);
    bw_bit(&pps, pic->pic_order_present_flag);
    bw_ue(&pps, pic->num_slice_groups_minus1);
    bw_ue(&pps, pic->num_ref_idx_l0_active_minus1);
    bw_ue(&pps, pic->num_ref_idx_l1_active_minus1);
    bw_bit(&pps, pic->weighted_pred_flag);
    bw_bits(&pps, pic->weighted_bipred_idc, 2);
    bw_se(&pps, pic->pic_init_qp_minus26);
    bw_se(&pps, pic->pic_init_qs_minus26);
    bw_se(&pps, pic->chroma_qp_index_offset);
    bw_bit(&pps, pic->deblocking_filter_control_present_flag);
    bw_bit(&pps, pic->constrained_intra_pred_flag);
    bw_bit(&pps, pic->redundant_pic_cnt_present_flag);
    bw_bit(&pps, pic->transform_8x8_mode_flag);
    bw_bit(&pps, 0);
    bw_se(&pps, pic->second_chroma_qp_index_offset);
    bw_trailing(&pps);
    offset = append_nal(output, offset, 0x67, &sps);
    offset = append_nal(output, offset, 0x68, &pps);
    return offset;
}

static int guid_is(REFGUID left, REFGUID right)
{
    return left && right && memcmp(left, right, sizeof(GUID)) == 0;
}

static int decoder_dimensions_supported(const D3D11_VIDEO_DECODER_DESC *desc)
{
    return desc && desc->SampleWidth && desc->SampleHeight &&
           desc->SampleWidth <= DXVA_MAX_SAMPLE_WIDTH &&
           desc->SampleHeight <= DXVA_MAX_SAMPLE_HEIGHT;
}

static int send_all(SOCKET socket_handle, const void *data, size_t size)
{
    const char *cursor = data;
    while (size) {
        int count = send(socket_handle, cursor, size > INT_MAX ? INT_MAX : (int)size, 0);
        if (count <= 0) return 0;
        cursor += count;
        size -= count;
    }
    return 1;
}

static int recv_all(SOCKET socket_handle, void *data, size_t size)
{
    char *cursor = data;
    while (size) {
        int count = recv(socket_handle, cursor, size > INT_MAX ? INT_MAX : (int)size, 0);
        if (count <= 0) return 0;
        cursor += count;
        size -= count;
    }
    return 1;
}

static unsigned short helper_port(void)
{
    const char *text = getenv("UU_AMF_HELPER_PORT");
    unsigned long value = HELPER_DEFAULT_PORT;
    if (text && *text) {
        unsigned long parsed = 0;
        const char *cursor = text;
        while (*cursor >= '0' && *cursor <= '9') {
            parsed = parsed * 10 + (unsigned long)(*cursor - '0');
            if (parsed > 65535) break;
            ++cursor;
        }
        if (!*cursor && parsed && parsed <= 65535) value = parsed;
    }
    return (unsigned short)value;
}

static void configure_socket_timeouts(SOCKET socket_handle, DWORD timeout)
{
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
}

static int helper_handshake(SOCKET socket_handle, uint32_t type)
{
    HelperInitMessage init;
    HelperInitReply reply;
    memset(&init, 0, sizeof(init));
    init.magic = HELPER_MAGIC;
    init.type = type;
    init.reserved = HELPER_PROTOCOL_VERSION;
    memset(&reply, 0, sizeof(reply));
    return send_all(socket_handle, &init, sizeof(init)) &&
           recv_all(socket_handle, &reply, sizeof(reply)) && reply.enabled &&
           reply.protocol_version == HELPER_PROTOCOL_VERSION;
}

static void helper_mark_unavailable(ULONGLONG now)
{
    InterlockedExchange(&helper_probe_result, -1);
    helper_probe_time = now;
}

static void decoder_backoff(BridgeDecoder *decoder)
{
    ULONGLONG now = GetTickCount64();
    decoder->retry_after = now + DXVA_RETRY_BACKOFF_MS;
    helper_mark_unavailable(now);
}

static int decoder_connect(BridgeDecoder *decoder)
{
    struct sockaddr_in address;
    ULONGLONG now = GetTickCount64();
    LONG probed;
    if (decoder->helper != INVALID_SOCKET) return 1;
    if (decoder->retry_after && now < decoder->retry_after) return 0;
    probed = InterlockedCompareExchange(&helper_probe_result, 0, 0);
    if (helper_probe_time && now - helper_probe_time < HELPER_PROBE_CACHE_MS && probed <= 0) {
        decoder->retry_after = now + DXVA_RETRY_BACKOFF_MS;
        return 0;
    }
    decoder->helper = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (decoder->helper == INVALID_SOCKET) return 0;
    configure_socket_timeouts(decoder->helper, HELPER_DECODE_IO_TIMEOUT_MS);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(helper_port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(decoder->helper, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) goto fail;
    if (!helper_handshake(decoder->helper, HELPER_DECODER_INIT)) goto fail;
    decoder->retry_after = 0;
    return 1;
fail:
    closesocket(decoder->helper);
    decoder->helper = INVALID_SOCKET;
    decoder_backoff(decoder);
    return 0;
}

static HRESULT STDMETHODCALLTYPE decoder_query(ID3D11VideoDecoder *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (guid_is(iid, &IID_IUnknown) || guid_is(iid, &IID_ID3D11DeviceChild) ||
        guid_is(iid, &IID_ID3D11VideoDecoder)) {
        *out = iface;
        ID3D11VideoDecoder_AddRef(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE decoder_add_ref(ID3D11VideoDecoder *iface)
{
    return InterlockedIncrement(&((BridgeDecoder *)iface)->refs);
}

static ULONG STDMETHODCALLTYPE decoder_release(ID3D11VideoDecoder *iface)
{
    BridgeDecoder *decoder = (BridgeDecoder *)iface;
    LONG refs = InterlockedDecrement(&decoder->refs);
    if (!refs) {
        UINT i;
        if (decoder->output) ID3D11VideoDecoderOutputView_Release(decoder->output);
        if (decoder->helper != INVALID_SOCKET) closesocket(decoder->helper);
        for (i = 0; i < DXVA_BUFFER_COUNT; ++i) free(decoder->buffers[i].data);
        free(decoder->frame);
        free(decoder->upload);
        ID3D11Device_Release(decoder->device);
        DeleteCriticalSection(&decoder->lock);
        free(decoder);
    }
    return refs;
}

static void STDMETHODCALLTYPE decoder_get_device(ID3D11VideoDecoder *iface, ID3D11Device **out)
{
    BridgeDecoder *decoder = (BridgeDecoder *)iface;
    if (out) { *out = decoder->device; ID3D11Device_AddRef(*out); }
}

static HRESULT STDMETHODCALLTYPE child_get_private_decoder(ID3D11VideoDecoder *iface, REFGUID guid, UINT *size, void *data)
{ (void)iface; (void)guid; (void)size; (void)data; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE child_set_private_decoder(ID3D11VideoDecoder *iface, REFGUID guid, UINT size, const void *data)
{ (void)iface; (void)guid; (void)size; (void)data; return S_OK; }
static HRESULT STDMETHODCALLTYPE child_set_private_iface_decoder(ID3D11VideoDecoder *iface, REFGUID guid, const IUnknown *data)
{ (void)iface; (void)guid; (void)data; return S_OK; }

static HRESULT STDMETHODCALLTYPE decoder_creation(ID3D11VideoDecoder *iface,
    D3D11_VIDEO_DECODER_DESC *desc, D3D11_VIDEO_DECODER_CONFIG *config)
{
    BridgeDecoder *decoder = (BridgeDecoder *)iface;
    if (!desc || !config) return E_POINTER;
    *desc = decoder->desc;
    *config = decoder->config;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE decoder_driver_handle(ID3D11VideoDecoder *iface, HANDLE *handle)
{ (void)iface; if (!handle) return E_POINTER; *handle = NULL; return S_OK; }

static const ID3D11VideoDecoderVtbl decoder_vtbl = {
    decoder_query, decoder_add_ref, decoder_release, decoder_get_device,
    child_get_private_decoder, child_set_private_decoder, child_set_private_iface_decoder,
    decoder_creation, decoder_driver_handle
};

static HRESULT STDMETHODCALLTYPE view_query(ID3D11VideoDecoderOutputView *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (guid_is(iid, &IID_IUnknown) || guid_is(iid, &IID_ID3D11DeviceChild) ||
        guid_is(iid, &IID_ID3D11View) || guid_is(iid, &IID_ID3D11VideoDecoderOutputView)) {
        *out = iface;
        ID3D11VideoDecoderOutputView_AddRef(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE view_add_ref(ID3D11VideoDecoderOutputView *iface)
{ return InterlockedIncrement(&((BridgeOutputView *)iface)->refs); }

static ULONG STDMETHODCALLTYPE view_release(ID3D11VideoDecoderOutputView *iface)
{
    BridgeOutputView *view = (BridgeOutputView *)iface;
    LONG refs = InterlockedDecrement(&view->refs);
    if (!refs) {
        ID3D11Resource_Release(view->resource);
        ID3D11Device_Release(view->device);
        free(view);
    }
    return refs;
}

static void STDMETHODCALLTYPE view_get_device(ID3D11VideoDecoderOutputView *iface, ID3D11Device **out)
{
    BridgeOutputView *view = (BridgeOutputView *)iface;
    if (out) { *out = view->device; ID3D11Device_AddRef(*out); }
}

static HRESULT STDMETHODCALLTYPE child_get_private_view(ID3D11VideoDecoderOutputView *iface, REFGUID guid, UINT *size, void *data)
{ (void)iface; (void)guid; (void)size; (void)data; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE child_set_private_view(ID3D11VideoDecoderOutputView *iface, REFGUID guid, UINT size, const void *data)
{ (void)iface; (void)guid; (void)size; (void)data; return S_OK; }
static HRESULT STDMETHODCALLTYPE child_set_private_iface_view(ID3D11VideoDecoderOutputView *iface, REFGUID guid, const IUnknown *data)
{ (void)iface; (void)guid; (void)data; return S_OK; }
static void STDMETHODCALLTYPE view_get_resource(ID3D11VideoDecoderOutputView *iface, ID3D11Resource **out)
{
    BridgeOutputView *view = (BridgeOutputView *)iface;
    if (out) { *out = view->resource; ID3D11Resource_AddRef(*out); }
}
static void STDMETHODCALLTYPE view_get_desc(ID3D11VideoDecoderOutputView *iface, D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *desc)
{ if (desc) *desc = ((BridgeOutputView *)iface)->desc; }

static const ID3D11VideoDecoderOutputViewVtbl view_vtbl = {
    view_query, view_add_ref, view_release, view_get_device,
    child_get_private_view, child_set_private_view, child_set_private_iface_view,
    view_get_resource, view_get_desc
};

static HRESULT STDMETHODCALLTYPE video_create_decoder(ID3D11VideoDevice *iface,
    const D3D11_VIDEO_DECODER_DESC *desc, const D3D11_VIDEO_DECODER_CONFIG *config,
    ID3D11VideoDecoder **out)
{
    BridgeDecoder *decoder;
    DxvaRuntime *runtime = runtime_from_video_device(iface);
    if (!runtime || !desc || !config || !out) return E_INVALIDARG;
    *out = NULL;
    if (!guid_is(&desc->Guid, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT) ||
        desc->OutputFormat != DXGI_FORMAT_NV12 ||
        !decoder_dimensions_supported(desc)) return E_INVALIDARG;
    decoder = calloc(1, sizeof(*decoder));
    if (!decoder) return E_OUTOFMEMORY;
    decoder->iface.lpVtbl = (ID3D11VideoDecoderVtbl *)&decoder_vtbl;
    decoder->refs = 1;
    decoder->device = runtime->device;
    ID3D11Device_AddRef(decoder->device);
    decoder->desc = *desc;
    decoder->config = *config;
    decoder->runtime = runtime;
    decoder->helper = INVALID_SOCKET;
    InitializeCriticalSection(&decoder->lock);
    *out = &decoder->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE video_create_output(ID3D11VideoDevice *iface, ID3D11Resource *resource,
    const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *desc, ID3D11VideoDecoderOutputView **out)
{
    BridgeOutputView *view;
    DxvaRuntime *runtime = runtime_from_video_device(iface);
    if (!runtime || !resource || !desc || !out || desc->ViewDimension != D3D11_VDOV_DIMENSION_TEXTURE2D) return E_INVALIDARG;
    *out = NULL;
    view = calloc(1, sizeof(*view));
    if (!view) return E_OUTOFMEMORY;
    view->iface.lpVtbl = (ID3D11VideoDecoderOutputViewVtbl *)&view_vtbl;
    view->refs = 1;
    view->device = runtime->device;
    ID3D11Device_AddRef(view->device);
    view->resource = resource;
    ID3D11Resource_AddRef(resource);
    view->desc = *desc;
    *out = &view->iface;
    return S_OK;
}

static UINT STDMETHODCALLTYPE video_profile_count(ID3D11VideoDevice *iface)
{
    (void)iface;
    /* UU's software decoder is faster than the synchronous Wine NV12 bridge. */
    return 0;
}
static HRESULT STDMETHODCALLTYPE video_profile(ID3D11VideoDevice *iface, UINT index, GUID *profile)
{
    (void)iface;
    if (!profile) return E_POINTER;
    if (index) return E_INVALIDARG;
    *profile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE video_check_format(ID3D11VideoDevice *iface, const GUID *profile,
    DXGI_FORMAT format, BOOL *supported)
{
    (void)iface;
    if (!profile || !supported) return E_POINTER;
    *supported = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE video_config_count(ID3D11VideoDevice *iface,
    const D3D11_VIDEO_DECODER_DESC *desc, UINT *count)
{
    (void)iface;
    if (!desc || !count) return E_POINTER;
    *count = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE video_config(ID3D11VideoDevice *iface,
    const D3D11_VIDEO_DECODER_DESC *desc, UINT index, D3D11_VIDEO_DECODER_CONFIG *config)
{
    (void)iface;
    if (!desc || !config) return E_POINTER;
    if (index || !guid_is(&desc->Guid, &D3D11_DECODER_PROFILE_H264_VLD_NOFGT) ||
        desc->OutputFormat != DXGI_FORMAT_NV12 ||
        !decoder_dimensions_supported(desc)) return E_INVALIDARG;
    memset(config, 0, sizeof(*config));
    config->ConfigBitstreamRaw = 2;
    config->ConfigMinRenderTargetBuffCount = 8;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_get_buffer(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder_iface, D3D11_VIDEO_DECODER_BUFFER_TYPE type,
    UINT *size, void **data)
{
    static const UINT capacities[DXVA_BUFFER_COUNT] = {
        4096, 4096, 4096, 4096, 4096, 1024 * 1024, 16 * 1024 * 1024, 4096, 4096
    };
    BridgeDecoder *decoder = (BridgeDecoder *)decoder_iface;
    DecoderBuffer *buffer;
    (void)iface;
    if (!decoder_iface || decoder_iface->lpVtbl != &decoder_vtbl || !size || !data ||
        (UINT)type >= DXVA_BUFFER_COUNT) return E_INVALIDARG;
    buffer = &decoder->buffers[type];
    if (!buffer->data) {
        buffer->capacity = capacities[type];
        buffer->data = malloc(buffer->capacity);
        if (!buffer->data) return E_OUTOFMEMORY;
    }
    buffer->offset = 0;
    buffer->size = 0;
    *size = buffer->capacity;
    *data = buffer->data;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_release_buffer(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type)
{ (void)iface; (void)decoder; (void)type; return S_OK; }

static HRESULT STDMETHODCALLTYPE context_begin_frame(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder_iface, ID3D11VideoDecoderOutputView *view,
    UINT key_size, const void *key)
{
    BridgeDecoder *decoder = (BridgeDecoder *)decoder_iface;
    (void)iface; (void)key_size; (void)key;
    if (!decoder_iface || decoder_iface->lpVtbl != &decoder_vtbl || !view || view->lpVtbl != &view_vtbl)
        return E_INVALIDARG;
    if (decoder->output) ID3D11VideoDecoderOutputView_Release(decoder->output);
    decoder->output = view;
    ID3D11VideoDecoderOutputView_AddRef(view);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE context_submit(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder_iface, UINT count, const D3D11_VIDEO_DECODER_BUFFER_DESC *descs)
{
    BridgeDecoder *decoder = (BridgeDecoder *)decoder_iface;
    UINT i;
    (void)iface;
    if (!decoder_iface || decoder_iface->lpVtbl != &decoder_vtbl || (count && !descs)) return E_INVALIDARG;
    for (i = 0; i < count; ++i) {
        UINT type = descs[i].BufferType;
        DecoderBuffer *buffer;
        if (type >= DXVA_BUFFER_COUNT) continue;
        buffer = &decoder->buffers[type];
        if (!buffer->data || descs[i].DataOffset > buffer->capacity ||
            descs[i].DataSize > buffer->capacity - descs[i].DataOffset) return E_INVALIDARG;
        buffer->offset = descs[i].DataOffset;
        buffer->size = descs[i].DataSize;
    }
    return S_OK;
}

static HRESULT upload_nv12(BridgeDecoder *decoder, const uint8_t *data,
    UINT width, UINT height)
{
    BridgeOutputView *view;
    ID3D11Texture2D *target = NULL;
    D3D11_TEXTURE2D_DESC desc;
    uint8_t *upload_data;
    uint64_t upload_size;
    UINT subresource;
    UINT output_width;
    UINT output_height;
    UINT y;
    HRESULT hr;
    if (!decoder->output || decoder->output->lpVtbl != &view_vtbl) return E_FAIL;
    view = (BridgeOutputView *)decoder->output;
    hr = ID3D11Resource_QueryInterface(view->resource, &IID_ID3D11Texture2D, (void **)&target);
    if (FAILED(hr)) return hr;
    ID3D11Texture2D_GetDesc(target, &desc);
    if (desc.Format != DXGI_FORMAT_NV12 || !width || !height || !desc.Width || !desc.Height) {
        hr = E_INVALIDARG;
        goto done;
    }
    output_width = width;
    output_height = height;
    if (output_width > desc.Width) {
        output_width = desc.Width;
        output_height = (UINT)((uint64_t)height * output_width / width);
    }
    if (output_height > desc.Height) {
        output_height = desc.Height;
        output_width = (UINT)((uint64_t)width * output_height / height);
    }
    output_width &= ~1u;
    output_height &= ~1u;
    if (!output_width || !output_height) {
        hr = E_INVALIDARG;
        goto done;
    }
    subresource = view->desc.Texture2D.ArraySlice;
    if (output_width == width && output_height == height &&
        width == desc.Width && height == desc.Height) {
        upload_data = (uint8_t *)data;
    } else {
        upload_size = (uint64_t)desc.Width * desc.Height * 3 / 2;
        if (upload_size > UINT_MAX) {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        if (upload_size > decoder->upload_capacity) {
            uint8_t *upload = realloc(decoder->upload, (size_t)upload_size);
            if (!upload) {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            decoder->upload = upload;
            decoder->upload_capacity = (UINT)upload_size;
        }
        upload_data = decoder->upload;
        memset(upload_data, 0, (size_t)upload_size);
    }
    if (upload_data != data && output_width == width && output_height == height) {
        for (y = 0; y < height; ++y)
            memcpy(upload_data + (size_t)y * desc.Width,
                   data + (size_t)y * width, width);
        for (y = 0; y < height / 2; ++y)
            memcpy(upload_data + (size_t)(desc.Height + y) * desc.Width,
                   data + (size_t)width * height + (size_t)y * width, width);
    } else if (upload_data != data) {
        UINT x;
        UINT source_pairs = width / 2;
        UINT output_pairs = output_width / 2;
        uint64_t luma_step = ((uint64_t)width << 32) / output_width;
        uint64_t chroma_step = ((uint64_t)source_pairs << 32) / output_pairs;
        for (y = 0; y < output_height; ++y) {
            const uint8_t *source = data +
                (size_t)((uint64_t)y * height / output_height) * width;
            uint8_t *target_row = upload_data + (size_t)y * desc.Width;
            uint64_t source_x = 0;
            for (x = 0; x < output_width; ++x) {
                target_row[x] = source[source_x >> 32];
                source_x += luma_step;
            }
        }
        for (y = 0; y < output_height / 2; ++y) {
            const uint8_t *source = data + (size_t)width * height +
                (size_t)((uint64_t)y * (height / 2) / (output_height / 2)) * width;
            uint8_t *target_row = upload_data + (size_t)(desc.Height + y) * desc.Width;
            uint64_t source_x = 0;
            for (x = 0; x < output_pairs; ++x) {
                UINT source_pair = (UINT)(source_x >> 32);
                target_row[x * 2] = source[source_pair * 2];
                target_row[x * 2 + 1] = source[source_pair * 2 + 1];
                source_x += chroma_step;
            }
        }
    }
    ID3D11DeviceContext_UpdateSubresource(decoder->runtime->context,
        (ID3D11Resource *)target, subresource, NULL, upload_data,
        desc.Width, desc.Width * desc.Height);
    ID3D11DeviceContext_Flush(decoder->runtime->context);
    hr = S_OK;
done:
    if (target) ID3D11Texture2D_Release(target);
    return hr;
}

static HRESULT STDMETHODCALLTYPE context_end_frame(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder_iface)
{
    BridgeDecoder *decoder = (BridgeDecoder *)decoder_iface;
    DecoderBuffer *bitstream;
    HelperFrameMessage packet;
    HelperReply reply;
    uint8_t parameter_sets[600];
    UINT parameter_size;
    HRESULT hr = S_OK;
    (void)iface;
    if (!decoder_iface || decoder_iface->lpVtbl != &decoder_vtbl) return E_INVALIDARG;
    bitstream = &decoder->buffers[D3D11_VIDEO_DECODER_BUFFER_BITSTREAM];
    if (!bitstream->data || !bitstream->size) return S_OK;
    if (decoder->retry_after && GetTickCount64() < decoder->retry_after) return S_OK;
    EnterCriticalSection(&decoder->lock);
    if (!decoder_connect(decoder)) { hr = E_FAIL; goto done; }
    parameter_size = build_h264_parameter_sets(decoder, parameter_sets);
    memset(&packet, 0, sizeof(packet));
    packet.magic = HELPER_MAGIC;
    packet.type = HELPER_DECODE_PACKET;
    packet.payload_size = parameter_size + bitstream->size;
    if (!send_all(decoder->helper, &packet, sizeof(packet)) ||
        (parameter_size && !send_all(decoder->helper, parameter_sets, parameter_size)) ||
        !send_all(decoder->helper, bitstream->data + bitstream->offset, bitstream->size) ||
        !recv_all(decoder->helper, &reply, sizeof(reply)) ||
        reply.magic != HELPER_MAGIC || reply.type != HELPER_REPLY || reply.status) {
        closesocket(decoder->helper);
        decoder->helper = INVALID_SOCKET;
        decoder_backoff(decoder);
        hr = E_FAIL;
        goto done;
    }
    if (reply.payload_size) {
        uint64_t expected = (uint64_t)reply.width * reply.height * 3 / 2;
        if (expected != reply.payload_size || reply.payload_size > 128u * 1024u * 1024u) {
            hr = E_FAIL;
            goto disconnect;
        }
        if (reply.payload_size > decoder->frame_capacity) {
            uint8_t *frame = realloc(decoder->frame, reply.payload_size);
            if (!frame) { hr = E_OUTOFMEMORY; goto done; }
            decoder->frame = frame;
            decoder->frame_capacity = reply.payload_size;
        }
        if (!recv_all(decoder->helper, decoder->frame, reply.payload_size)) {
            hr = E_FAIL;
            goto disconnect;
        }
        hr = upload_nv12(decoder, decoder->frame, reply.width, reply.height);
        if (FAILED(hr)) goto disconnect;
    }
    goto done;
disconnect:
    closesocket(decoder->helper);
    decoder->helper = INVALID_SOCKET;
    decoder_backoff(decoder);
done:
    LeaveCriticalSection(&decoder->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE context_extension(ID3D11VideoContext *iface,
    ID3D11VideoDecoder *decoder, const D3D11_VIDEO_DECODER_EXTENSION *extension)
{ (void)iface; (void)decoder; (void)extension; return E_NOTIMPL; }

static void patch_video_device(DxvaRuntime *runtime, ID3D11VideoDevice *video)
{
    if (!video) return;
    if (video->lpVtbl != &runtime->video_device_vtbl) {
        runtime->video_device_vtbl = *video->lpVtbl;
        runtime->video_device_vtbl.CreateVideoDecoder = video_create_decoder;
        runtime->video_device_vtbl.CreateVideoDecoderOutputView = video_create_output;
        runtime->video_device_vtbl.GetVideoDecoderProfileCount = video_profile_count;
        runtime->video_device_vtbl.GetVideoDecoderProfile = video_profile;
        runtime->video_device_vtbl.CheckVideoDecoderFormat = video_check_format;
        runtime->video_device_vtbl.GetVideoDecoderConfigCount = video_config_count;
        runtime->video_device_vtbl.GetVideoDecoderConfig = video_config;
        InterlockedExchangePointer((void *volatile *)&video->lpVtbl, &runtime->video_device_vtbl);
    }
}

static void patch_video_context(DxvaRuntime *runtime, ID3D11VideoContext *video)
{
    if (!video) return;
    if (video->lpVtbl != &runtime->video_context_vtbl) {
        runtime->video_context_vtbl = *video->lpVtbl;
        runtime->video_context_vtbl.GetDecoderBuffer = context_get_buffer;
        runtime->video_context_vtbl.ReleaseDecoderBuffer = context_release_buffer;
        runtime->video_context_vtbl.DecoderBeginFrame = context_begin_frame;
        runtime->video_context_vtbl.DecoderEndFrame = context_end_frame;
        runtime->video_context_vtbl.SubmitDecoderBuffers = context_submit;
        runtime->video_context_vtbl.DecoderExtension = context_extension;
        InterlockedExchangePointer((void *volatile *)&video->lpVtbl, &runtime->video_context_vtbl);
    }
}

static HRESULT STDMETHODCALLTYPE device_query_hook(ID3D11Device *device, REFIID iid, void **out)
{
    DxvaRuntime *runtime = runtime_from_device(device);
    HRESULT hr;
    if (!runtime) return E_NOINTERFACE;
    hr = runtime->device_query(device, iid, out);
    if (SUCCEEDED(hr) && out && guid_is(iid, &IID_ID3D11VideoDevice))
        patch_video_device(runtime, (ID3D11VideoDevice *)*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE context_query_hook(ID3D11DeviceContext *context, REFIID iid, void **out)
{
    DxvaRuntime *runtime = runtime_from_context(context);
    HRESULT hr;
    if (!runtime) return E_NOINTERFACE;
    hr = runtime->context_query(context, iid, out);
    if (SUCCEEDED(hr) && out && guid_is(iid, &IID_ID3D11VideoContext))
        patch_video_context(runtime, (ID3D11VideoContext *)*out);
    return hr;
}

HRESULT dxva_bridge_install(ID3D11Device *device, ID3D11DeviceContext *context)
{
    HMODULE module;
    WSADATA winsock_data;
    DxvaRuntime *runtime;
    ID3D11VideoDevice *video_device = NULL;
    ID3D11VideoContext *video_context = NULL;
    LONG i;
    HRESULT result = S_OK;
    if (!device || !context) return E_INVALIDARG;
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) return E_FAIL;
    while (InterlockedCompareExchange(&runtime_lock, 1, 0)) Sleep(0);
    for (i = 0; i < runtime_count; ++i) {
        if (runtimes[i].device == device) goto done;
    }
    if (runtime_count == DXVA_RUNTIME_COUNT) {
        result = E_OUTOFMEMORY;
        goto done;
    }
    if (!module_pinned && !GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCSTR)(uintptr_t)&dxva_bridge_install, &module)) {
        result = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    module_pinned = 1;
    runtime = &runtimes[runtime_count++];
    memset(runtime, 0, sizeof(*runtime));
    runtime->device = device;
    runtime->context = context;
    ID3D11Device_AddRef(device);
    ID3D11DeviceContext_AddRef(context);
    runtime->device_vtbl = *device->lpVtbl;
    runtime->context_vtbl = *context->lpVtbl;
    runtime->device_query = device->lpVtbl->QueryInterface;
    runtime->context_query = context->lpVtbl->QueryInterface;
    runtime->device_vtbl.QueryInterface = device_query_hook;
    runtime->context_vtbl.QueryInterface = context_query_hook;
    InterlockedExchangePointer((void *volatile *)&device->lpVtbl, &runtime->device_vtbl);
    InterlockedExchangePointer((void *volatile *)&context->lpVtbl, &runtime->context_vtbl);
    if (SUCCEEDED(runtime->device_query(device, &IID_ID3D11VideoDevice, (void **)&video_device))) {
        patch_video_device(runtime, video_device);
        ID3D11VideoDevice_Release(video_device);
    }
    if (SUCCEEDED(runtime->context_query(context, &IID_ID3D11VideoContext, (void **)&video_context))) {
        patch_video_context(runtime, video_context);
        ID3D11VideoContext_Release(video_context);
    }
done:
    InterlockedExchange(&runtime_lock, 0);
    return result;
}
