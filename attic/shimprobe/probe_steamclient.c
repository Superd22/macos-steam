/* Decoy steamclient64.dll — proves which code answered SteamAPI_Init (#13).
 * Logs every load and CreateInterface request to shimprobe.log beside the DLL,
 * then returns NULL so init still fails (this is an instrument, not a shim). */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static void note(const char *fmt, ...)
{
    char dir[MAX_PATH], path[MAX_PATH + 16];
    HMODULE self;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&note, &self);
    GetModuleFileNameA(self, dir, sizeof(dir));
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof(path), "%s\\shimprobe.log", dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        note("PROBE_DLLMAIN_ATTACH pid=%lu\n", GetCurrentProcessId());
    return TRUE;
}

__declspec(dllexport) void *CreateInterface(const char *name, int *ret)
{
    note("PROBE_CREATEINTERFACE name=%s\n", name ? name : "(null)");
    if (ret) *ret = 1;
    return NULL;
}
