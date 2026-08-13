/* bridge_pe.c — the PE half of the seam spike, stage 2 (#10).
 *
 * A mingw-w64 builtin DLL (patch_marker.py stamps "Wine builtin DLL\0" at file
 * offset 0x40 so Wine loads it as a builtin and pairs it with bridgetest.so).
 * It owns no Steam logic — it only reaches the seam the way winecrt0 does
 * (NtQueryVirtualMemory(MemoryWineUnixFuncs) on its own image base yields the
 * unixlib handle) and forwards call codes into the .so via ntdll's exported
 * __wine_unix_call. It also times the round trips, because "latency per call"
 * is only meaningful measured from the Windows side, where the game lives.
 */

#include <windows.h>
#include <stdint.h>

#include "bridge_abi.h"

typedef UINT64 unixlib_handle_t;
typedef LONG   NTSTATUS_T;

/* wine/winternl.h: MemoryWineUnixFuncs = 1000 */
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS_T (WINAPI *pNtQueryVirtualMemory)(HANDLE, PVOID, DWORD, PVOID, SIZE_T, SIZE_T *);
typedef NTSTATUS_T (WINAPI *pWineUnixCall)(unixlib_handle_t, unsigned int, void *);

static HMODULE          self_module;
static unixlib_handle_t g_handle;
static pWineUnixCall    g_unix_call;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

/* Resolve the unixlib handle + __wine_unix_call once and cache them. Returns 0
 * on success, or a negative step so the exe can localise a failure. */
static int ensure_seam(LONG *status)
{
    HMODULE ntdll;
    pNtQueryVirtualMemory query;
    SIZE_T ret_len = 0;
    NTSTATUS_T st;

    *status = 0;
    if (g_unix_call && g_handle) return 0;

    ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return -1;
    query = (pNtQueryVirtualMemory)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    if (!query) return -2;
    g_unix_call = (pWineUnixCall)GetProcAddress(ntdll, "__wine_unix_call");
    if (!g_unix_call) return -3;              /* not Wine, or export gone */

    st = query(GetCurrentProcess(), (void *)self_module, MemoryWineUnixFuncs,
               &g_handle, sizeof(g_handle), &ret_len);
    if (st) { *status = st; return -4; }      /* no .so attached to us */
    return 0;
}

/* Connect to Steam and read one value back across the seam. */
__declspec(dllexport) int bridge_steam_init(struct steam_init_result *out, LONG *status)
{
    int rc = ensure_seam(status);
    if (rc) return rc;
    if (g_unix_call(g_handle, BRIDGE_CALL_INIT, out)) return -6;
    return 0;
}

/* Time N bare seam round trips (BRIDGE_CALL_NOOP): the transport floor, no
 * native Steam work. Also verifies the magic echoes back intact. */
__declspec(dllexport) int bridge_seam_rtt(UINT64 iters, double *avg_ns, double *min_ns,
                                          int *echo_ok, LONG *status)
{
    LARGE_INTEGER freq, t0, t1;
    struct noop_params np;
    double total = 0.0, mn = 1e18;
    UINT64 i;
    int rc = ensure_seam(status);
    if (rc) return rc;

    *echo_ok = 1;
    QueryPerformanceFrequency(&freq);
    for (i = 0; i < iters; i++) {
        np.magic_in = 0x5EA151DE00000000ULL + i;
        np.magic_out = 0;
        QueryPerformanceCounter(&t0);
        if (g_unix_call(g_handle, BRIDGE_CALL_NOOP, &np)) return -6;
        QueryPerformanceCounter(&t1);
        if (np.magic_out != (np.magic_in ^ BRIDGE_NOOP_XOR)) *echo_ok = 0;
        {
            double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
            total += ns;
            if (ns < mn) mn = ns;
        }
    }
    *avg_ns = iters ? total / (double)iters : 0.0;
    *min_ns = mn;
    return 0;
}

/* Time N real Steamworks getter calls (ISteamFriends::GetPersonaName) across the
 * seam — the cost the game actually pays per Steamworks call. */
__declspec(dllexport) int bridge_persona_rtt(UINT64 iters, double *avg_ns, double *min_ns,
                                             char *name_out, LONG *status)
{
    LARGE_INTEGER freq, t0, t1;
    struct persona_params pp;
    double total = 0.0, mn = 1e18;
    UINT64 i;
    int rc = ensure_seam(status);
    if (rc) return rc;

    name_out[0] = 0;
    QueryPerformanceFrequency(&freq);
    for (i = 0; i < iters; i++) {
        QueryPerformanceCounter(&t0);
        if (g_unix_call(g_handle, BRIDGE_CALL_PERSONA, &pp)) return -6;
        QueryPerformanceCounter(&t1);
        {
            double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart;
            total += ns;
            if (ns < mn) mn = ns;
        }
    }
    if (pp.ok) lstrcpynA(name_out, pp.persona_name, 128);
    *avg_ns = iters ? total / (double)iters : 0.0;
    *min_ns = mn;
    return 0;
}

__declspec(dllexport) int bridge_steam_shutdown(LONG *status)
{
    int rc = ensure_seam(status);
    if (rc) return rc;
    if (g_unix_call(g_handle, BRIDGE_CALL_SHUTDOWN, NULL)) return -6;
    return 0;
}
