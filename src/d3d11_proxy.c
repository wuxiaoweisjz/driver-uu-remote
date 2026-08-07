#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "wayland_capture_hook.h"

typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
    D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (STDMETHODCALLTYPE *CreateDeviceAndSwapChainFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
    IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (STDMETHODCALLTYPE *CoreCreateDeviceFn)(
    IDXGIFactory *, IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *);
typedef HRESULT (STDMETHODCALLTYPE *On12CreateDeviceFn)(
    IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT,
    UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef HRESULT (*InstallBridgeFn)(ID3D11Device *, ID3D11DeviceContext *);

static HMODULE dxvk_module;

static DWORD WINAPI wayland_capture_thread(void *opaque)
{
    return uu_wayland_capture_hook_thread(opaque);
}

static FARPROC dxvk_proc(const char *name)
{
    if (!dxvk_module) dxvk_module = LoadLibraryW(L"d3d11_dxvk.dll");
    return dxvk_module ? GetProcAddress(dxvk_module, name) : NULL;
}

static void install_bridge(ID3D11Device *device, ID3D11DeviceContext *context)
{
    HMODULE bridge;
    InstallBridgeFn install;
    ID3D11DeviceContext *temporary = NULL;
    if (!device) return;
    if (!context) {
        ID3D11Device_GetImmediateContext(device, &temporary);
        context = temporary;
    }
    bridge = LoadLibraryW(L"amfrt64.dll");
    install = bridge ? (InstallBridgeFn)(void *)GetProcAddress(bridge, "UUInstallDxvaBridge") : NULL;
    if (install && context) install(device, context);
    if (temporary) ID3D11DeviceContext_Release(temporary);
}

HRESULT STDMETHODCALLTYPE D3D11CreateDevice(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk_version,
    ID3D11Device **device, D3D_FEATURE_LEVEL *selected_level,
    ID3D11DeviceContext **context)
{
    CreateDeviceFn function = (CreateDeviceFn)(void *)dxvk_proc("D3D11CreateDevice");
    HRESULT hr;
    if (!function) return E_FAIL;
    hr = function(adapter, driver_type, software, flags, levels, level_count, sdk_version,
                  device, selected_level, context);
    if (SUCCEEDED(hr)) install_bridge(device ? *device : NULL, context ? *context : NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_desc, IDXGISwapChain **swap_chain,
    ID3D11Device **device, D3D_FEATURE_LEVEL *selected_level,
    ID3D11DeviceContext **context)
{
    CreateDeviceAndSwapChainFn function = (CreateDeviceAndSwapChainFn)(void *)
        dxvk_proc("D3D11CreateDeviceAndSwapChain");
    HRESULT hr;
    if (!function) return E_FAIL;
    hr = function(adapter, driver_type, software, flags, levels, level_count, sdk_version,
                  swap_desc, swap_chain, device, selected_level, context);
    if (SUCCEEDED(hr)) install_bridge(device ? *device : NULL, context ? *context : NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D11CoreCreateDevice(
    IDXGIFactory *factory, IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
    UINT sdk_version, ID3D11Device **device, D3D_FEATURE_LEVEL *selected_level)
{
    CoreCreateDeviceFn function = (CoreCreateDeviceFn)(void *)dxvk_proc("D3D11CoreCreateDevice");
    HRESULT hr;
    if (!function) return E_FAIL;
    hr = function(factory, adapter, driver_type, software, flags, levels, level_count,
                  sdk_version, device, selected_level);
    if (SUCCEEDED(hr)) install_bridge(device ? *device : NULL, NULL);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D11On12CreateDevice(
    IUnknown *device12, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
    IUnknown *const *queues, UINT queue_count, UINT node_mask, ID3D11Device **device,
    ID3D11DeviceContext **context, D3D_FEATURE_LEVEL *selected_level)
{
    On12CreateDeviceFn function = (On12CreateDeviceFn)(void *)dxvk_proc("D3D11On12CreateDevice");
    HRESULT hr;
    if (!function) return E_FAIL;
    hr = function(device12, flags, levels, level_count, queues, queue_count, node_mask,
                  device, context, selected_level);
    if (SUCCEEDED(hr)) install_bridge(device ? *device : NULL, context ? *context : NULL);
    return hr;
}

BOOL WINAPI UUInstallWaylandCaptureHook(void)
{
    return uu_install_wayland_capture_hook(GetModuleHandleW(NULL));
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    WCHAR wayland[2];
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (GetEnvironmentVariableW(L"WAYLAND_DISPLAY", wayland, 2) > 0)
            CloseHandle(CreateThread(NULL, 0, wayland_capture_thread, NULL, 0, NULL));
    }
    return TRUE;
}
