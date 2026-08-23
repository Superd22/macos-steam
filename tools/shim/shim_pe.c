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
#include <stdint.h>
#include <stdio.h>

#include "shim_abi.h"

typedef UINT64 unixlib_handle_t;
typedef LONG   NTSTATUS_T;
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS_T (WINAPI *pNtQueryVirtualMemory)(HANDLE, PVOID, DWORD, PVOID, SIZE_T, SIZE_T *);
typedef NTSTATUS_T (WINAPI *pWineUnixCall)(unixlib_handle_t, unsigned int, void *);

static HMODULE          self_module;
static unixlib_handle_t g_handle;
static pWineUnixCall    g_unix_call;

static void dbg(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
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

/* ---- PE-side interface object: MSVC vtable ptr @0, opaque native handle @8 -- */
struct w_iface { const void **vtable; uint64_t handle; };

/* vtables, sized generously; unmapped slots default to a per-interface UNKNOWN
 * stub that logs and returns 0 (never crashes: MS-x64 is caller-cleanup, so a
 * 0-arg stub is safe under any real signature, and 0/NULL is a benign return). */
#define VT_SLOTS 64
static const void *vt_client[VT_SLOTS];
static const void *vt_user[VT_SLOTS];
static const void *vt_stats[VT_SLOTS];
static const void *vt_utils[VT_SLOTS];
static const void *vt_apps[VT_SLOTS];
static const void *vt_friends[VT_SLOTS];
static const void *vt_generic[VT_SLOTS];
static const void *vt_input[VT_SLOTS];
static const void *vt_controller[VT_SLOTS];

/* Per-slot numbered stubs, so an unmapped call names its exact vtable index.
 * MS-x64 is caller-cleanup, so a 0-arg stub is safe under any real signature. */
#define SLOTLIST X(0)X(1)X(2)X(3)X(4)X(5)X(6)X(7)X(8)X(9)X(10)X(11)X(12)X(13)X(14)X(15) \
    X(16)X(17)X(18)X(19)X(20)X(21)X(22)X(23)X(24)X(25)X(26)X(27)X(28)X(29)X(30)X(31) \
    X(32)X(33)X(34)X(35)X(36)X(37)X(38)X(39)X(40)X(41)X(42)X(43)X(44)X(45)X(46)X(47) \
    X(48)X(49)X(50)X(51)X(52)X(53)X(54)X(55)X(56)X(57)X(58)X(59)X(60)X(61)X(62)X(63)
static uint64_t clientstub_impl(int n){dbg("shim: ISteamClient slot %d (unmapped)",n);return 0;}
static uint64_t utilsstub_impl(int n){dbg("shim: ISteamUtils slot %d (unmapped)",n);return 0;}
static uint64_t userstub_impl(int n){dbg("shim: ISteamUser slot %d (unmapped)",n);return 0;}
static uint64_t statsstub_impl(int n){dbg("shim: ISteamUserStats slot %d (unmapped)",n);return 0;}
static uint64_t appsstub_impl(int n){dbg("shim: ISteamApps slot %d (unmapped)",n);return 0;}
static uint64_t friendsstub_impl(int n){dbg("shim: ISteamFriends slot %d (unmapped)",n);return 0;}
static uint64_t genericstub_impl(int n){dbg("shim: generic iface slot %d (unmapped)",n);return 0;}
static uint64_t inputstub_impl(int n){dbg("shim: ISteamInput slot %d (unmapped)",n);return 0;}
static uint64_t controllerstub_impl(int n){dbg("shim: ISteamController slot %d (unmapped)",n);return 0;}
#define X(n) static uint64_t cstub_##n(void){return clientstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t ustub_##n(void){return utilsstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t usrstub_##n(void){return userstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t sstub_##n(void){return statsstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t astub_##n(void){return appsstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t fstub_##n(void){return friendsstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t gstub_##n(void){return genericstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t istub_##n(void){return inputstub_impl(n);}
SLOTLIST
#undef X
#define X(n) static uint64_t ctstub_##n(void){return controllerstub_impl(n);}
SLOTLIST
#undef X
#define X(n) (const void *)cstub_##n,
static const void *cstubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)ustub_##n,
static const void *ustubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)usrstub_##n,
static const void *usrstubs[64] = { SLOTLIST };
#undef X
#define X(n) (const void *)sstub_##n,
static const void *sstubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)astub_##n,
static const void *astubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)fstub_##n,
static const void *fstubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)gstub_##n,
static const void *gstubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)istub_##n,
static const void *istubs[64]   = { SLOTLIST };
#undef X
#define X(n) (const void *)ctstub_##n,
static const void *ctstubs[64]  = { SLOTLIST };
#undef X

/* small w_iface cache so repeated acquisition of the same native handle returns
 * a stable pointer (games and steam_api64.dll compare interface pointers). */
static struct { uint64_t handle; const void **vt; struct w_iface *w; } g_cache[32];
static int g_ncache;

static struct w_iface *wrap(uint64_t handle, const void **vt)
{
    int i;
    if (!handle) return NULL;
    for (i = 0; i < g_ncache; i++)
        if (g_cache[i].handle == handle && g_cache[i].vt == vt) return g_cache[i].w;
    struct w_iface *w = (struct w_iface *)HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtable = vt; w->handle = handle;
    if (g_ncache < 32) { g_cache[g_ncache].handle = handle; g_cache[g_ncache].vt = vt; g_cache[g_ncache].w = w; g_ncache++; }
    return w;
}

static const void **vt_for_version(const char *ver)
{
    if (!ver) return vt_generic;
    if (!strncmp(ver, "SteamClient", 11))          return vt_client;
    if (!strncmp(ver, "STEAMUSERSTATS", 14))       return vt_stats;
    if (!strncmp(ver, "SteamUserStats", 14))       return vt_stats;
    if (!strncmp(ver, "SteamUser", 9))             return vt_user;   /* after UserStats */
    if (!strncmp(ver, "SteamUtils", 10))           return vt_utils;
    if (!strncmp(ver, "STEAMUTILS", 10))           return vt_utils;
    if (!strncmp(ver, "SteamApps", 9))             return vt_apps;
    if (!strncmp(ver, "STEAMAPPS", 9))             return vt_apps;
    if (!strncmp(ver, "SteamFriends", 12))         return vt_friends;
    if (!strncmp(ver, "SteamInput", 10))           return vt_input;
    if (!strncmp(ver, "SteamController", 15))      return vt_controller;
    return vt_generic;
}

/* ---- ISteamClient thunks ------------------------------------------------ */
static int32_t ic_CreateSteamPipe(struct w_iface *s)
{ struct sp_client_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Client_CreateSteamPipe, &p);
  dbg("shim: CreateSteamPipe -> %d", p.ret); return p.ret; }
static uint8_t ic_BReleaseSteamPipe(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_BReleaseSteamPipe, &p); return (uint8_t)p.ret; }
static int32_t ic_ConnectToGlobalUser(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_ConnectToGlobalUser, &p);
  dbg("shim: ConnectToGlobalUser(%d) -> %d", pipe, p.ret); return p.ret; }
static void ic_ReleaseUser(struct w_iface *s, int32_t pipe, int32_t user)
{ struct sp_client_releaseu p; p.handle = s->handle; p.pipe = pipe; p.user = user; seam(C_Client_ReleaseUser, &p); }

static void *acquire(struct w_iface *s, int32_t user, int32_t pipe, const char *ver)
{
    struct sp_client_getgen p; p.handle = s->handle; p.ver = (uint64_t)(uintptr_t)ver;
    p.user = user; p.pipe = pipe; p.ret = 0;
    seam(C_Client_GetGeneric, &p);
    dbg("shim: acquire(\"%s\") native=%p", ver ? ver : "(null)", (void*)p.ret);
    return wrap(p.ret, vt_for_version(ver));
}
static void *ic_GetISteamUser(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void *ic_GetISteamUtils(struct w_iface *s, int32_t pipe, const char *ver)                { return acquire(s, 0, pipe, ver); }
static void *ic_GetISteamUserStats(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }
static void *ic_GetISteamApps(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void *ic_GetISteamFriends(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)   { return acquire(s, u, pipe, ver); }
static void *ic_GetISteamGenericInterface(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }

/* ---- ISteamUser thunks -------------------------------------------------- */
static int32_t iu_GetHSteamUser(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetHSteamUser, &p); return p.ret; }
static uint8_t iu_BLoggedOn(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_BLoggedOn, &p); return (uint8_t)p.ret; }
/* GetSteamID returns CSteamID BY VALUE. MSVC uses the sret ABI for it: the caller
 * passes a hidden result-buffer pointer (rdx, after this=rcx) and expects the
 * callee to fill it and RETURN THAT POINTER in rax. The dylib (SysV) returns the
 * value in rax, so the seam value is correct; only this PE thunk must adapt.
 * Getting this wrong makes steam_api64 deref the SteamID as a pointer (page fault). */
static uint64_t *iu_GetSteamID(struct w_iface *s, uint64_t *sret)
{ struct sp_user_u64 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetSteamID, &p);
  *sret = p.ret; return sret; }

/* ---- ISteamUtils thunks ------------------------------------------------- */
static uint32_t iut_GetAppID(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetAppID, &p);
  dbg("shim: GetAppID() -> %u", p.ret); return p.ret; }
static uint32_t iut_GetSecondsSinceAppActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceAppActive, &p); return p.ret; }
static uint32_t iut_GetSecondsSinceComputerActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceComputerActive, &p); return p.ret; }
static int32_t iut_GetConnectedUniverse(struct w_iface *s)
{ struct sp_utils_i32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetConnectedUniverse, &p); return p.ret; }
static uint32_t iut_GetServerRealTime(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetServerRealTime, &p); return p.ret; }
static const char *iut_GetIPCountry(struct w_iface *s)
{ struct sp_utils_str p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCountry, &p);
  dbg("shim: GetIPCountry() -> %s", p.ret ? (const char*)(uintptr_t)p.ret : "(null)");
  return (const char *)(uintptr_t)p.ret; }
static uint8_t iut_GetCurrentBatteryPower(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetCurrentBatteryPower, &p); return (uint8_t)p.ret; }
static uint8_t iut_IsAPICallCompleted(struct w_iface *s, uint64_t call, void *failed)
{ struct sp_utils_call p; p.handle = s->handle; p.call = call; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_IsAPICallCompleted, &p); return (uint8_t)p.ret; }
static int32_t iut_GetAPICallFailureReason(struct w_iface *s, uint64_t call)
{ struct sp_utils_callfail p; p.handle = s->handle; p.call = call; p.ret = 0;
  seam(C_Utils_GetAPICallFailureReason, &p); return p.ret; }
static uint8_t iut_GetAPICallResult(struct w_iface *s, uint64_t call, void *buf, int32_t cub, int32_t expected, void *failed)
{ struct sp_utils_callres p; p.handle = s->handle; p.call = call; p.buf = (uint64_t)(uintptr_t)buf;
  p.cub = cub; p.expected = expected; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_GetAPICallResult, &p); return (uint8_t)p.ret; }
static void iut_RunFrame(struct w_iface *s)
{ struct sp_utils_void p; p.handle = s->handle; seam(C_Utils_RunFrame, &p); }
static uint32_t iut_GetIPCCallCount(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCCallCount, &p); return p.ret; }
static const char *iut_GetSteamUILanguage(struct w_iface *s)
{ struct sp_utils_str p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSteamUILanguage, &p);
  dbg("shim: GetSteamUILanguage() -> %s", p.ret ? (const char*)(uintptr_t)p.ret : "(null)");
  return (const char *)(uintptr_t)p.ret; }

/* ---- ISteamUserStats (v012) thunks -------------------------------------- */
static uint8_t is_RequestCurrentStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_RequestCurrentStats, &p); return (uint8_t)p.ret; }
static uint8_t is_GetAchievement(struct w_iface *s, const char *name, void *achieved)
{ struct sp_stats_getach p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.achieved = (uint64_t)(uintptr_t)achieved; p.ret = 0;
  seam(C_Stats_GetAchievement, &p); return (uint8_t)p.ret; }
static uint8_t is_SetAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_SetAchievement, &p);
  dbg("shim: SetAchievement(\"%s\") -> %d", name, p.ret); return (uint8_t)p.ret; }
static uint8_t is_ClearAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_ClearAchievement, &p); return (uint8_t)p.ret; }
static uint8_t is_GetAchievementAndUnlockTime(struct w_iface *s, const char *name, void *achieved, void *unlock)
{ struct sp_stats_getachtime p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.achieved = (uint64_t)(uintptr_t)achieved; p.unlock = (uint64_t)(uintptr_t)unlock; p.ret = 0;
  seam(C_Stats_GetAchievementAndUnlockTime, &p); return (uint8_t)p.ret; }
static uint8_t is_StoreStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_StoreStats, &p);
  dbg("shim: StoreStats() -> %d", p.ret); return (uint8_t)p.ret; }
static uint32_t is_GetNumAchievements(struct w_iface *s)
{ struct sp_stats_u32ret p; p.handle = s->handle; p.ret = 0; seam(C_Stats_GetNumAchievements, &p); return p.ret; }
static const char *is_GetAchievementName(struct w_iface *s, uint32_t idx)
{ struct sp_stats_nameidx p; p.handle = s->handle; p.idx = idx; p.ret = 0; seam(C_Stats_GetAchievementName, &p); return (const char *)(uintptr_t)p.ret; }
static uint8_t is_ResetAllStats(struct w_iface *s, uint8_t achievements_too)
{ struct sp_stats_reset p; p.handle = s->handle; p.achievements_too = achievements_too; p.ret = 0; seam(C_Stats_ResetAllStats, &p);
  dbg("shim: ResetAllStats(%d) -> %d", achievements_too, p.ret); return (uint8_t)p.ret; }
static const char *is_GetAchievementDisplayAttribute(struct w_iface *s, const char *name, const char *key)
{ struct sp_stats_dispattr p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Stats_GetAchievementDisplayAttribute, &p); return (const char *)(uintptr_t)p.ret; }

/* ---- ISteamApps (VERSION008, slots 0-9) thunks -------------------------- */
static uint8_t ia_BIsSubscribed(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribed, &p);
  dbg("shim: BIsSubscribed() -> %d", p.ret); return (uint8_t)p.ret; }
static uint8_t ia_BIsLowViolence(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsLowViolence, &p); return (uint8_t)p.ret; }
static uint8_t ia_BIsCybercafe(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsCybercafe, &p); return (uint8_t)p.ret; }
static uint8_t ia_BIsVACBanned(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsVACBanned, &p); return (uint8_t)p.ret; }
static const char *ia_GetCurrentGameLanguage(struct w_iface *s)
{ struct sp_apps_str p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetCurrentGameLanguage, &p);
  dbg("shim: GetCurrentGameLanguage() -> %s", p.ret ? (const char*)(uintptr_t)p.ret : "(null)");
  return (const char *)(uintptr_t)p.ret; }
static const char *ia_GetAvailableGameLanguages(struct w_iface *s)
{ struct sp_apps_str p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAvailableGameLanguages, &p);
  return (const char *)(uintptr_t)p.ret; }
static uint8_t ia_BIsSubscribedApp(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsSubscribedApp, &p);
  dbg("shim: BIsSubscribedApp(%u) -> %d", appid, p.ret); return (uint8_t)p.ret; }
static uint8_t ia_BIsDlcInstalled(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsDlcInstalled, &p); return (uint8_t)p.ret; }
static uint32_t ia_GetEarliestPurchaseUnixTime(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_u32 p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_GetEarliestPurchaseUnixTime, &p); return p.ret; }
static uint8_t ia_BIsSubscribedFromFreeWeekend(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribedFromFreeWeekend, &p); return (uint8_t)p.ret; }
/* GetAppOwner returns CSteamID BY VALUE — same MSVC sret ABI as iu_GetSteamID
 * below: fill the caller's hidden buffer and return that pointer, not the value. */
static uint64_t *ia_GetAppOwner(struct w_iface *s, uint64_t *sret)
{ struct sp_apps_u64 p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAppOwner, &p);
  *sret = p.ret; return sret; }
static const char *ia_GetLaunchQueryParam(struct w_iface *s, const char *key)
{ struct sp_apps_qparam p; p.handle = s->handle; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Apps_GetLaunchQueryParam, &p); return (const char *)(uintptr_t)p.ret; }

/* ---- ISteamUser (appended) ---------------------------------------------- */
static uint8_t iu_GetUserDataFolder(struct w_iface *s, char *buf, int32_t len)
{ struct sp_user_datafolder p; p.handle = s->handle; p.buf = (uint64_t)(uintptr_t)buf; p.len = len; p.ret = 0;
  seam(C_User_GetUserDataFolder, &p); return (uint8_t)p.ret; }

/* ---- ISteamInput (VERSION006) thunks ------------------------------------ */
static uint8_t iin_Init(struct w_iface *s, uint8_t explicit_runframe)
{ struct sp_input_init p; p.handle = s->handle; p.explicit_runframe = explicit_runframe; p.ret = 0;
  seam(C_Input_Init, &p); dbg("shim: SteamInput Init(%d) -> %d", explicit_runframe, p.ret);
  return (uint8_t)p.ret; }
static uint8_t iin_Shutdown(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_Shutdown, &p); return (uint8_t)p.ret; }
static void iin_RunFrame(struct w_iface *s, uint8_t reserved)
{ struct sp_input_runframe p; p.handle = s->handle; p.reserved = reserved; seam(C_Input_RunFrame, &p); }
static uint8_t iin_BNewDataAvailable(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_BNewDataAvailable, &p); return (uint8_t)p.ret; }
static int32_t iin_GetConnectedControllers(struct w_iface *s, uint64_t *out)
{ struct sp_input_handles p; p.handle = s->handle; p.out = (uint64_t)(uintptr_t)out; p.ret = 0;
  seam(C_Input_GetConnectedControllers, &p); return p.ret; }

static void build_vtables(void)
{
    int i;
    for (i = 0; i < VT_SLOTS; i++) {
        vt_client[i]  = cstubs[i];
        vt_user[i]    = usrstubs[i];
        vt_stats[i]   = sstubs[i];
        vt_utils[i]   = ustubs[i];
        vt_apps[i]    = astubs[i];
        vt_friends[i] = fstubs[i];
        vt_generic[i] = gstubs[i];
        vt_input[i]      = istubs[i];
        vt_controller[i] = ctstubs[i];
    }
    /* ISteamClient (SteamClient020 order, #3 §7.4 / native-probe steam_min.h) */
    vt_client[0]  = (const void *)ic_CreateSteamPipe;
    vt_client[1]  = (const void *)ic_BReleaseSteamPipe;
    vt_client[2]  = (const void *)ic_ConnectToGlobalUser;
    vt_client[4]  = (const void *)ic_ReleaseUser;
    vt_client[5]  = (const void *)ic_GetISteamUser;
    vt_client[8]  = (const void *)ic_GetISteamFriends;
    vt_client[9]  = (const void *)ic_GetISteamUtils;
    vt_client[12] = (const void *)ic_GetISteamGenericInterface;
    vt_client[13] = (const void *)ic_GetISteamUserStats;
    vt_client[15] = (const void *)ic_GetISteamApps;

    /* ISteamUser (leading slots, stable across versions; proven #10) */
    vt_user[0] = (const void *)iu_GetHSteamUser;
    vt_user[1] = (const void *)iu_BLoggedOn;
    vt_user[2] = (const void *)iu_GetSteamID;

    /* ISteamUtils VERSION010 — full 39-slot layout from Proton's generated MSVC
     * vtable (winISteamUtils.c). Slots left on the numbered stub are deliberate:
     * SetWarningMessageHook (16) would hand the native client a PE function
     * pointer, and the overlay/VR/BigPicture/Deck predicates (17,18,24,26,28,30,
     * 34) are honestly false for us — the overlay is out of scope on the map. */
    vt_utils[0]  = (const void *)iut_GetSecondsSinceAppActive;
    vt_utils[1]  = (const void *)iut_GetSecondsSinceComputerActive;
    vt_utils[2]  = (const void *)iut_GetConnectedUniverse;
    vt_utils[3]  = (const void *)iut_GetServerRealTime;
    vt_utils[4]  = (const void *)iut_GetIPCountry;          /* const char* */
    vt_utils[8]  = (const void *)iut_GetCurrentBatteryPower;
    vt_utils[9]  = (const void *)iut_GetAppID;
    vt_utils[11] = (const void *)iut_IsAPICallCompleted;
    vt_utils[12] = (const void *)iut_GetAPICallFailureReason;
    vt_utils[13] = (const void *)iut_GetAPICallResult;
    vt_utils[14] = (const void *)iut_RunFrame;
    vt_utils[15] = (const void *)iut_GetIPCCallCount;
    vt_utils[23] = (const void *)iut_GetSteamUILanguage;    /* const char* */

    /* ISteamUserStats VERSION012 — MSVC order. Overloaded GetStat/SetStat
     * (slots 1-4) stay stubs; only the achievement path is wired. */
    vt_stats[0]  = (const void *)is_RequestCurrentStats;
    vt_stats[6]  = (const void *)is_GetAchievement;
    vt_stats[7]  = (const void *)is_SetAchievement;
    vt_stats[8]  = (const void *)is_ClearAchievement;
    vt_stats[9]  = (const void *)is_GetAchievementAndUnlockTime;
    vt_stats[10] = (const void *)is_StoreStats;
    vt_stats[12] = (const void *)is_GetAchievementDisplayAttribute;
    vt_stats[14] = (const void *)is_GetNumAchievements;
    vt_stats[15] = (const void *)is_GetAchievementName;
    vt_stats[21] = (const void *)is_ResetAllStats;

    /* ISteamApps VERSION008 (33 slots). Beyond the leading block, only the two
     * returns a game dereferences are wired: GetAppOwner (20, CSteamID by value
     * -> sret) and GetLaunchQueryParam (21, const char*). */
    vt_apps[0] = (const void *)ia_BIsSubscribed;
    vt_apps[1] = (const void *)ia_BIsLowViolence;
    vt_apps[2] = (const void *)ia_BIsCybercafe;
    vt_apps[3] = (const void *)ia_BIsVACBanned;
    vt_apps[4] = (const void *)ia_GetCurrentGameLanguage;
    vt_apps[5] = (const void *)ia_GetAvailableGameLanguages;
    vt_apps[6] = (const void *)ia_BIsSubscribedApp;
    vt_apps[7] = (const void *)ia_BIsDlcInstalled;
    vt_apps[8] = (const void *)ia_GetEarliestPurchaseUnixTime;
    vt_apps[9]  = (const void *)ia_BIsSubscribedFromFreeWeekend;
    vt_apps[20] = (const void *)ia_GetAppOwner;
    vt_apps[21] = (const void *)ia_GetLaunchQueryParam;

    /* ISteamUser: GetUserDataFolder (6) writes into a caller buffer that stays
     * uninitialised under the stub. */
    vt_user[6] = (const void *)iu_GetUserDataFolder;

    /* ISteamInput VERSION006 leading slots. */
    vt_input[0] = (const void *)iin_Init;
    vt_input[1] = (const void *)iin_Shutdown;
    vt_input[3] = (const void *)iin_RunFrame;
    vt_input[5] = (const void *)iin_BNewDataAvailable;
    vt_input[6] = (const void *)iin_GetConnectedControllers;
}

/* ---- flat exports ------------------------------------------------------- */
__declspec(dllexport) void *CreateInterface(const char *name, int *returnCode)
{
    struct sp_create_interface p; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
    dbg("shim: CreateInterface(\"%s\")", name ? name : "(null)");
    seam(C_CreateInterface, &p);
    if (returnCode) *returnCode = p.ret ? 0 : 1;   /* 0 = k_EInterfaceOK */
    return wrap(p.ret, vt_for_version(name));
}

__declspec(dllexport) BOOL Steam_BGetCallback(int32_t pipe, void *msg)
{
    struct sp_bgetcallback p; p.pipe = pipe; p.msg = (uint64_t)(uintptr_t)msg; p.ret = 0;
    seam(C_BGetCallback, &p);
    return p.ret ? TRUE : FALSE;
}
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
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        build_vtables();
        dbg("shim: steamclient64.dll attached");
    }
    return TRUE;
}
