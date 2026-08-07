#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "capture_protocol.h"
#include "wayland_capture_hook.h"

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

typedef BOOL (WINAPI *BitBltFn)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef UINT (WINAPI *SendInputFn)(UINT, LPINPUT, int);

#define GVINPUT_SEND_RVA 0x76b940u
#define GVINPUT_IMAGE_SIZE 0x1ea4000u

static BitBltFn original_bitblt;
static SendInputFn original_send_input;
static SOCKET helper_connection = INVALID_SOCKET;
static SOCKET input_connection = INVALID_SOCKET;
static CRITICAL_SECTION capture_lock;
static CRITICAL_SECTION input_lock;
static LONG transport_state;

static int socket_send_all(SOCKET socket, const void *data, int size)
{
    const char *cursor = data;
    while (size > 0) {
        int count = send(socket, cursor, size, 0);
        if (count <= 0) return -1;
        cursor += count;
        size -= count;
    }
    return 0;
}

static int socket_recv_all(SOCKET socket, void *data, int size)
{
    char *cursor = data;
    while (size > 0) {
        int count = recv(socket, cursor, size, 0);
        if (count <= 0) return -1;
        cursor += count;
        size -= count;
    }
    return 0;
}

static SOCKET connect_helper(void)
{
    WSADATA data;
    struct sockaddr_in address;
    DWORD timeout = 3000;
    SOCKET connection;
    /* UU balances its own WSAStartup calls with WSACleanup, which can also
       invalidate sockets opened by the injected bridge. Re-register before
       every reconnect so a cleanup in the host cannot disable capture. */
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return INVALID_SOCKET;
    connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) return INVALID_SOCKET;
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(CAPTURE_DEFAULT_PORT);
    if (connect(connection, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        closesocket(connection);
        return INVALID_SOCKET;
    }
    return connection;
}

static uint8_t *request_frame(CaptureReply *reply)
{
    CaptureRequest request = {CAPTURE_MAGIC, CAPTURE_REQUEST,
                              CAPTURE_PROTOCOL_VERSION, 0};
    uint8_t *pixels = NULL;

    EnterCriticalSection(&capture_lock);
    if (helper_connection == INVALID_SOCKET) helper_connection = connect_helper();

    if (helper_connection == INVALID_SOCKET ||
        socket_send_all(helper_connection, &request, sizeof(request)) < 0 ||
        socket_recv_all(helper_connection, reply, sizeof(*reply)) < 0 ||
        reply->magic != CAPTURE_MAGIC || reply->type != CAPTURE_REPLY ||
        reply->protocol_version != CAPTURE_PROTOCOL_VERSION || reply->status ||
        !reply->width || !reply->height || reply->stride != reply->width * 4u ||
        reply->payload_size != reply->stride * reply->height ||
        reply->payload_size > 256u * 1024u * 1024u) goto fail;
    pixels = HeapAlloc(GetProcessHeap(), 0, reply->payload_size);
    if (!pixels || socket_recv_all(helper_connection, pixels,
                                   (int)reply->payload_size) < 0)
        goto fail;
    LeaveCriticalSection(&capture_lock);
    return pixels;

fail:
    if (pixels) HeapFree(GetProcessHeap(), 0, pixels);
    if (helper_connection != INVALID_SOCKET) closesocket(helper_connection);
    helper_connection = INVALID_SOCKET;
    LeaveCriticalSection(&capture_lock);
    return NULL;
}

static BOOL WINAPI capture_bitblt(HDC destination, int x_destination, int y_destination,
                                  int width, int height, HDC source,
                                  int x_source, int y_source, DWORD operation)
{
    CaptureReply reply;
    uint8_t *pixels;
    BITMAPINFO information;
    int copied;
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    BOOL capture_operation = (operation & CAPTUREBLT) != 0 ||
        (x_source == 0 && y_source == 0 && width >= screen_width && height >= screen_height);

    if (!capture_operation || width <= 0 || height <= 0)
        return original_bitblt(destination, x_destination, y_destination, width, height,
                               source, x_source, y_source, operation);

    pixels = request_frame(&reply);
    if (!pixels)
        return original_bitblt(destination, x_destination, y_destination, width, height,
                               source, x_source, y_source, operation);

    memset(&information, 0, sizeof(information));
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = (LONG)reply.width;
    information.bmiHeader.biHeight = -(LONG)reply.height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    copied = StretchDIBits(destination, x_destination, y_destination, width, height,
                           x_source, y_source, width, height, pixels, &information,
                           DIB_RGB_COLORS, SRCCOPY);
    HeapFree(GetProcessHeap(), 0, pixels);
    if (copied <= 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    return TRUE;
}

static UINT send_input_chunk(const INPUT *inputs, UINT count)
{
    CaptureInputRequest request = {CAPTURE_MAGIC, CAPTURE_INPUT_REQUEST,
                                   CAPTURE_PROTOCOL_VERSION, count};
    CaptureInputEvent events[CAPTURE_MAX_INPUT_EVENTS];
    CaptureInputReply reply;
    UINT index;

    if (!count || count > CAPTURE_MAX_INPUT_EVENTS) return 0;
    for (index = 0; index < count; ++index) {
        CaptureInputEvent *event = &events[index];
        memset(event, 0, sizeof(*event));
        if (inputs[index].type == INPUT_MOUSE) {
            event->type = CAPTURE_INPUT_MOUSE;
            event->dx = inputs[index].mi.dx;
            event->dy = inputs[index].mi.dy;
            event->mouse_data = inputs[index].mi.mouseData;
            event->flags = inputs[index].mi.dwFlags;
        } else if (inputs[index].type == INPUT_KEYBOARD) {
            event->type = CAPTURE_INPUT_KEYBOARD;
            event->virtual_key = inputs[index].ki.wVk;
            event->scan_code = inputs[index].ki.wScan;
            event->flags = inputs[index].ki.dwFlags;
        } else {
            return 0;
        }
    }

    EnterCriticalSection(&input_lock);
    if (input_connection == INVALID_SOCKET) input_connection = connect_helper();
    if (input_connection == INVALID_SOCKET ||
        socket_send_all(input_connection, &request, sizeof(request)) < 0 ||
        socket_send_all(input_connection, events, sizeof(events[0]) * count) < 0 ||
        socket_recv_all(input_connection, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_INPUT_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION ||
        reply.accepted_count > count) {
        if (input_connection != INVALID_SOCKET) closesocket(input_connection);
        input_connection = INVALID_SOCKET;
        LeaveCriticalSection(&input_lock);
        return 0;
    }
    LeaveCriticalSection(&input_lock);
    return reply.accepted_count;
}

static UINT WINAPI capture_send_input(UINT count, LPINPUT inputs, int input_size)
{
    UINT accepted = 0;

    if (!inputs || input_size != sizeof(INPUT)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    while (accepted < count) {
        UINT chunk = count - accepted;
        UINT result;
        if (chunk > CAPTURE_MAX_INPUT_EVENTS) chunk = CAPTURE_MAX_INPUT_EVENTS;
        result = send_input_chunk(inputs + accepted, chunk);
        if (!result) {
            if (!accepted && original_send_input)
                return original_send_input(count, inputs, input_size);
            break;
        }
        accepted += result;
        if (result != chunk) break;
    }
    return accepted;
}

static BOOL patch_bitblt_import(HMODULE module)
{
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    if (!descriptor) return FALSE;

    for (; descriptor->Name; ++descriptor) {
        const char *library = (const char *)(base + descriptor->Name);
        IMAGE_THUNK_DATA64 *names;
        IMAGE_THUNK_DATA64 *addresses;
        if (lstrcmpiA(library, "gdi32.dll") != 0) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + descriptor->OriginalFirstThunk);
        addresses = (IMAGE_THUNK_DATA64 *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *import_name;
            DWORD protection;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)import_name->Name, "BitBlt") != 0) continue;
            if ((BitBltFn)(uintptr_t)addresses->u1.Function == capture_bitblt)
                return TRUE;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                                PAGE_READWRITE, &protection)) return FALSE;
            original_bitblt = (BitBltFn)(uintptr_t)addresses->u1.Function;
            InterlockedExchangePointer((void *volatile *)&addresses->u1.Function,
                                       (void *)capture_bitblt);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                           protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                                  sizeof(addresses->u1.Function));
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL patch_send_input_import(HMODULE module)
{
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_IMPORT_DESCRIPTOR *descriptor;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    descriptor = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    if (!descriptor) return FALSE;

    for (; descriptor->Name; ++descriptor) {
        const char *library = (const char *)(base + descriptor->Name);
        IMAGE_THUNK_DATA64 *names;
        IMAGE_THUNK_DATA64 *addresses;
        if (lstrcmpiA(library, "user32.dll") != 0) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + descriptor->OriginalFirstThunk);
        addresses = (IMAGE_THUNK_DATA64 *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *import_name;
            DWORD protection;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)import_name->Name, "SendInput") != 0) continue;
            if ((SendInputFn)(uintptr_t)addresses->u1.Function == capture_send_input)
                return TRUE;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                                PAGE_READWRITE, &protection)) return FALSE;
            original_send_input = (SendInputFn)(uintptr_t)addresses->u1.Function;
            InterlockedExchangePointer((void *volatile *)&addresses->u1.Function,
                                       (void *)capture_send_input);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                           protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                                  sizeof(addresses->u1.Function));
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL patch_gvinput_send(HMODULE module)
{
    static const uint8_t expected[] = {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48
    };
    /* Jump through GameViewerServer's USER32!SendInput IAT entry. The IAT is
       patched first, so UU's already-structured INPUT array reaches the helper. */
    static const uint8_t replacement[] = {0xff, 0x25, 0xfa, 0xf7, 0x6c, 0x01};
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    uint8_t *target;
    DWORD protection;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage != GVINPUT_IMAGE_SIZE) return FALSE;
    target = base + GVINPUT_SEND_RVA;
    if (!memcmp(target, replacement, sizeof(replacement))) return TRUE;
    if (memcmp(target, expected, sizeof(expected))) return FALSE;
    if (!VirtualProtect(target, sizeof(replacement), PAGE_EXECUTE_READWRITE,
                        &protection)) return FALSE;
    memcpy(target, replacement, sizeof(replacement));
    VirtualProtect(target, sizeof(replacement), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(replacement));
    return TRUE;
}

BOOL uu_install_wayland_capture_hook(HMODULE module)
{
    WSADATA data;
    BOOL input_patched;
    HMODULE process_module;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return FALSE;
    if (InterlockedCompareExchange(&transport_state, 1, 0) == 0) {
        InitializeCriticalSection(&capture_lock);
        InitializeCriticalSection(&input_lock);
        InterlockedExchange(&transport_state, 2);
    } else {
        while (InterlockedCompareExchange(&transport_state, 2, 2) != 2) Sleep(0);
    }
    input_patched = patch_send_input_import(module);
    process_module = GetModuleHandleW(NULL);
    if (process_module) {
        input_patched = patch_send_input_import(process_module) || input_patched;
        if (input_patched) patch_gvinput_send(process_module);
    }
    return patch_bitblt_import(module) && input_patched;
}

DWORD WINAPI uu_wayland_capture_hook_thread(void *opaque)
{
    HMODULE streamer = NULL;
    unsigned int attempt;
    (void)opaque;
    for (attempt = 0; attempt < 200 && !streamer; ++attempt) {
        streamer = GetModuleHandleW(L"streamer.dll");
        if (!streamer) Sleep(25);
    }
    return streamer && uu_install_wayland_capture_hook(streamer) ? 0 : 1;
}
