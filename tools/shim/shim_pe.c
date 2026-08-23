/* shim_pe.c — the PE half of the achievement shim (#11): steamclient64.dll.
 *
 * This is the file steam_api64.dll loads via HKCU\...\ActiveProcess\
 * SteamClientDll64 (#13). It presents Valve's flat C export set and MSVC-ABI
 * interface vtables to steam_api64.dll, and forwards every call across Wine's
 * __wine_unix_call seam into steamclient64.so, which drives the real
 * steamclient.dylib (shim_unix.cpp). It holds ZERO Steam logic — pure
 * marshalling — exactly like Proton's lsteamclient PE side (#3).
 *
 * The seam only bridges the CALLING CONVENTION: steam_api64.dll calls our
 * vtable thunks MS-x64 (this in RCX); each thunk packs a fixed-layout params
 * struct and crosses to the unix side, where the native call is made SysV. One
 * address space, one arch, so pointers and callback payloads pass verbatim.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shim_abi.h"

/* i386 MSVC instance methods are __thiscall: `this` in ECX, remaining args on
 * the stack, and CALLEE-cleanup — the callee must pop exactly the bytes the
 * caller pushed. On x86_64 that same C signature already IS the convention
 * (this in RCX) and cleanup is the caller's, so THISCALL is empty there. Wine
 * draws the same line in include/wine/asm.h (DEFINE_THISCALL_WRAPPER is a no-op
 * off i386); gcc's thiscall attribute gives us the cleanup for free from the
 * declared signature, which is why every vtable thunk below must carry it. */
#if defined(__i386__)
# define THISCALL __attribute__((thiscall))
#else
# define THISCALL
#endif


typedef UINT64 unixlib_handle_t;
typedef LONG   NTSTATUS_T;
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS_T (WINAPI *pNtQueryVirtualMemory)(HANDLE, PVOID, DWORD, PVOID, SIZE_T, SIZE_T *);
typedef NTSTATUS_T (WINAPI *pWineUnixCall)(unixlib_handle_t, unsigned int, void *);

static HMODULE          self_module;
static unixlib_handle_t g_handle;
static pWineUnixCall    g_unix_call;

/* OutputDebugStringA alone is not enough: CrossOver's launcher does not pass
 * WINEDEBUG through, so under the production launch path the PE side is silent
 * exactly when a title fails and you need to know which vtable slot it wanted.
 * Same lesson as #12's stderr-only launch log. Set SHIM_PE_LOG to a path.
 * The unix half already does this via SHIM_UNIX_LOG. */
static void dbg(const char *fmt, ...)
{
    static FILE *log;
    static int   tried;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    if (!tried) {
        const char *p = getenv("SHIM_PE_LOG");
        tried = 1;
        if (p) log = fopen(p, "a");
    }
    if (log) { fprintf(log, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf); fflush(log); }
}

/* Wine reports a fault as a bare absolute address, and CrossOver strips the
 * trace channels that would name the module (+loaddll produces nothing here),
 * so the only way to attribute a crash is to have written the map down first.
 * Only runs when SHIM_PE_LOG is set — this is a diagnostic, not a hot path. */
static void dump_modules(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    if (!getenv("SHIM_PE_LOG")) return;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof me;
    if (Module32First(snap, &me)) do {
        dbg("shim: module %p..%p  %s", me.modBaseAddr,
            (void *)(me.modBaseAddr + me.modBaseSize), me.szModule);
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
}

static int ensure_seam(void)
{
    HMODULE ntdll;
    pNtQueryVirtualMemory query;
    SIZE_T ret_len = 0;
    if (g_unix_call && g_handle) return 0;
    ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return -1;
    query = (pNtQueryVirtualMemory)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    g_unix_call = (pWineUnixCall)GetProcAddress(ntdll, "__wine_unix_call");
    if (!query || !g_unix_call) return -2;
    if (query(GetCurrentProcess(), (void *)self_module, MemoryWineUnixFuncs,
              &g_handle, sizeof(g_handle), &ret_len)) return -3;
    return 0;
}

static int seam(unsigned code, void *args)
{
    if (ensure_seam()) { dbg("shim: seam unavailable (not Wine / no .so)"); return -1; }
    return (int)g_unix_call(g_handle, code, args);
}

/* ---- per-thread scratch (#20) -------------------------------------------
 * gcc's __thread does NOT work here. This DLL is brought in by LoadLibrary at
 * runtime (steam_api resolves it from the SteamClientDll registry value), and a
 * thread that already existed when that happened has no slot for the module's
 * TLS block — reads come back as garbage. Explicit TlsAlloc/TlsGetValue has no
 * such rule: the slot is per-thread and allocated on first touch, whenever the
 * thread was born.
 *
 * NOTE: an earlier revision of this comment credited the switch with fixing the
 * Unity crash in GetCurrentGameLanguage. It did not, and the claim cost a lot of
 * time. That crash was in the dbg() call one line below the copy-down, which
 * formatted the RAW native pointer with %s instead of the copied-down string;
 * on i386 that truncates a >4 GB macOS heap address to 32 bits and the CRT's
 * strlen walks into it. The DLL that still crashed already had this TLS code in
 * it. Two independent i386 pointer-width bugs on the same line of source, and
 * fixing the harder one first hid the easier one — see FINDINGS-32BIT.md. */
#ifdef __i386__          /* only the i386 copy-down paths need per-thread scratch */
static DWORD g_tls = TLS_OUT_OF_INDEXES;

struct tls_scratch {
    char           str[4][1024];   /* rotating pool for const char* returns   */
    unsigned       turn;
    unsigned char *payload;        /* grown to fit the largest callback seen  */
    int            payload_cap;
};

static struct tls_scratch *tls_get(void)
{
    struct tls_scratch *t;
    if (g_tls == TLS_OUT_OF_INDEXES) return NULL;
    t = (struct tls_scratch *)TlsGetValue(g_tls);
    if (!t) {
        t = (struct tls_scratch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
        if (!t) return NULL;
        TlsSetValue(g_tls, t);
    }
    return t;
}

static void tls_free_current_thread(void)
{
    struct tls_scratch *t;
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    t = (struct tls_scratch *)TlsGetValue(g_tls);
    if (!t) return;
    if (t->payload) HeapFree(GetProcessHeap(), 0, t->payload);
    HeapFree(GetProcessHeap(), 0, t);
    TlsSetValue(g_tls, NULL);
}
#else
static void tls_free_current_thread(void) { }
#endif

/* ---- native memory the game is handed BACK (#20) ------------------------
 * Direction matters. Anything the game passes IN is a PE address: it zero-
 * extends into the uint64 params-struct fields and the native side writes
 * through it, on both bitnesses, with no work. Anything the native side hands
 * BACK by pointer is different — it lives on the dylib's heap, which on macOS
 * sits well above 4 GB (0x7fd695b48ae0, from our own shim_unix.log). A 64-bit
 * PE holds that fine. A 32-bit PE cannot hold it at all, so the bytes must be
 * copied down across the seam before the game can look at them. */
#ifdef __i386__
/* Rotating pool: Valve's const char* returns are documented valid until the
 * next call, but games routinely hold two at once (language + country in one
 * log line). Four deep is cheap insurance; a single buffer is not. */
static const char *native_str(uint64_t native)
{
    struct tls_scratch *t = tls_get();
    struct sp_copystr p;
    char *buf;
    if (!native) return NULL;
    if (!t) { dbg("shim: native_str has no TLS scratch"); return NULL; }
    buf = t->str[t->turn++ & 3];
    p.src = native; p.dst = (uint64_t)(uintptr_t)buf; p.cap = (int32_t)sizeof t->str[0]; p.ret = 0;
    seam(C_CopyStr, &p);
    if (p.ret >= (int32_t)sizeof t->str[0])
        dbg("shim: native_str TRUNCATED %d -> %d bytes", p.ret, (int)sizeof t->str[0] - 1);
    return buf;
}
#else
static const char *native_str(uint64_t native) { return (const char *)(uintptr_t)native; }
#endif

/* ---- PE-side interface object: MSVC vtable ptr @0, opaque native handle @8 -- */
struct w_iface { const void **vtable; uint64_t handle; };

/* The MSVC vtables the game sees: one table per interface VERSION, generated
 * slot-exact from Proton's lsteamclient (shim_vtables.h / gen_vtables.py, #20).
 *
 * #11 could serve every version of an interface from one hand-written table
 * whose unused slots were 0-arg stubs, because MS-x64 is caller-cleanup.
 * Neither shortcut survives i386:
 *   - thiscall is CALLEE-cleanup, so a stub must pop exactly the bytes its
 *     caller pushed. The generated stubs carry Proton's per-method arity.
 *   - layouts differ BY VERSION, not just by interface: SteamInput006 (Mars)
 *     inserts SetInputActionManifestFilePath at slot 2, shifting every later
 *     slot against SteamInput002 (Among Us). One table for both silently calls
 *     the wrong method (map trap 2).
 * So wiring below is resolved BY NAME against each version's own table, and a
 * name that is not in that version is reported rather than silently dropped. */
static unsigned int vt_unmapped(const char *version, int slot, const char *name);
#include "shim_vtables.h"

static unsigned int vt_unmapped(const char *version, int slot, const char *name)
{ dbg("shim: %s slot %d %s (unmapped)", version, slot, name); return 0; }

static const struct vt_desc *vt_find(const char *ver)
{
    int i;
    if (!ver) return NULL;
    for (i = 0; g_vtdescs[i].version; i++)
        if (!strcmp(g_vtdescs[i].version, ver)) return &g_vtdescs[i];
    return NULL;
}

/* Wire one real thunk into a version's table by METHOD NAME. Returns 0 on
 * success. A miss is loud: on i386 a wrongly-placed thunk is a stack corruption,
 * not a wrong answer, so this must never fail quietly. */
/* Every ISteamClient typed getter does the same thing: hand back the interface
 * for a version string, which acquire() already resolves generically. Wiring
 * them one at a time means a title asking for one nobody wired gets NULL —
 * which is what left Among Us holding a NULL ISteamMatchmaking. It is a
 * multiplayer game, so it stops there, and reports it as a failed sign-in (#20).
 * Wire the whole family from the version's own table instead. Called BEFORE the
 * explicit wiring below, so a hand-written thunk still wins. */
static void *THISCALL ic_GetIface_UPV(struct w_iface *s, int32_t u, int32_t pipe, const char *ver);
static void *THISCALL ic_GetIface_PV(struct w_iface *s, int32_t pipe, const char *ver);

static void wire_getters(const char *ver)
{
    const struct vt_desc *d = vt_find(ver);
    int i;
    if (!d) { dbg("shim: wire_getters: no table for %s", ver); return; }
    for (i = 0; d->methods[i].name; i++) {
        const struct vt_method *m = &d->methods[i];
        if (strncmp(m->name, "GetISteam", 9)) continue;
        if (m->bytes == 16)      d->vtable[m->slot] = (const void *)ic_GetIface_UPV;
        else if (m->bytes == 12) d->vtable[m->slot] = (const void *)ic_GetIface_PV;
        else dbg("shim: %s.%s unexpected shape (%u bytes) — left stubbed",
                 ver, m->name, (unsigned)m->bytes);
    }
}

static int wire(const char *ver, const char *method, const void *fn)
{
    const struct vt_desc *d = vt_find(ver);
    int i;
    if (!d) { dbg("shim: WIRE MISS %s (version not generated) .%s", ver, method); return -1; }
    for (i = 0; d->methods[i].name; i++)
        if (!strcmp(d->methods[i].name, method)) { d->vtable[d->methods[i].slot] = fn; return 0; }
    dbg("shim: WIRE MISS %s.%s (not in this version)", ver, method);
    return -1;
}

/* small w_iface cache so repeated acquisition of the same native handle returns
 * a stable pointer (games and steam_api64.dll compare interface pointers). */
static struct { uint64_t handle; const void **vt; struct w_iface *w; } g_cache[32];
static int g_ncache;

static struct w_iface *wrap(uint64_t handle, const void **vt)
{
    int i;
    if (!handle || !vt) return NULL;   /* vt==NULL: no table for this version (i386) */
    for (i = 0; i < g_ncache; i++)
        if (g_cache[i].handle == handle && g_cache[i].vt == vt) return g_cache[i].w;
    struct w_iface *w = (struct w_iface *)HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtable = vt; w->handle = handle;
    if (g_ncache < 32) { g_cache[g_ncache].handle = handle; g_cache[g_ncache].vt = vt; g_cache[g_ncache].w = w; g_ncache++; }
    return w;
}

#ifndef __i386__
/* x86_64-only fallback for a version with no generated table. MS-x64 is
 * caller-cleanup, so a 0-arg stub is harmless under any real signature and #11
 * shipped exactly this. i386 has NO safe equivalent: a stub that pops the wrong
 * byte count corrupts the caller's stack on the first call, so there we hand
 * back NULL and let steam_api see an unavailable interface instead. */
#define GENERIC_SLOTS 128
static const void *vt_generic[GENERIC_SLOTS];
static uint64_t generic_stub(void) { return 0; }
static void fill_generic(void)
{ int i; for (i = 0; i < GENERIC_SLOTS; i++) vt_generic[i] = (const void *)generic_stub; }
#endif

/* Resolve a requested interface version to its slot-exact vtable. Matching is
 * on the FULL version string: "SteamInput002" and "SteamInput006" are different
 * layouts, and the old prefix match (any "SteamInput*" -> one table) is what
 * would have served Among Us the Mars layout (#20). */
static const void **vt_resolve(const char *ver)
{
    const struct vt_desc *d = vt_find(ver);
    if (d) return d->vtable;
#ifdef __i386__
    dbg("shim: NO TABLE for \"%s\" -> returning NULL (i386 has no safe generic "
        "vtable; add it to interface-versions.txt and rebuild)", ver ? ver : "(null)");
    return NULL;
#else
    dbg("shim: no table for \"%s\" -> generic stub vtable", ver ? ver : "(null)");
    return vt_generic;
#endif
}

/* ---- ISteamClient thunks ------------------------------------------------ */
static int32_t THISCALL ic_CreateSteamPipe(struct w_iface *s)
{ struct sp_client_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Client_CreateSteamPipe, &p);
  dbg("shim: CreateSteamPipe -> %d", p.ret); return p.ret; }
static uint8_t THISCALL ic_BReleaseSteamPipe(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_BReleaseSteamPipe, &p); return (uint8_t)p.ret; }
static int32_t THISCALL ic_ConnectToGlobalUser(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_ConnectToGlobalUser, &p);
  dbg("shim: ConnectToGlobalUser(%d) -> %d", pipe, p.ret); return p.ret; }
static void THISCALL ic_ReleaseUser(struct w_iface *s, int32_t pipe, int32_t user)
{ struct sp_client_releaseu p; p.handle = s->handle; p.pipe = pipe; p.user = user; seam(C_Client_ReleaseUser, &p); }

static void *acquire(struct w_iface *s, int32_t user, int32_t pipe, const char *ver)
{
    struct sp_client_getgen p; p.handle = s->handle; p.ver = (uint64_t)(uintptr_t)ver;
    p.user = user; p.pipe = pipe; p.ret = 0;
    seam(C_Client_GetGeneric, &p);
    dbg("shim: acquire(\"%s\") native=0x%llx", ver ? ver : "(null)", (unsigned long long)p.ret);
    return wrap(p.ret, vt_resolve(ver));
}
/* The two shapes every ISteamClient typed getter comes in. Proton's byte count
 * tells them apart: 16 = (this, hUser, hPipe, version), 12 = (this, hPipe,
 * version) — the pipe-only form GetISteamUtils uses. */
static void * THISCALL ic_GetIface_UPV(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetIface_PV(struct w_iface *s, int32_t pipe, const char *ver)             { return acquire(s, 0, pipe, ver); }

static void * THISCALL ic_GetISteamUser(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamUtils(struct w_iface *s, int32_t pipe, const char *ver)                { return acquire(s, 0, pipe, ver); }
static void * THISCALL ic_GetISteamUserStats(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamApps(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamFriends(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)   { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamGenericInterface(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }

/* ---- ISteamUser thunks -------------------------------------------------- */
static int32_t THISCALL iu_GetHSteamUser(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetHSteamUser, &p); return p.ret; }
static uint8_t THISCALL iu_BLoggedOn(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_BLoggedOn, &p); return (uint8_t)p.ret; }
/* GetSteamID returns CSteamID BY VALUE. MSVC uses the sret ABI for it: the caller
 * passes a hidden result-buffer pointer (rdx, after this=rcx) and expects the
 * callee to fill it and RETURN THAT POINTER in rax. The dylib (SysV) returns the
 * value in rax, so the seam value is correct; only this PE thunk must adapt.
 * Getting this wrong makes steam_api64 deref the SteamID as a pointer (page fault). */
static uint64_t * THISCALL iu_GetSteamID(struct w_iface *s, uint64_t *sret)
{ struct sp_user_u64 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetSteamID, &p);
  *sret = p.ret; return sret; }

/* ---- ISteamUtils thunks ------------------------------------------------- */
static uint32_t THISCALL iut_GetAppID(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetAppID, &p);
  dbg("shim: GetAppID() -> %u", p.ret); return p.ret; }
static uint32_t THISCALL iut_GetSecondsSinceAppActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceAppActive, &p); return p.ret; }
static uint32_t THISCALL iut_GetSecondsSinceComputerActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceComputerActive, &p); return p.ret; }
static int32_t THISCALL iut_GetConnectedUniverse(struct w_iface *s)
{ struct sp_utils_i32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetConnectedUniverse, &p); return p.ret; }
static uint32_t THISCALL iut_GetServerRealTime(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetServerRealTime, &p); return p.ret; }
static const char * THISCALL iut_GetIPCountry(struct w_iface *s)
{ struct sp_utils_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCountry, &p);
  r = native_str(p.ret);
  dbg("shim: GetIPCountry() -> %s", r ? r : "(null)");
  return r; }
static uint8_t THISCALL iut_GetCurrentBatteryPower(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetCurrentBatteryPower, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL iut_IsAPICallCompleted(struct w_iface *s, uint64_t call, void *failed)
{ struct sp_utils_call p; p.handle = s->handle; p.call = call; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_IsAPICallCompleted, &p); return (uint8_t)p.ret; }
static int32_t THISCALL iut_GetAPICallFailureReason(struct w_iface *s, uint64_t call)
{ struct sp_utils_callfail p; p.handle = s->handle; p.call = call; p.ret = 0;
  seam(C_Utils_GetAPICallFailureReason, &p); return p.ret; }
static uint8_t THISCALL iut_GetAPICallResult(struct w_iface *s, uint64_t call, void *buf, int32_t cub, int32_t expected, void *failed)
{ struct sp_utils_callres p; p.handle = s->handle; p.call = call; p.buf = (uint64_t)(uintptr_t)buf;
  p.cub = cub; p.expected = expected; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_GetAPICallResult, &p); return (uint8_t)p.ret; }
static void THISCALL iut_RunFrame(struct w_iface *s)
{ struct sp_utils_void p; p.handle = s->handle; seam(C_Utils_RunFrame, &p); }
static uint32_t THISCALL iut_GetIPCCallCount(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCCallCount, &p); return p.ret; }
static const char * THISCALL iut_GetSteamUILanguage(struct w_iface *s)
{ struct sp_utils_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSteamUILanguage, &p);
  r = native_str(p.ret);
  dbg("shim: GetSteamUILanguage() -> %s", r ? r : "(null)");
  return r; }

/* ---- ISteamUserStats (v012) thunks -------------------------------------- */
static uint8_t THISCALL is_RequestCurrentStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_RequestCurrentStats, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_GetAchievement(struct w_iface *s, const char *name, void *achieved)
{ struct sp_stats_getach p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.achieved = (uint64_t)(uintptr_t)achieved; p.ret = 0;
  seam(C_Stats_GetAchievement, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_SetAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_SetAchievement, &p);
  dbg("shim: SetAchievement(\"%s\") -> %d", name, p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL is_ClearAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_ClearAchievement, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_GetAchievementAndUnlockTime(struct w_iface *s, const char *name, void *achieved, void *unlock)
{ struct sp_stats_getachtime p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.achieved = (uint64_t)(uintptr_t)achieved; p.unlock = (uint64_t)(uintptr_t)unlock; p.ret = 0;
  seam(C_Stats_GetAchievementAndUnlockTime, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_StoreStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_StoreStats, &p);
  dbg("shim: StoreStats() -> %d", p.ret); return (uint8_t)p.ret; }
static uint32_t THISCALL is_GetNumAchievements(struct w_iface *s)
{ struct sp_stats_u32ret p; p.handle = s->handle; p.ret = 0; seam(C_Stats_GetNumAchievements, &p); return p.ret; }
static const char * THISCALL is_GetAchievementName(struct w_iface *s, uint32_t idx)
{ struct sp_stats_nameidx p; p.handle = s->handle; p.idx = idx; p.ret = 0; seam(C_Stats_GetAchievementName, &p); return native_str(p.ret); }
static uint8_t THISCALL is_ResetAllStats(struct w_iface *s, uint8_t achievements_too)
{ struct sp_stats_reset p; p.handle = s->handle; p.achievements_too = achievements_too; p.ret = 0; seam(C_Stats_ResetAllStats, &p);
  dbg("shim: ResetAllStats(%d) -> %d", achievements_too, p.ret); return (uint8_t)p.ret; }
static const char * THISCALL is_GetAchievementDisplayAttribute(struct w_iface *s, const char *name, const char *key)
{ struct sp_stats_dispattr p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Stats_GetAchievementDisplayAttribute, &p); return native_str(p.ret); }

/* ---- ISteamApps (VERSION008, slots 0-9) thunks -------------------------- */
static uint8_t THISCALL ia_BIsSubscribed(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribed, &p);
  dbg("shim: BIsSubscribed() -> %d", p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsLowViolence(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsLowViolence, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsCybercafe(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsCybercafe, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsVACBanned(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsVACBanned, &p); return (uint8_t)p.ret; }
static const char * THISCALL ia_GetCurrentGameLanguage(struct w_iface *s)
{ struct sp_apps_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetCurrentGameLanguage, &p);
  r = native_str(p.ret);
  dbg("shim: GetCurrentGameLanguage() -> %s", r ? r : "(null)");
  return r; }
static const char * THISCALL ia_GetAvailableGameLanguages(struct w_iface *s)
{ struct sp_apps_str p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAvailableGameLanguages, &p);
  return native_str(p.ret); }
static uint8_t THISCALL ia_BIsSubscribedApp(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsSubscribedApp, &p);
  dbg("shim: BIsSubscribedApp(%u) -> %d", appid, p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsDlcInstalled(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsDlcInstalled, &p); return (uint8_t)p.ret; }
static uint32_t THISCALL ia_GetEarliestPurchaseUnixTime(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_u32 p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_GetEarliestPurchaseUnixTime, &p); return p.ret; }
static uint8_t THISCALL ia_BIsSubscribedFromFreeWeekend(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribedFromFreeWeekend, &p); return (uint8_t)p.ret; }
/* GetAppOwner returns CSteamID BY VALUE — same MSVC sret ABI as iu_GetSteamID
 * below: fill the caller's hidden buffer and return that pointer, not the value. */
static uint64_t * THISCALL ia_GetAppOwner(struct w_iface *s, uint64_t *sret)
{ struct sp_apps_u64 p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAppOwner, &p);
  *sret = p.ret; return sret; }
static const char * THISCALL ia_GetLaunchQueryParam(struct w_iface *s, const char *key)
{ struct sp_apps_qparam p; p.handle = s->handle; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Apps_GetLaunchQueryParam, &p); return native_str(p.ret); }

/* ---- ISteamUser (appended) ---------------------------------------------- */
/* EOS "Auth with Steam" (#20). Async: the SteamAPICall_t comes back here and the
 * ticket itself arrives later via EncryptedAppTicketResponse_t (1466) on the
 * existing pump, whereupon the game calls GetEncryptedAppTicket. Both buffers
 * belong to the game, so they are PE addresses that zero-extend across the
 * seam — no copy-down. Proton: slot 20 pops 12 bytes, slot 21 pops 16. */
static uint64_t THISCALL iu_RequestEncryptedAppTicket(struct w_iface *s, void *data, int32_t cb)
{ struct sp_user_reqticket p; p.handle = s->handle; p.data = (uint64_t)(uintptr_t)data;
  p.cb = cb; p.ret = 0; seam(C_User_RequestEncryptedAppTicket, &p);
  dbg("shim: RequestEncryptedAppTicket(cb=%d) -> call=%llu", cb, (unsigned long long)p.ret);
  return p.ret; }
static uint8_t THISCALL iu_GetEncryptedAppTicket(struct w_iface *s, void *ticket, int32_t max, uint32_t *cbticket)
{ struct sp_user_getticket p; p.handle = s->handle; p.ticket = (uint64_t)(uintptr_t)ticket;
  p.cbticket = (uint64_t)(uintptr_t)cbticket; p.max = max; p.ret = 0;
  seam(C_User_GetEncryptedAppTicket, &p);
  dbg("shim: GetEncryptedAppTicket(max=%d) -> %d (%u bytes)", max, p.ret, cbticket ? *cbticket : 0u);
  return (uint8_t)p.ret; }
static uint8_t THISCALL iu_GetUserDataFolder(struct w_iface *s, char *buf, int32_t len)
{ struct sp_user_datafolder p; p.handle = s->handle; p.buf = (uint64_t)(uintptr_t)buf; p.len = len; p.ret = 0;
  seam(C_User_GetUserDataFolder, &p); return (uint8_t)p.ret; }

/* ---- ISteamInput (VERSION006) thunks ------------------------------------ */
static uint8_t THISCALL iin_Init(struct w_iface *s, uint8_t explicit_runframe)
{ struct sp_input_init p; p.handle = s->handle; p.explicit_runframe = explicit_runframe; p.ret = 0;
  seam(C_Input_Init, &p); dbg("shim: SteamInput Init(%d) -> %d", explicit_runframe, p.ret);
  return (uint8_t)p.ret; }
static uint8_t THISCALL iin_Shutdown(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_Shutdown, &p); return (uint8_t)p.ret; }
static void THISCALL iin_RunFrame(struct w_iface *s, uint8_t reserved)
{ struct sp_input_runframe p; p.handle = s->handle; p.reserved = reserved; seam(C_Input_RunFrame, &p); }
/* SteamInput002 declares Init()/RunFrame() with NO arguments; 006 added
 * bExplicitlyCallRunFrame / bReservedValue. Same method name, different
 * signature — so on i386 one thunk cannot serve both versions: it would pop 4
 * bytes the 002 caller never pushed. Slot order is not the only thing that
 * drifts between versions (#20). */
static uint8_t THISCALL iin_Init_002(struct w_iface *s)
{ struct sp_input_init p; p.handle = s->handle; p.explicit_runframe = 0; p.ret = 0;
  seam(C_Input_Init, &p); dbg("shim: SteamInput002 Init() -> %d", p.ret);
  return (uint8_t)p.ret; }
static void THISCALL iin_RunFrame_002(struct w_iface *s)
{ struct sp_input_runframe p; p.handle = s->handle; p.reserved = 0; seam(C_Input_RunFrame, &p); }

static uint8_t THISCALL iin_BNewDataAvailable(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_BNewDataAvailable, &p); return (uint8_t)p.ret; }
static int32_t THISCALL iin_GetConnectedControllers(struct w_iface *s, uint64_t *out)
{ struct sp_input_handles p; p.handle = s->handle; p.out = (uint64_t)(uintptr_t)out; p.ret = 0;
  seam(C_Input_GetConnectedControllers, &p); return p.ret; }

static void build_vtables(void)
{
    vt_fill_stubs();
#ifndef __i386__
    fill_generic();
#endif

    wire_getters("SteamClient017");
    wire_getters("SteamClient020");

    /* ISteamClient -> SteamClient017, SteamClient020 */
    wire("SteamClient017", "CreateSteamPipe", (const void *)ic_CreateSteamPipe);
    wire("SteamClient020", "CreateSteamPipe", (const void *)ic_CreateSteamPipe);
    wire("SteamClient017", "BReleaseSteamPipe", (const void *)ic_BReleaseSteamPipe);
    wire("SteamClient020", "BReleaseSteamPipe", (const void *)ic_BReleaseSteamPipe);
    wire("SteamClient017", "ConnectToGlobalUser", (const void *)ic_ConnectToGlobalUser);
    wire("SteamClient020", "ConnectToGlobalUser", (const void *)ic_ConnectToGlobalUser);
    wire("SteamClient017", "ReleaseUser", (const void *)ic_ReleaseUser);
    wire("SteamClient020", "ReleaseUser", (const void *)ic_ReleaseUser);
    wire("SteamClient017", "GetISteamUser", (const void *)ic_GetISteamUser);
    wire("SteamClient020", "GetISteamUser", (const void *)ic_GetISteamUser);
    wire("SteamClient017", "GetISteamFriends", (const void *)ic_GetISteamFriends);
    wire("SteamClient020", "GetISteamFriends", (const void *)ic_GetISteamFriends);
    wire("SteamClient017", "GetISteamUtils", (const void *)ic_GetISteamUtils);
    wire("SteamClient020", "GetISteamUtils", (const void *)ic_GetISteamUtils);
    wire("SteamClient017", "GetISteamGenericInterface", (const void *)ic_GetISteamGenericInterface);
    wire("SteamClient020", "GetISteamGenericInterface", (const void *)ic_GetISteamGenericInterface);
    wire("SteamClient017", "GetISteamUserStats", (const void *)ic_GetISteamUserStats);
    wire("SteamClient020", "GetISteamUserStats", (const void *)ic_GetISteamUserStats);
    wire("SteamClient017", "GetISteamApps", (const void *)ic_GetISteamApps);
    wire("SteamClient020", "GetISteamApps", (const void *)ic_GetISteamApps);

    /* ISteamUser -> SteamUser021 */
    wire("SteamUser021", "GetHSteamUser", (const void *)iu_GetHSteamUser);
    wire("SteamUser021", "BLoggedOn", (const void *)iu_BLoggedOn);
    wire("SteamUser021", "GetSteamID", (const void *)iu_GetSteamID);
    wire("SteamUser021", "GetUserDataFolder", (const void *)iu_GetUserDataFolder);
    wire("SteamUser021", "RequestEncryptedAppTicket", (const void *)iu_RequestEncryptedAppTicket);
    wire("SteamUser021", "GetEncryptedAppTicket", (const void *)iu_GetEncryptedAppTicket);

    /* ISteamUtils -> SteamUtils010 */
    wire("SteamUtils010", "GetSecondsSinceAppActive", (const void *)iut_GetSecondsSinceAppActive);
    wire("SteamUtils010", "GetSecondsSinceComputerActive", (const void *)iut_GetSecondsSinceComputerActive);
    wire("SteamUtils010", "GetConnectedUniverse", (const void *)iut_GetConnectedUniverse);
    wire("SteamUtils010", "GetServerRealTime", (const void *)iut_GetServerRealTime);
    wire("SteamUtils010", "GetIPCountry", (const void *)iut_GetIPCountry);
    wire("SteamUtils010", "GetCurrentBatteryPower", (const void *)iut_GetCurrentBatteryPower);
    wire("SteamUtils010", "GetAppID", (const void *)iut_GetAppID);
    wire("SteamUtils010", "IsAPICallCompleted", (const void *)iut_IsAPICallCompleted);
    wire("SteamUtils010", "GetAPICallFailureReason", (const void *)iut_GetAPICallFailureReason);
    wire("SteamUtils010", "GetAPICallResult", (const void *)iut_GetAPICallResult);
    wire("SteamUtils010", "RunFrame", (const void *)iut_RunFrame);
    wire("SteamUtils010", "GetIPCCallCount", (const void *)iut_GetIPCCallCount);
    wire("SteamUtils010", "GetSteamUILanguage", (const void *)iut_GetSteamUILanguage);

    /* ISteamUserStats -> STEAMUSERSTATS_INTERFACE_VERSION012 */
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "RequestCurrentStats", (const void *)is_RequestCurrentStats);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievement", (const void *)is_GetAchievement);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "SetAchievement", (const void *)is_SetAchievement);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "ClearAchievement", (const void *)is_ClearAchievement);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementAndUnlockTime", (const void *)is_GetAchievementAndUnlockTime);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "StoreStats", (const void *)is_StoreStats);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementDisplayAttribute", (const void *)is_GetAchievementDisplayAttribute);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "GetNumAchievements", (const void *)is_GetNumAchievements);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementName", (const void *)is_GetAchievementName);
    wire("STEAMUSERSTATS_INTERFACE_VERSION012", "ResetAllStats", (const void *)is_ResetAllStats);

    /* ISteamApps -> STEAMAPPS_INTERFACE_VERSION008 */
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribed", (const void *)ia_BIsSubscribed);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsLowViolence", (const void *)ia_BIsLowViolence);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsCybercafe", (const void *)ia_BIsCybercafe);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsVACBanned", (const void *)ia_BIsVACBanned);
    wire("STEAMAPPS_INTERFACE_VERSION008", "GetCurrentGameLanguage", (const void *)ia_GetCurrentGameLanguage);
    wire("STEAMAPPS_INTERFACE_VERSION008", "GetAvailableGameLanguages", (const void *)ia_GetAvailableGameLanguages);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribedApp", (const void *)ia_BIsSubscribedApp);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsDlcInstalled", (const void *)ia_BIsDlcInstalled);
    wire("STEAMAPPS_INTERFACE_VERSION008", "GetEarliestPurchaseUnixTime", (const void *)ia_GetEarliestPurchaseUnixTime);
    wire("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribedFromFreeWeekend", (const void *)ia_BIsSubscribedFromFreeWeekend);
    wire("STEAMAPPS_INTERFACE_VERSION008", "GetAppOwner", (const void *)ia_GetAppOwner);
    wire("STEAMAPPS_INTERFACE_VERSION008", "GetLaunchQueryParam", (const void *)ia_GetLaunchQueryParam);

    /* ISteamInput -> SteamInput002, SteamInput006 */
    wire("SteamInput002", "Init", (const void *)iin_Init_002);
    wire("SteamInput006", "Init", (const void *)iin_Init);
    wire("SteamInput002", "Shutdown", (const void *)iin_Shutdown);
    wire("SteamInput006", "Shutdown", (const void *)iin_Shutdown);
    wire("SteamInput002", "RunFrame", (const void *)iin_RunFrame_002);
    wire("SteamInput006", "RunFrame", (const void *)iin_RunFrame);
    /* SteamInput002.BNewDataAvailable: not in this version */
    wire("SteamInput006", "BNewDataAvailable", (const void *)iin_BNewDataAvailable);
    wire("SteamInput002", "GetConnectedControllers", (const void *)iin_GetConnectedControllers);
    wire("SteamInput006", "GetConnectedControllers", (const void *)iin_GetConnectedControllers);

}

/* ---- flat exports ------------------------------------------------------- */
__declspec(dllexport) void *CreateInterface(const char *name, int *returnCode)
{
    struct sp_create_interface p; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
    dbg("shim: CreateInterface(\"%s\")", name ? name : "(null)");
    seam(C_CreateInterface, &p);
    if (returnCode) *returnCode = p.ret ? 0 : 1;   /* 0 = k_EInterfaceOK */
    return wrap(p.ret, vt_resolve(name));
}

/* CallbackMsg_t is the one struct on this path that is NOT bitness-neutral,
 * because it carries a pointer:
 *
 *     win32 : { int user; int cb; uint8 *param; int cub; }   16 bytes, param @8
 *     x86_64: { int user; int cb; uint8 *param; int cub; }   24 bytes, param @8
 *
 * #11 could forward the game's own CallbackMsg_t straight through and let the
 * dylib fill it, because macOS x86_64 and Windows x64 agree byte for byte. A
 * 32-bit caller agrees on neither the size nor, more seriously, the payload
 * address: m_pubParam points into the dylib's heap above 4 GB. So on i386 we
 * hand the native side OUR 64-bit struct, then copy the payload down into PE
 * memory and hand the game a pointer it can actually dereference.
 *
 * The payload STRUCTS need no conversion: Valve picks callback packing by
 * PLATFORM, not bitness (PACK_SMALL on macOS, PACK_LARGE on Windows), so a
 * win32 game sees exactly what a win64 game sees — and 1101/1102/1103 are
 * byte-identical across all four ABIs regardless (#3 §6). */
#ifdef __i386__
struct cbmsg32 { int32_t user; int32_t cb; uint32_t param; int32_t cub; };
/* Trailing pad pins this at 24 whether the 32-bit compiler aligns uint64 to 4
 * or to 8; both put param @8 and cub @16, which is the x86_64 layout. */
struct cbmsg64 { int32_t user; int32_t cb; uint64_t param; int32_t cub; int32_t _pad; };
_Static_assert(sizeof(struct cbmsg64) == 24, "cbmsg64 must match the x86_64 CallbackMsg_t");
_Static_assert(sizeof(struct cbmsg32) == 16, "cbmsg32 must match the win32 CallbackMsg_t");

__declspec(dllexport) BOOL Steam_BGetCallback(int32_t pipe, void *msg)
{
    struct tls_scratch *t = tls_get();
    struct cbmsg64 m64;
    struct cbmsg32 *out = (struct cbmsg32 *)msg;
    struct sp_bgetcallback p;

    if (!out || !t) return FALSE;
    memset(&m64, 0, sizeof m64);
    p.pipe = pipe; p.msg = (uint64_t)(uintptr_t)&m64; p.ret = 0;
    seam(C_BGetCallback, &p);
    if (!p.ret) return FALSE;

    /* The payload stays alive until Steam_FreeLastCallback, so it is safe to
     * size the buffer from m_cubParam and copy in a second seam call rather
     * than guessing a capacity up front. */
    if (m64.param && m64.cub > 0) {
        if (m64.cub > t->payload_cap) {
            unsigned char *grown = (unsigned char *)(t->payload
                ? HeapReAlloc(GetProcessHeap(), 0, t->payload, m64.cub)
                : HeapAlloc(GetProcessHeap(), 0, m64.cub));
            if (!grown) { dbg("shim: callback payload alloc failed (%d bytes)", m64.cub); return FALSE; }
            t->payload = grown; t->payload_cap = m64.cub;
        }
        struct sp_copymem c;
        c.src = m64.param; c.dst = (uint64_t)(uintptr_t)t->payload; c.len = m64.cub;
        seam(C_CopyMem, &c);
        out->param = (uint32_t)(uintptr_t)t->payload;
    } else {
        out->param = 0;
    }
    out->user = m64.user; out->cb = m64.cb; out->cub = m64.cub;
    dbg("shim: callback %d (%d bytes) -> PE payload %p", m64.cb, m64.cub, (void *)(uintptr_t)out->param);
    return TRUE;
}
#else
__declspec(dllexport) BOOL Steam_BGetCallback(int32_t pipe, void *msg)
{
    struct sp_bgetcallback p; p.pipe = pipe; p.msg = (uint64_t)(uintptr_t)msg; p.ret = 0;
    seam(C_BGetCallback, &p);
    return p.ret ? TRUE : FALSE;
}
#endif
__declspec(dllexport) void Steam_FreeLastCallback(int32_t pipe)
{ struct sp_freelast p; p.pipe = pipe; seam(C_FreeLastCallback, &p); }
__declspec(dllexport) BOOL Steam_GetAPICallResult(int32_t pipe, uint64_t call, void *cb, int cub, int expected, void *failed)
{ struct sp_apicallresult p; p.pipe = pipe; p.call = call; p.cb = (uint64_t)(uintptr_t)cb; p.cub = cub;
  p.expected = expected; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_GetAPICallResult, &p); return p.ret ? TRUE : FALSE; }
__declspec(dllexport) void Steam_ReleaseThreadLocalMemory(int flag)
{ struct sp_release_tls p; p.flag = flag; seam(C_ReleaseThreadLocalMemory, &p); }

/* steam_api64.dll probes these; benign answers keep it from bailing. */
__declspec(dllexport) BOOL Steam_IsKnownInterface(const char *ver) { (void)ver; return TRUE; }
__declspec(dllexport) void Steam_NotifyMissingInterface(int32_t pipe, const char *ver) { (void)pipe; dbg("shim: NotifyMissingInterface(\"%s\")", ver ? ver : "?"); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    /* Thread notifications are deliberately NOT disabled any more: the per-thread
     * scratch (tls_get) is freed on DLL_THREAD_DETACH. DisableThreadLibraryCalls
     * would save a few notifications and leak a block per game thread instead. */
    if (reason == DLL_THREAD_DETACH) { tls_free_current_thread(); return TRUE; }
    if (reason == DLL_PROCESS_DETACH) {
        tls_free_current_thread();
#ifdef __i386__
        if (g_tls != TLS_OUT_OF_INDEXES) { TlsFree(g_tls); g_tls = TLS_OUT_OF_INDEXES; }
#endif
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = (HMODULE)inst;
#ifdef __i386__
        g_tls = TlsAlloc();
        if (g_tls == TLS_OUT_OF_INDEXES) dbg("shim: TlsAlloc failed");
#endif
        build_vtables();
        /* Overlay (#25 / ADR 0003). When the injector puts us into a process at
         * creation, merely being loaded must be enough — nothing here will ever
         * call a seam entry point, because the title may not use Steam at all.
         * Binding the seam is what makes ntdll dlopen our unixlib, and the
         * unixlib's constructor is what dlopens Valve's renderer, so this one
         * call is the whole payload.
         *
         * It has to happen in DllMain, under loader lock, because the deadline
         * is the process's first USER call (winemac.so is demand-loaded — see
         * the study's Addendum 2 B7). ensure_seam only does GetModuleHandle,
         * GetProcAddress and NtQueryVirtualMemory: no LoadLibrary, so it is not
         * the loader-lock hazard that a LoadLibrary here would be. */
        if (GetEnvironmentVariableA("SHIM_OVERLAY", NULL, 0) > 0) {
            int rc = ensure_seam();
            /* Diagnostic for #25: whether the title's own startup has already
             * run by the time we initialise. user32 loaded here means the PE
             * loader has already processed the title's static imports — and if
             * any of those touched USER, winemac.so (and NSApp) beat us. */
            dbg("shim: overlay pre-bind, ensure_seam=%d user32=%s d3d12=%s dxgi=%s", rc,
                GetModuleHandleA("user32.dll") ? "LOADED" : "no",
                GetModuleHandleA("d3d12.dll") ? "LOADED" : "no",
                GetModuleHandleA("dxgi.dll") ? "LOADED" : "no");
            {
                FILE *f = fopen("C:\\overlayinject.log", "a");
                if (f) {
                    fprintf(f, "  payload DllMain: ensure_seam=%d user32=%s d3d12=%s dxgi=%s\n", rc,
                            GetModuleHandleA("user32.dll") ? "LOADED" : "no",
                            GetModuleHandleA("d3d12.dll") ? "LOADED" : "no",
                            GetModuleHandleA("dxgi.dll") ? "LOADED" : "no");
                    fclose(f);
                }
            }
        }
        /* Base address is load-bearing diagnostics: a wine fault is reported as
         * an absolute address, and the only way to tell whether it is ours is to
         * subtract our base and disassemble the offset (#11's method). */
        dump_modules();
        dbg("shim: %s attached, base=%p",
            sizeof(void *) == 8 ? "steamclient64.dll" : "steamclient.dll", (void *)inst);
    }
    return TRUE;
}
