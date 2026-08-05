#include <windows.h>

static int parse_coordinate(WCHAR **cursor, int *value)
{
    unsigned int parsed = 0;
    WCHAR *text = *cursor;

    while (*text == L' ') ++text;
    if (*text < L'0' || *text > L'9') return 0;
    while (*text >= L'0' && *text <= L'9') {
        if (parsed > 1000000U) return 0;
        parsed = parsed * 10U + (unsigned int)(*text - L'0');
        ++text;
    }
    *cursor = text;
    *value = (int)parsed;
    return 1;
}

void mainCRTStartup(void)
{
    INPUT input[2];
    WCHAR *cursor = GetCommandLineW();
    int x;
    int y;

    while (*cursor && *cursor != L' ') ++cursor;
    if (!parse_coordinate(&cursor, &x) || !parse_coordinate(&cursor, &y))
        ExitProcess(2);

    if (!SetCursorPos(x, y)) ExitProcess(3);
    ZeroMemory(input, sizeof(input));
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ExitProcess(SendInput(2, input, sizeof(input[0])) == 2 ? 0 : 4);
}
