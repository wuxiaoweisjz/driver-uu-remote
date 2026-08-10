#include <windows.h>
#include <d3d11.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

static BOOL close_to(BYTE value, BYTE expected)
{
    int difference = (int)value - (int)expected;
    return difference >= -8 && difference <= 8;
}

static BOOL expected_pixel(const BYTE *pixel, BYTE blue, BYTE green, BYTE red)
{
    return close_to(pixel[0], blue) && close_to(pixel[1], green) &&
           close_to(pixel[2], red);
}

static BOOL run_capture(int width, int height, BOOL verify_pattern)
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
        if (result && verify_pattern) {
            const BYTE *top_left = pixels + ((SIZE_T)height / 8 * width + width / 8) * 4u;
            const BYTE *top_right = pixels + ((SIZE_T)height / 8 * width + width * 7 / 8) * 4u;
            const BYTE *bottom_left = pixels + ((SIZE_T)height * 7 / 8 * width + width / 8) * 4u;
            const BYTE *bottom_right = pixels + ((SIZE_T)height * 7 / 8 * width + width * 7 / 8) * 4u;
            result = expected_pixel(top_left, 40, 40, 220) &&
                     expected_pixel(top_right, 80, 200, 40) &&
                     expected_pixel(bottom_left, 220, 90, 40) &&
                     expected_pixel(bottom_right, 30, 190, 230);
        }
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, desktop);
    return result;
}

BOOL WINAPI RunCapture(void)
{
    return run_capture(GetSystemMetrics(SM_CXSCREEN),
                       GetSystemMetrics(SM_CYSCREEN), FALSE);
}

BOOL WINAPI RunQualitySwitchCapture(void)
{
    static LONG preset;
    static const int widths[] = {1920, 1280, 960, 1280};
    static const int heights[] = {1080, 720, 540, 720};
    LONG index = InterlockedIncrement(&preset) - 1;
    index %= (LONG)(sizeof(widths) / sizeof(widths[0]));
    return run_capture(widths[index], heights[index], TRUE);
}

BOOL WINAPI RunInput(void)
{
    INPUT input[2];
    ZeroMemory(input, sizeof(input));
    input[0].type = INPUT_MOUSE;
    input[0].mi.dx = 1;
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dx = -1;
    input[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    return SendInput(2, input, sizeof(input[0])) == 2;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
