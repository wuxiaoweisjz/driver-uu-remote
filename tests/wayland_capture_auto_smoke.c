#include <windows.h>

typedef BOOL (WINAPI *RunCaptureFn)(void);
typedef BOOL (WINAPI *RunInputFn)(void);

static void print_message(const char *message)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, lstrlenA(message),
              &written, NULL);
}

void mainCRTStartup(void)
{
    INPUT direct_input;
    char stress[2];
    unsigned int iterations = GetEnvironmentVariableA(
        "UU_WAYLAND_CAPTURE_STRESS", stress, sizeof(stress)) > 0 ? 120 : 1;
    unsigned int index;
    HMODULE streamer = LoadLibraryA("streamer.dll");
    RunCaptureFn capture = streamer ? (RunCaptureFn)(void *)GetProcAddress(
        streamer, "RunCapture") : NULL;
    RunInputFn input = streamer ? (RunInputFn)(void *)GetProcAddress(
        streamer, "RunInput") : NULL;

    Sleep(250);
    if (!capture || !input) {
        print_message("automatic Wayland BitBlt bridge failed\r\n");
        ExitProcess(1);
    }
    if (!input()) {
        print_message("automatic Wayland input bridge failed\r\n");
        ExitProcess(1);
    }
    ZeroMemory(&direct_input, sizeof(direct_input));
    direct_input.type = INPUT_MOUSE;
    direct_input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &direct_input, sizeof(direct_input)) != 1) {
        print_message("automatic Wayland process input bridge failed\r\n");
        ExitProcess(1);
    }
    for (index = 0; index < iterations; ++index) {
        if (!capture()) {
            print_message("automatic Wayland BitBlt bridge failed\r\n");
            ExitProcess(1);
        }
    }
    print_message("automatic Wayland BitBlt bridge passed\r\n");
    ExitProcess(0);
}
