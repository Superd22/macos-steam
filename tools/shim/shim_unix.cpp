/* shim_unix.cpp — the unix half of the achievement shim (#11).
 *
 * A thin x86_64 Mach-O dylib named steamclient64.so, discovered by CrossOver's
 * ntdll.so via WINEDLLPATH beside the builtin PE (the #8/#10 loader mechanism).
 * It hosts Valve's real steamclient.dylib IN-PROCESS and answers seam calls by
 * making the actual Steamworks calls in the native (SysV) ABI — the PE half
 * having translated from MSVC. This is the #10 seam, widened from one getter to
 * the whole achievement path.
 *
 * Layout facts that make this small (from #3's Proton study, proven in #10):
 *  - one address space, one arch: pointers and CallbackMsg_t / callback payloads
 *    cross verbatim, so Steam_BGetCallback is a passthrough, not a converter;
 *  - the native interface pointer is an opaque handle; we cast it back to a C++
 *    class of pure virtuals in the dylib's own (declaration/Itanium) order and
 *    let the compiler emit the virtual dispatch.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>

#include <dlfcn.h>
#include <pwd.h>
#include <unistd.h>

#include "shim_abi.h"
#include "steam_ifaces.h"

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);

/* ---- diagnostics -------------------------------------------------------- */
static FILE *g_log;
static void ulog(const char *fmt, ...)
{
    if (!g_log) {
        const char *p = getenv("SHIM_UNIX_LOG");
        g_log = fopen(p ? p : "/tmp/shim_unix.log", "a");
        if (!g_log) return;
        setvbuf(g_log, nullptr, _IONBF, 0);
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
}

/* ---- native library + flat entry points --------------------------------- */
static void *g_dylib;
static CreateInterfaceFn n_CreateInterface;
static bool (*n_BGetCallback)(HSteamPipe, void * /*CallbackMsg_t*/);
static void (*n_FreeLastCallback)(HSteamPipe);
static bool (*n_GetAPICallResult)(HSteamPipe, SteamAPICall_t, void *, int, int, bool *);
static void (*n_ReleaseThreadLocalMemory)(int);
static Fn_BConnected n_BConnected;
static Fn_BLoggedOn  n_BLoggedOn;

static std::string dylib_path()
{
    const char *home = getenv("HOME");
    if (!home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : "/tmp"; }
    return std::string(home) +
        "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib";
}

static bool ensure_dylib()
{
    if (g_dylib) return true;
    std::string path = dylib_path();
    g_dylib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_dylib) { ulog("dlopen FAILED: %s", dlerror()); return false; }
    n_CreateInterface          = (CreateInterfaceFn)dlsym(g_dylib, "CreateInterface");
    n_BGetCallback             = (bool(*)(HSteamPipe,void*))dlsym(g_dylib, "Steam_BGetCallback");
    n_FreeLastCallback         = (void(*)(HSteamPipe))dlsym(g_dylib, "Steam_FreeLastCallback");
    n_GetAPICallResult         = (bool(*)(HSteamPipe,SteamAPICall_t,void*,int,int,bool*))dlsym(g_dylib, "Steam_GetAPICallResult");
    n_ReleaseThreadLocalMemory = (void(*)(int))dlsym(g_dylib, "Steam_ReleaseThreadLocalMemory");
    n_BConnected               = (Fn_BConnected)dlsym(g_dylib, "Steam_BConnected");
    n_BLoggedOn                = (Fn_BLoggedOn)dlsym(g_dylib, "Steam_BLoggedOn");
    ulog("dylib loaded pid=%d CreateInterface=%p BGetCallback=%p GetAPICallResult=%p",
         (int)getpid(), (void*)n_CreateInterface, (void*)n_BGetCallback, (void*)n_GetAPICallResult);
    return n_CreateInterface != nullptr;
}

/* ---- flat exports ------------------------------------------------------- */
static NTSTATUS u_create_interface(void *args)
{
    auto *p = (sp_create_interface *)args;
    p->ret = 0;
    if (!ensure_dylib()) return 0;
    const char *name = (const char *)p->name;
    int rc = 0;
    void *iface = n_CreateInterface(name, &rc);
    p->ret = (uint64_t)iface;
    ulog("CreateInterface(\"%s\") -> %p rc=%d", name ? name : "(null)", iface, rc);
    return 0;
}

static NTSTATUS u_bgetcallback(void *args)
{
    auto *p = (sp_bgetcallback *)args;
    p->ret = 0;
    if (!n_BGetCallback) return 0;
    p->ret = n_BGetCallback(p->pipe, (void *)p->msg) ? 1 : 0;
    return 0;
}
static NTSTATUS u_freelast(void *args)
{
    auto *p = (sp_freelast *)args;
    if (n_FreeLastCallback) n_FreeLastCallback(p->pipe);
    return 0;
}
static NTSTATUS u_apicallresult(void *args)
{
    auto *p = (sp_apicallresult *)args;
    p->ret = 0;
    if (!n_GetAPICallResult) return 0;
    p->ret = n_GetAPICallResult(p->pipe, p->call, (void *)p->cb, p->cub, p->expected, (bool *)p->failed) ? 1 : 0;
    return 0;
}
static NTSTATUS u_release_tls(void *args)
{
    auto *p = (sp_release_tls *)args;
    if (n_ReleaseThreadLocalMemory) n_ReleaseThreadLocalMemory(p->flag);
    return 0;
}

/* ---- ISteamClient ------------------------------------------------------- */
static NTSTATUS u_client_createpipe(void *args)
{
    auto *p = (sp_client_noarg *)args;
    p->ret = ((ISteamClient *)p->handle)->CreateSteamPipe();
    /* first pipe up: log the honest online oracle for provenance (map trap #1) */
    if (n_BConnected) ulog("CreateSteamPipe -> %d ; BConnected=%d BLoggedOn=%d", p->ret,
                           n_BConnected(1, p->ret) ? 1 : 0, n_BLoggedOn ? (n_BLoggedOn(1, p->ret) ? 1 : 0) : -1);
    return 0;
}
static NTSTATUS u_client_releasepipe(void *args)
{
    auto *p = (sp_client_pipe *)args;
    p->ret = ((ISteamClient *)p->handle)->BReleaseSteamPipe(p->pipe) ? 1 : 0;
    return 0;
}
static NTSTATUS u_client_connectglobal(void *args)
{
    auto *p = (sp_client_pipe *)args;
    p->ret = ((ISteamClient *)p->handle)->ConnectToGlobalUser(p->pipe);
    ulog("ConnectToGlobalUser(%d) -> %d", p->pipe, p->ret);
    return 0;
}
static NTSTATUS u_client_releaseuser(void *args)
{
    auto *p = (sp_client_releaseu *)args;
    ((ISteamClient *)p->handle)->ReleaseUser(p->pipe, p->user);
    return 0;
}
static NTSTATUS u_client_getgeneric(void *args)
{
    auto *p = (sp_client_getgen *)args;
    const char *ver = (const char *)p->ver;
    void *iface = ((ISteamClient *)p->handle)->GetISteamGenericInterface(p->user, p->pipe, ver);
    p->ret = (uint64_t)iface;
    ulog("GetISteamGenericInterface(u=%d,pipe=%d,\"%s\") -> %p", p->user, p->pipe, ver ? ver : "(null)", iface);
    return 0;
}

/* ---- ISteamUser --------------------------------------------------------- */
static NTSTATUS u_user_gethuser(void *args)
{ auto *p = (sp_user_i32 *)args; p->ret = ((ISteamUser *)p->handle)->GetHSteamUser(); return 0; }
static NTSTATUS u_user_bloggedon(void *args)
{ auto *p = (sp_user_i32 *)args; p->ret = ((ISteamUser *)p->handle)->BLoggedOn() ? 1 : 0; return 0; }
static NTSTATUS u_user_getsteamid(void *args)
{ auto *p = (sp_user_u64 *)args; p->ret = (uint64_t)((ISteamUser *)p->handle)->GetSteamID(); return 0; }

/* ---- ISteamUtils -------------------------------------------------------- */
static NTSTATUS u_utils_getappid(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = ((ISteamUtils010 *)p->handle)->GetAppID();
  ulog("GetAppID() -> %u", p->ret); return 0; }

/* ---- ISteamUserStats ---------------------------------------------------- */
#define STATS(h) ((ISteamUserStats012 *)(h))
static NTSTATUS u_stats_request(void *args)
{ auto *p = (sp_stats_noarg *)args; p->ret = STATS(p->handle)->RequestCurrentStats() ? 1 : 0; return 0; }
static NTSTATUS u_stats_getstati(void *args)
{ auto *p = (sp_stats_statint *)args; p->ret = STATS(p->handle)->GetStatI((const char*)p->name, (int32_t*)p->data) ? 1 : 0; return 0; }
static NTSTATUS u_stats_setstati(void *args)
{ auto *p = (sp_stats_statint *)args; p->ret = STATS(p->handle)->SetStatI((const char*)p->name, p->val) ? 1 : 0; return 0; }
static NTSTATUS u_stats_getach(void *args)
{ auto *p = (sp_stats_getach *)args; p->ret = STATS(p->handle)->GetAchievement((const char*)p->name, (bool*)p->achieved) ? 1 : 0; return 0; }
static NTSTATUS u_stats_setach(void *args)
{ auto *p = (sp_stats_name *)args; p->ret = STATS(p->handle)->SetAchievement((const char*)p->name) ? 1 : 0;
  ulog("SetAchievement(\"%s\") -> %d", (const char*)p->name, p->ret); return 0; }
static NTSTATUS u_stats_clearach(void *args)
{ auto *p = (sp_stats_name *)args; p->ret = STATS(p->handle)->ClearAchievement((const char*)p->name) ? 1 : 0; return 0; }
static NTSTATUS u_stats_getachtime(void *args)
{ auto *p = (sp_stats_getachtime *)args;
  p->ret = STATS(p->handle)->GetAchievementAndUnlockTime((const char*)p->name, (bool*)p->achieved, (uint32_t*)p->unlock) ? 1 : 0; return 0; }
static NTSTATUS u_stats_store(void *args)
{ auto *p = (sp_stats_noarg *)args; p->ret = STATS(p->handle)->StoreStats() ? 1 : 0;
  ulog("StoreStats() -> %d", p->ret); return 0; }
static NTSTATUS u_stats_numach(void *args)
{ auto *p = (sp_stats_u32ret *)args; p->ret = STATS(p->handle)->GetNumAchievements(); return 0; }
static NTSTATUS u_stats_achname(void *args)
{ auto *p = (sp_stats_nameidx *)args; p->ret = (uint64_t)STATS(p->handle)->GetAchievementName(p->idx); return 0; }
static NTSTATUS u_stats_reset(void *args)
{ auto *p = (sp_stats_reset *)args; p->ret = STATS(p->handle)->ResetAllStats(p->achievements_too != 0) ? 1 : 0;
  ulog("ResetAllStats(%d) -> %d", p->achievements_too, p->ret); return 0; }
static NTSTATUS u_stats_dispattr(void *args)
{ auto *p = (sp_stats_dispattr *)args;
  p->ret = (uint64_t)STATS(p->handle)->GetAchievementDisplayAttribute((const char*)p->name, (const char*)p->key); return 0; }

/* ---- ISteamApps (VERSION008) — forwarded so Mars gets real ownership/language
 * answers instead of NULL/0 stubs (the #12 boot crash). ------------------------ */
#define APPS(h) ((ISteamApps008 *)(h))
static NTSTATUS u_apps_bissubscribed(void *args)
{ auto *p = (sp_apps_bool *)args; p->ret = APPS(p->handle)->BIsSubscribed() ? 1 : 0;
  ulog("BIsSubscribed() -> %d", p->ret); return 0; }
static NTSTATUS u_apps_bislowviolence(void *args)
{ auto *p = (sp_apps_bool *)args; p->ret = APPS(p->handle)->BIsLowViolence() ? 1 : 0; return 0; }
static NTSTATUS u_apps_biscybercafe(void *args)
{ auto *p = (sp_apps_bool *)args; p->ret = APPS(p->handle)->BIsCybercafe() ? 1 : 0; return 0; }
static NTSTATUS u_apps_bisvacbanned(void *args)
{ auto *p = (sp_apps_bool *)args; p->ret = APPS(p->handle)->BIsVACBanned() ? 1 : 0; return 0; }
static NTSTATUS u_apps_getlang(void *args)
{ auto *p = (sp_apps_str *)args; p->ret = (uint64_t)APPS(p->handle)->GetCurrentGameLanguage();
  ulog("GetCurrentGameLanguage() -> %s", p->ret ? (const char*)p->ret : "(null)"); return 0; }
static NTSTATUS u_apps_getlangs(void *args)
{ auto *p = (sp_apps_str *)args; p->ret = (uint64_t)APPS(p->handle)->GetAvailableGameLanguages(); return 0; }
static NTSTATUS u_apps_bissubscribedapp(void *args)
{ auto *p = (sp_apps_appid_bool *)args; p->ret = APPS(p->handle)->BIsSubscribedApp(p->appid) ? 1 : 0;
  ulog("BIsSubscribedApp(%u) -> %d", p->appid, p->ret); return 0; }
static NTSTATUS u_apps_bisdlcinstalled(void *args)
{ auto *p = (sp_apps_appid_bool *)args; p->ret = APPS(p->handle)->BIsDlcInstalled(p->appid) ? 1 : 0; return 0; }
static NTSTATUS u_apps_earliest(void *args)
{ auto *p = (sp_apps_appid_u32 *)args; p->ret = APPS(p->handle)->GetEarliestPurchaseUnixTime(p->appid); return 0; }
static NTSTATUS u_apps_freeweekend(void *args)
{ auto *p = (sp_apps_bool *)args; p->ret = APPS(p->handle)->BIsSubscribedFromFreeWeekend() ? 1 : 0; return 0; }
static NTSTATUS u_apps_getappowner(void *args)
{ auto *p = (sp_apps_u64 *)args; p->ret = (uint64_t)APPS(p->handle)->GetAppOwner(); return 0; }
static NTSTATUS u_apps_launchqueryparam(void *args)
{ auto *p = (sp_apps_qparam *)args;
  p->ret = (uint64_t)(uintptr_t)APPS(p->handle)->GetLaunchQueryParam((const char *)p->key); return 0; }

/* ---- ISteamUser (appended) ---------------------------------------------- */
static NTSTATUS u_user_getuserdatafolder(void *args)
{ auto *p = (sp_user_datafolder *)args;
  p->ret = ((ISteamUser *)p->handle)->GetUserDataFolder((char *)p->buf, p->len) ? 1 : 0; return 0; }

/* ---- ISteamUtils (VERSION010) — the rest of the interface. GetIPCountry and
 * GetSteamUILanguage are the const char* returns Mars dereferences; the numeric
 * getters are forwarded because a real value costs the same as a fake one.
 * Deliberately NOT forwarded: SetWarningMessageHook (16) takes a PE function
 * pointer, which would need Proton's deferred-upcall queue to call back safely,
 * and the overlay/VR/Deck predicates, where our honest answer is the stub's
 * false. --------------------------------------------------------------------- */
#define UTILS(h) ((ISteamUtils010 *)(h))
static NTSTATUS u_utils_secsappactive(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = UTILS(p->handle)->GetSecondsSinceAppActive(); return 0; }
static NTSTATUS u_utils_secscomputeractive(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = UTILS(p->handle)->GetSecondsSinceComputerActive(); return 0; }
static NTSTATUS u_utils_connecteduniverse(void *args)
{ auto *p = (sp_utils_i32 *)args; p->ret = UTILS(p->handle)->GetConnectedUniverse(); return 0; }
static NTSTATUS u_utils_serverrealtime(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = UTILS(p->handle)->GetServerRealTime(); return 0; }
static NTSTATUS u_utils_ipcountry(void *args)
{ auto *p = (sp_utils_str *)args; p->ret = (uint64_t)(uintptr_t)UTILS(p->handle)->GetIPCountry();
  ulog("GetIPCountry() -> %s", p->ret ? (const char *)(uintptr_t)p->ret : "(null)"); return 0; }
static NTSTATUS u_utils_batterypower(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = UTILS(p->handle)->GetCurrentBatteryPower(); return 0; }
static NTSTATUS u_utils_apicallcompleted(void *args)
{ auto *p = (sp_utils_call *)args;
  p->ret = UTILS(p->handle)->IsAPICallCompleted(p->call, (bool *)p->failed) ? 1 : 0; return 0; }
static NTSTATUS u_utils_apicallfailure(void *args)
{ auto *p = (sp_utils_callfail *)args; p->ret = UTILS(p->handle)->GetAPICallFailureReason(p->call); return 0; }
/* Callback payloads are byte-identical macOS<->Windows x64 (#11), so the result
 * buffer crosses verbatim — no struct conversion, unlike Proton's generated path. */
static NTSTATUS u_utils_apicallresult(void *args)
{ auto *p = (sp_utils_callres *)args;
  p->ret = UTILS(p->handle)->GetAPICallResult(p->call, (void *)p->buf, p->cub, p->expected,
                                              (bool *)p->failed) ? 1 : 0; return 0; }
static NTSTATUS u_utils_runframe(void *args)
{ auto *p = (sp_utils_void *)args; UTILS(p->handle)->RunFrame(); return 0; }
static NTSTATUS u_utils_ipccallcount(void *args)
{ auto *p = (sp_utils_u32 *)args; p->ret = UTILS(p->handle)->GetIPCCallCount(); return 0; }
static NTSTATUS u_utils_uilanguage(void *args)
{ auto *p = (sp_utils_str *)args; p->ret = (uint64_t)(uintptr_t)UTILS(p->handle)->GetSteamUILanguage();
  ulog("GetSteamUILanguage() -> %s", p->ret ? (const char *)(uintptr_t)p->ret : "(null)"); return 0; }

extern "C" {
NTSTATUS __wine_unix_lib_init(void) { return 0; }

/* Order MUST match enum shim_call in shim_abi.h. extern + used are load-bearing:
 * a file-scope const array is internal linkage in C++, and ntdll.so resolves
 * these by name via dlsym. */
extern const unixlib_entry_t __wine_unix_call_funcs[] __attribute__((used, visibility("default")));
extern const unixlib_entry_t __wine_unix_call_wow64_funcs[] __attribute__((used, visibility("default")));

const unixlib_entry_t __wine_unix_call_funcs[] = {
    u_create_interface, u_bgetcallback, u_freelast, u_apicallresult, u_release_tls,
    u_client_createpipe, u_client_releasepipe, u_client_connectglobal, u_client_releaseuser, u_client_getgeneric,
    u_user_gethuser, u_user_bloggedon, u_user_getsteamid,
    u_utils_getappid,
    u_stats_request, u_stats_getstati, u_stats_setstati, u_stats_getach, u_stats_setach,
    u_stats_clearach, u_stats_getachtime, u_stats_store, u_stats_numach, u_stats_achname,
    u_stats_reset, u_stats_dispattr,
    u_apps_bissubscribed, u_apps_bislowviolence, u_apps_biscybercafe, u_apps_bisvacbanned,
    u_apps_getlang, u_apps_getlangs, u_apps_bissubscribedapp, u_apps_bisdlcinstalled,
    u_apps_earliest, u_apps_freeweekend,
    u_apps_getappowner, u_apps_launchqueryparam,
    u_user_getuserdatafolder,
    u_utils_secsappactive, u_utils_secscomputeractive, u_utils_connecteduniverse,
    u_utils_serverrealtime, u_utils_ipcountry, u_utils_batterypower,
    u_utils_apicallcompleted, u_utils_apicallfailure, u_utils_apicallresult,
    u_utils_runframe, u_utils_ipccallcount, u_utils_uilanguage,
};
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    u_create_interface, u_bgetcallback, u_freelast, u_apicallresult, u_release_tls,
    u_client_createpipe, u_client_releasepipe, u_client_connectglobal, u_client_releaseuser, u_client_getgeneric,
    u_user_gethuser, u_user_bloggedon, u_user_getsteamid,
    u_utils_getappid,
    u_stats_request, u_stats_getstati, u_stats_setstati, u_stats_getach, u_stats_setach,
    u_stats_clearach, u_stats_getachtime, u_stats_store, u_stats_numach, u_stats_achname,
    u_stats_reset, u_stats_dispattr,
    u_apps_bissubscribed, u_apps_bislowviolence, u_apps_biscybercafe, u_apps_bisvacbanned,
    u_apps_getlang, u_apps_getlangs, u_apps_bissubscribedapp, u_apps_bisdlcinstalled,
    u_apps_earliest, u_apps_freeweekend,
    u_apps_getappowner, u_apps_launchqueryparam,
    u_user_getuserdatafolder,
    u_utils_secsappactive, u_utils_secscomputeractive, u_utils_connecteduniverse,
    u_utils_serverrealtime, u_utils_ipcountry, u_utils_batterypower,
    u_utils_apicallcompleted, u_utils_apicallfailure, u_utils_apicallresult,
    u_utils_runframe, u_utils_ipccallcount, u_utils_uilanguage,
};

/* A short array silently maps every opcode past the end onto garbage, and a
 * long one hides a missing handler — both look like a hang, not an error (#11). */
static_assert(sizeof(__wine_unix_call_funcs) / sizeof(*__wine_unix_call_funcs) == C_COUNT,
              "__wine_unix_call_funcs is out of step with enum shim_call");
static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(*__wine_unix_call_wow64_funcs) == C_COUNT,
              "__wine_unix_call_wow64_funcs is out of step with enum shim_call");
} /* extern "C" */
