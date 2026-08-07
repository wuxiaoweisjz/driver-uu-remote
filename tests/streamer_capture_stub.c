#include <windows.h>
#include <d3d11.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

BOOL WINAPI RunCapture(void)
{
    HDC desktop;
    HDC memory;
    HBITMAP bitmap;
    HGDIOBJ previous;
    BITMAPINFO information;
    BYTE *pixels = NULL;
    SIZE_T pixel_size;
    SIZE_T index;
    BOOL has_visible_pixel = FALSE;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    BOOL result;

    if (GetTickCount() == ~(DWORD)0)
        D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                          D3D11_SDK_VERSION, NULL, NULL, NULL);
    desktop = GetDC(NULL);
    memory = desktop ? CreateCompatibleDC(desktop) : NULL;
    ZeroMemory(&information, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    bitmap = desktop ? CreateDIBSection(desktop, &information, DIB_RGB_COLORS,
                                        (void **)&pixels, NULL, 0) : NULL;
    if (!desktop || !memory || !bitmap) return FALSE;
    previous = SelectObject(memory, bitmap);
    result = previous && previous != HGDI_ERROR &&
        BitBlt(memory, 0, 0, width, height, desktop, 0, 0, SRCCOPY | CAPTUREBLT);
    if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
    pixel_size = (SIZE_T)width * (SIZE_T)height * 4u;
    if (!pixels) {
        result = FALSE;
    } else {
        for (index = 0; index < pixel_size; index += 4) {
            if (pixels[index] || pixels[index + 1] || pixels[index + 2]) {
                has_visible_pixel = TRUE;
                break;
            }
        }
        result = has_visible_pixel;
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, desktop);
    return result;
}

BOOL WINAPI RunInput(void)
{
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(1, &input, sizeof(input)) == 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
