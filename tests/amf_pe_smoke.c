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
    D3D11_VIDEO_DECODER_DESC decoder_desc;
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
        ID3D11VideoDevice_GetVideoDecoderProfileCount(second_video_device) != 0)
        FAIL("Second D3D11 device exposed disabled DXVA11 bridge", 32);
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
    if (ID3D11VideoDevice_GetVideoDecoderProfileCount(video_device) != 0 ||
        FAILED(ID3D11VideoDevice_CheckVideoDecoderFormat(video_device,
            &D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &supported)) || supported)
        FAIL("Unstable DXVA11 decode path was exposed", 24);
    ZeroMemory(&decoder_desc, sizeof(decoder_desc));
    decoder_desc.Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    decoder_desc.SampleWidth = 2560;
    decoder_desc.SampleHeight = 1440;
    decoder_desc.OutputFormat = DXGI_FORMAT_NV12;
    config_count = 0;
    if (FAILED(ID3D11VideoDevice_GetVideoDecoderConfigCount(video_device,
            &decoder_desc, &config_count)) || config_count != 0)
        FAIL("Unstable DXVA11 decoder config was exposed", 25);
    write_text("DXVA11 decode disabled; UU software fallback selected\r\n");
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
    ID3D11VideoContext_Release(video_context);
    ID3D11VideoDevice_Release(video_device);
    finish(output, surface, encoder, context, immediate, device, module, 0);
#undef FAIL
}
