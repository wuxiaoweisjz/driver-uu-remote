#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

#include "core/Factory.h"
#include "components/VideoEncoderVCE.h"

static void write_text(const char *text)
{
    DWORD written;
    DWORD length = 0;
    while (text[length]) ++length;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}

static void finish(AMFData *output, AMFSurface *surface, AMFComponent *encoder,
                   AMFContext *context, ID3D11DeviceContext *immediate,
                   ID3D11Device *device, HMODULE module, UINT status)
{
    if (output) output->pVtbl->Release(output);
    if (surface) surface->pVtbl->Release(surface);
    if (encoder) encoder->pVtbl->Release(encoder);
    if (context) context->pVtbl->Release(context);
    if (immediate) ID3D11DeviceContext_Release(immediate);
    if (device) ID3D11Device_Release(device);
    if (module) FreeLibrary(module);
    ExitProcess(status);
}

void mainCRTStartup(void)
{
    HMODULE module = NULL;
    AMFInit_Fn init;
    AMFQueryVersion_Fn query_version;
    amf_uint64 version = 0;
    AMFFactory *factory = NULL;
    AMFContext *context = NULL;
    AMFComponent *encoder = NULL;
    AMFSurface *surface = NULL;
    AMFData *output = NULL;
    AMFVariantStruct value;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *immediate = NULL;
    ID3D11Device *second_device = NULL;
    ID3D11DeviceContext *second_context = NULL;
    ID3D11VideoDevice *second_video_device = NULL;
    ID3D11VideoDevice *video_device = NULL;
    ID3D11VideoContext *video_context = NULL;
    ID3D11VideoDecoder *decoder = NULL;
    ID3D11VideoDecoderOutputView *decoder_view = NULL;
    ID3D11Texture2D *decode_texture = NULL;
    D3D11_VIDEO_DECODER_DESC decoder_desc;
    D3D11_VIDEO_DECODER_CONFIG decoder_config;
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view_desc;
    D3D11_VIDEO_DECODER_BUFFER_DESC buffer_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    HANDLE bitstream_file = INVALID_HANDLE_VALUE;
    void *decoder_buffer = NULL;
    UINT decoder_buffer_size = 0;
    DWORD bitstream_size;
    DWORD bytes_read;
    UINT config_count = 0;
    BOOL supported = FALSE;
    D3D_FEATURE_LEVEL feature_level;
    AMF_RESULT result;
    int attempt;

#define FAIL(message, code) do { write_text(message "\r\n"); finish(output, surface, encoder, context, immediate, device, module, code); } while (0)
    module = LoadLibraryW(L"amfrt64.dll");
    if (!module) FAIL("LoadLibrary failed", 10);
    init = (AMFInit_Fn)(void *)GetProcAddress(module, AMF_INIT_FUNCTION_NAME);
    query_version = (AMFQueryVersion_Fn)(void *)GetProcAddress(module, AMF_QUERY_VERSION_FUNCTION_NAME);
    if (!init || !query_version) FAIL("AMF exports missing", 11);
    if (query_version(&version) != AMF_OK || init(AMF_FULL_VERSION, &factory) != AMF_OK)
        FAIL("AMF factory failed", 12);
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &device, &feature_level, &immediate)))
        FAIL("D3D11CreateDevice failed", 13);
    if (factory->pVtbl->CreateContext(factory, &context) != AMF_OK)
        FAIL("CreateContext failed", 14);
    if (context->pVtbl->InitDX11(context, device, AMF_DX11_0) != AMF_OK)
        FAIL("InitDX11 failed", 15);
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &second_device, &feature_level,
                                 &second_context)) ||
        FAILED(ID3D11Device_QueryInterface(second_device, &IID_ID3D11VideoDevice,
                                           (void **)&second_video_device)) ||
        ID3D11VideoDevice_GetVideoDecoderProfileCount(second_video_device) != 1)
        FAIL("Second D3D11 device DXVA11 bridge failed", 32);
    ID3D11VideoDevice_Release(second_video_device);
    ID3D11DeviceContext_Release(second_context);
    ID3D11Device_Release(second_device);
    second_video_device = NULL;
    second_context = NULL;
    second_device = NULL;
    if (FAILED(ID3D11Device_QueryInterface(device, &IID_ID3D11VideoDevice,
                                           (void **)&video_device)) ||
        FAILED(ID3D11DeviceContext_QueryInterface(immediate, &IID_ID3D11VideoContext,
                                                  (void **)&video_context)))
        FAIL("DXVA11 interfaces missing", 23);
    if (ID3D11VideoDevice_GetVideoDecoderProfileCount(video_device) != 1 ||
        FAILED(ID3D11VideoDevice_CheckVideoDecoderFormat(video_device,
            &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &supported)) || !supported)
        FAIL("DXVA11 H.264 NV12 capability missing", 24);
    ZeroMemory(&decoder_desc, sizeof(decoder_desc));
    decoder_desc.Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    decoder_desc.SampleWidth = 1920;
    decoder_desc.SampleHeight = 1080;
    decoder_desc.OutputFormat = DXGI_FORMAT_NV12;
    if (FAILED(ID3D11VideoDevice_GetVideoDecoderConfigCount(video_device,
            &decoder_desc, &config_count)) || config_count != 1 ||
        FAILED(ID3D11VideoDevice_GetVideoDecoderConfig(video_device,
            &decoder_desc, 0, &decoder_config)) ||
        FAILED(ID3D11VideoDevice_CreateVideoDecoder(video_device,
            &decoder_desc, &decoder_config, &decoder)))
        FAIL("DXVA11 decoder creation failed", 25);
    ZeroMemory(&texture_desc, sizeof(texture_desc));
    texture_desc.Width = 1920;
    texture_desc.Height = 1080;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_NV12;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &texture_desc, NULL, &decode_texture)))
        FAIL("DXVA11 output texture creation failed", 26);
    ZeroMemory(&view_desc, sizeof(view_desc));
    view_desc.DecodeProfile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    view_desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11VideoDevice_CreateVideoDecoderOutputView(video_device,
            (ID3D11Resource *)decode_texture, &view_desc, &decoder_view)))
        FAIL("DXVA11 output view creation failed", 27);
    bitstream_file = CreateFileW(L"Z:\\tmp\\uu-amf-decode-smoke.h264", GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (bitstream_file == INVALID_HANDLE_VALUE ||
        (bitstream_size = GetFileSize(bitstream_file, NULL)) == INVALID_FILE_SIZE || !bitstream_size) {
        if (bitstream_file != INVALID_HANDLE_VALUE) CloseHandle(bitstream_file);
        bitstream_file = INVALID_HANDLE_VALUE;
        write_text("DXVA11 capability passed; decode sample not provided\r\n");
    } else {
        if (FAILED(ID3D11VideoContext_GetDecoderBuffer(video_context, decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &decoder_buffer_size, &decoder_buffer)) ||
            bitstream_size > decoder_buffer_size ||
            !ReadFile(bitstream_file, decoder_buffer, bitstream_size, &bytes_read, NULL) ||
            bytes_read != bitstream_size)
            FAIL("DXVA11 decoder buffer failed", 29);
        CloseHandle(bitstream_file);
        bitstream_file = INVALID_HANDLE_VALUE;
        if (FAILED(ID3D11VideoContext_ReleaseDecoderBuffer(video_context, decoder,
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM)) ||
            FAILED(ID3D11VideoContext_DecoderBeginFrame(video_context, decoder, decoder_view, 0, NULL)))
            FAIL("DXVA11 BeginFrame failed", 30);
        ZeroMemory(&buffer_desc, sizeof(buffer_desc));
        buffer_desc.BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        buffer_desc.DataSize = bitstream_size;
        if (FAILED(ID3D11VideoContext_SubmitDecoderBuffers(video_context, decoder, 1, &buffer_desc)) ||
            FAILED(ID3D11VideoContext_DecoderEndFrame(video_context, decoder)))
            FAIL("DXVA11 decode submission failed", 31);
        write_text("DXVA11 Vulkan H.264 decode path passed\r\n");
    }
    if (factory->pVtbl->CreateComponent(factory, context, AMFVideoEncoderVCE_AVC, &encoder) != AMF_OK)
        FAIL("CreateComponent failed", 16);

    AMFVariantInit(&value);
    value.type = AMF_VARIANT_INT64;
    value.int64Value = 8000000;
    if (encoder->pVtbl->SetProperty(encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, value) != AMF_OK)
        FAIL("SetProperty failed", 17);
    result = encoder->pVtbl->Init(encoder, AMF_SURFACE_NV12, 1920, 1080);
    if (result != AMF_OK) FAIL("Encoder Init failed", 18);
    result = context->pVtbl->AllocSurface(context, AMF_MEMORY_DX11, AMF_SURFACE_NV12,
                                          1920, 1080, &surface);
    if (result != AMF_OK) FAIL("AllocSurface failed", 19);
    surface->pVtbl->SetPts(surface, 1);
    surface->pVtbl->SetDuration(surface, 333333);
    if (encoder->pVtbl->SubmitInput(encoder, (AMFData *)surface) != AMF_OK)
        FAIL("SubmitInput failed", 20);
    for (attempt = 0; attempt < 20; ++attempt) {
        result = encoder->pVtbl->QueryOutput(encoder, &output);
        if (result == AMF_OK) break;
        if (result != AMF_REPEAT) FAIL("QueryOutput failed", 21);
        Sleep(10);
    }
    if (!output || ((AMFBuffer *)output)->pVtbl->GetSize((AMFBuffer *)output) == 0)
        FAIL("Encoder returned no packet", 22);
    write_text("Encoded H.264 packet: non-zero bytes\r\n");
    ID3D11VideoDecoderOutputView_Release(decoder_view);
    ID3D11Texture2D_Release(decode_texture);
    ID3D11VideoDecoder_Release(decoder);
    ID3D11VideoContext_Release(video_context);
    ID3D11VideoDevice_Release(video_device);
    finish(output, surface, encoder, context, immediate, device, module, 0);
#undef FAIL
}
