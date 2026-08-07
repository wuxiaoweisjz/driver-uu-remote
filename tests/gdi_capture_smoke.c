#include <windows.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

static void print_result(const char *message, DWORD value)
{
    char buffer[160];
    DWORD written;
    int length = wsprintfA(buffer, message, value);

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, (DWORD)length, &written, NULL);
}

void mainCRTStartup(void)
{
    char bridge_test[2];
    HDC desktop = GetDC(NULL);
    HDC memory = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous = NULL;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    DWORD error = ERROR_SUCCESS;
    UINT exit_code = 1;

    if (GetEnvironmentVariableA("UU_WAYLAND_CAPTURE_TEST", bridge_test,
                                sizeof(bridge_test)) > 0) {
        HMODULE proxy = LoadLibraryA("d3d11.dll");
        BOOL (WINAPI *install_hook)(void) = proxy ? (void *)GetProcAddress(
            proxy, "UUInstallWaylandCaptureHook") : NULL;
        if (!install_hook || !install_hook()) {
            print_result("Wayland capture hook installation failed, error=%lu\r\n",
                         GetLastError());
            ExitProcess(2);
        }
    }

    if (!desktop || width <= 0 || height <= 0) {
        error = GetLastError();
        print_result("desktop DC unavailable, error=%lu\r\n", error);
        goto done;
    }
    memory = CreateCompatibleDC(desktop);
    bitmap = CreateCompatibleBitmap(desktop, width, height);
    if (!memory || !bitmap) {
        error = GetLastError();
        print_result("capture allocation failed, error=%lu\r\n", error);
        goto done;
    }
    previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        error = GetLastError();
        print_result("capture bitmap selection failed, error=%lu\r\n", error);
        goto done;
    }
    if (!BitBlt(memory, 0, 0, width, height, desktop, 0, 0, SRCCOPY | CAPTUREBLT)) {
        error = GetLastError();
        print_result("desktop BitBlt failed, error=%lu\r\n", error);
        goto done;
    }
    print_result("desktop BitBlt passed, width=%lu\r\n", (DWORD)width);
    exit_code = 0;

done:
    if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (desktop) ReleaseDC(NULL, desktop);
    ExitProcess(exit_code);
}
