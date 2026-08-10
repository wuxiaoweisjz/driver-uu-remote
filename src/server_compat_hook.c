#include <windows.h>
#include <cfgmgr32.h>
#include <stdint.h>
#include <string.h>

typedef CONFIGRET (WINAPI *CMGetChildFn)(PDEVINST, DEVINST, ULONG);
typedef CONFIGRET (WINAPI *CMQueryAndRemoveSubTreeWFn)(
    DEVINST, PPNP_VETO_TYPE, LPWSTR, ULONG, ULONG);

static CMGetChildFn original_cm_get_child;
static CMQueryAndRemoveSubTreeWFn original_cm_query_and_remove_subtree;

static CONFIGRET WINAPI compat_cm_get_child(PDEVINST child, DEVINST parent,
                                             ULONG flags)
{
    if (parent == 0) {
        if (child) *child = 0;
        return CR_NO_SUCH_DEVNODE;
    }
    return original_cm_get_child
        ? original_cm_get_child(child, parent, flags)
        : CR_FAILURE;
}

static CONFIGRET WINAPI compat_cm_query_and_remove_subtree(
    DEVINST ancestor, PPNP_VETO_TYPE veto_type, LPWSTR veto_name,
    ULONG name_length, ULONG flags)
{
    (void)veto_type;
    (void)veto_name;
    (void)name_length;
    (void)flags;
    if (ancestor == 0) return CR_NO_SUCH_DEVNODE;
    /* Wine 11.14 aborts in this unimplemented export. UU only uses it to
       clean Windows-only virtual devices, which cannot exist under Wine. */
    return CR_NO_SUCH_DEVNODE;
}

static BOOL patch_import(HMODULE module, const char *library,
                         const char *function, void *replacement,
                         void **original)
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
        IMAGE_THUNK_DATA64 *names;
        IMAGE_THUNK_DATA64 *addresses;
        const char *import_library = (const char *)(base + descriptor->Name);
        if (lstrcmpiA(import_library, library) != 0) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + descriptor->OriginalFirstThunk);
        addresses = (IMAGE_THUNK_DATA64 *)(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *import_name;
            DWORD protection;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)import_name->Name, function) != 0) continue;
            if ((void *)(uintptr_t)addresses->u1.Function == replacement) return TRUE;
            if (!VirtualProtect(&addresses->u1.Function,
                                sizeof(addresses->u1.Function), PAGE_READWRITE,
                                &protection)) return FALSE;
            *original = (void *)(uintptr_t)addresses->u1.Function;
            InterlockedExchangePointer((void *volatile *)&addresses->u1.Function,
                                       replacement);
            VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function),
                           protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), &addresses->u1.Function,
                                  sizeof(addresses->u1.Function));
            return TRUE;
        }
    }
    return FALSE;
}

__declspec(dllexport) BOOL WINAPI UUInstallServerCompatHook(void)
{
    HMODULE process = GetModuleHandleW(NULL);
    BOOL child_patched = patch_import(
        process, "setupapi.dll", "CM_Get_Child", (void *)compat_cm_get_child,
        (void **)&original_cm_get_child);
    BOOL remove_patched = patch_import(
        process, "setupapi.dll", "CM_Query_And_Remove_SubTreeW",
        (void *)compat_cm_query_and_remove_subtree,
        (void **)&original_cm_query_and_remove_subtree);
    return child_patched && remove_patched;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        UUInstallServerCompatHook();
    }
    return TRUE;
}
