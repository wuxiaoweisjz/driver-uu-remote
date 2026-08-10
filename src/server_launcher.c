#include <windows.h>
#include <stddef.h>

static WCHAR *skip_program_name(WCHAR *command_line)
{
    if (*command_line == L'"') {
        ++command_line;
        while (*command_line && *command_line != L'"') ++command_line;
        if (*command_line == L'"') ++command_line;
    } else {
        while (*command_line && *command_line != L' ' && *command_line != L'\t')
            ++command_line;
    }
    while (*command_line == L' ' || *command_line == L'\t') ++command_line;
    return command_line;
}

static BOOL replace_filename(WCHAR *path, DWORD capacity, const WCHAR *name)
{
    WCHAR *cursor = path + lstrlenW(path);
    while (cursor > path && cursor[-1] != L'\\' && cursor[-1] != L'/') --cursor;
    if ((DWORD)(cursor - path + lstrlenW(name) + 1) > capacity) return FALSE;
    lstrcpyW(cursor, name);
    return TRUE;
}

static int fail(const WCHAR *message)
{
    DWORD written;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    WriteConsoleW(error, message, (DWORD)lstrlenW(message), &written, NULL);
    WriteConsoleW(error, L"\r\n", 2, &written, NULL);
    return 1;
}

static int run(void)
{
    WCHAR executable[MAX_PATH];
    WCHAR hook[MAX_PATH];
    WCHAR directory[MAX_PATH];
    WCHAR command[32768];
    WCHAR *arguments;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    LPVOID remote_path = NULL;
    HANDLE loader_thread = NULL;
    FARPROC load_library;
    DWORD hook_size;
    DWORD loader_result = 0;
    DWORD exit_code = 1;

    startup.cb = sizeof(startup);
    if (!GetModuleFileNameW(NULL, directory, MAX_PATH) ||
        lstrlenW(directory) >= MAX_PATH - 1)
        return fail(L"UU server launcher could not resolve its path.");
    lstrcpyW(executable, directory);
    lstrcpyW(hook, directory);
    if (!replace_filename(executable, MAX_PATH, L"GameViewerServer.real.exe") ||
        !replace_filename(hook, MAX_PATH, L"uu-server-compat.dll") ||
        !replace_filename(directory, MAX_PATH, L""))
        return fail(L"UU server launcher path is too long.");

    arguments = skip_program_name(GetCommandLineW());
    if ((DWORD)(lstrlenW(executable) + lstrlenW(arguments) + 4) >=
        ARRAYSIZE(command))
        return fail(L"UU server launcher command line is too long.");
    command[0] = L'"';
    lstrcpyW(command + 1, executable);
    lstrcatW(command, L"\"");
    if (*arguments) {
        lstrcatW(command, L" ");
        lstrcatW(command, arguments);
    }

    if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, directory, &startup, &process))
        return fail(L"UU server launcher could not start the real server.");

    hook_size = (DWORD)((lstrlenW(hook) + 1) * sizeof(WCHAR));
    remote_path = VirtualAllocEx(process.hProcess, NULL, hook_size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path ||
        !WriteProcessMemory(process.hProcess, remote_path, hook, hook_size, NULL))
        goto injection_failed;
    load_library = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!load_library) goto injection_failed;
    loader_thread = CreateRemoteThread(process.hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)(void *)load_library, remote_path, 0, NULL);
    if (!loader_thread || WaitForSingleObject(loader_thread, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(loader_thread, &loader_result) || !loader_result)
        goto injection_failed;

    CloseHandle(loader_thread);
    VirtualFreeEx(process.hProcess, remote_path, 0, MEM_RELEASE);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return (int)exit_code;

injection_failed:
    if (loader_thread) CloseHandle(loader_thread);
    if (remote_path) VirtualFreeEx(process.hProcess, remote_path, 0, MEM_RELEASE);
    TerminateProcess(process.hProcess, ERROR_DLL_INIT_FAILED);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return fail(L"UU server launcher could not inject the Wine compatibility hook.");
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)run());
}
