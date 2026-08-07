#ifndef UU_WAYLAND_CAPTURE_HOOK_H
#define UU_WAYLAND_CAPTURE_HOOK_H

#include <windows.h>

BOOL uu_install_wayland_capture_hook(HMODULE module);
DWORD WINAPI uu_wayland_capture_hook_thread(void *opaque);

#endif
