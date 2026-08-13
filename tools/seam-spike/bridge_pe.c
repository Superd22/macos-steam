/* bridge_pe.c — the PE half of the seam spike (#8 stage 1).
 *
 * Built with mingw-w64 as bridgetest.dll, then patch_marker.py stamps
 * "Wine builtin DLL\0" at file offset 0x40 so Wine treats it as a builtin
 * and goes looking for x86_64-unix/bridgetest.so in WINEDLLPATH.
 *
 * The seam is reached exactly the way winecrt0 does it, minus the static
 * linkage: NtQueryVirtualMemory(MemoryWineUnixFuncs) on our own image base
 * yields the unixlib handle, and ntdll's exported __wine_unix_call
 * dispatches into the .so's function table.
 */

#include <windows.h>
#include <stdint.h>

typedef UINT64 unixlib_handle_t;
typedef LONG   NTSTATUS_T;

/* wine/winternl.h: MemoryWineUnixFuncs = 1000 */
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS_T (WINAPI *pNtQueryVirtualMemory)( HANDLE, PVOID, DWORD, PVOID, SIZE_T, SIZE_T * );
typedef NTSTATUS_T (WINAPI *pWineUnixCall)( unixlib_handle_t, unsigned int, void * );

/* Must match struct ping_params in bridge_unix.c field for field. */
struct ping_params
{
    UINT64 magic_in;
    UINT64 magic_out;
    UINT64 unix_pid;
    char   unix_sysname[64];
};

static HMODULE self_module;

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        self_module = (HMODULE)inst;
        DisableThreadLibraryCalls( inst );
    }
    return TRUE;
}

/* Returns 0 on success. Negative = which step failed; *status carries the
 * NTSTATUS of the failing call so the exe can print it. */
__declspec(dllexport) int bridge_ping( UINT64 magic_in, struct ping_params *out, LONG *status )
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    pNtQueryVirtualMemory query;
    pWineUnixCall unix_call;
    unixlib_handle_t handle = 0;
    SIZE_T ret_len = 0;
    NTSTATUS_T st;

    *status = 0;
    if (!ntdll) return -1;

    query = (pNtQueryVirtualMemory)GetProcAddress( ntdll, "NtQueryVirtualMemory" );
    if (!query) return -2;

    unix_call = (pWineUnixCall)GetProcAddress( ntdll, "__wine_unix_call" );
    if (!unix_call) return -3;                    /* not Wine, or export gone */

    st = query( GetCurrentProcess(), (void *)self_module, MemoryWineUnixFuncs,
                &handle, sizeof(handle), &ret_len );
    if (st) { *status = st; return -4; }          /* no .so was attached to us */

    out->magic_in = magic_in;
    out->magic_out = 0;
    out->unix_pid = 0;
    out->unix_sysname[0] = 0;

    st = unix_call( handle, 0, out );
    if (st) { *status = st; return -5; }
    return 0;
}
