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
};
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    u_create_interface, u_bgetcallback, u_freelast, u_apicallresult, u_release_tls,
    u_client_createpipe, u_client_releasepipe, u_client_connectglobal, u_client_releaseuser, u_client_getgeneric,
    u_user_gethuser, u_user_bloggedon, u_user_getsteamid,
    u_utils_getappid,
    u_stats_request, u_stats_getstati, u_stats_setstati, u_stats_getach, u_stats_setach,
    u_stats_clearach, u_stats_getachtime, u_stats_store, u_stats_numach, u_stats_achname,
    u_stats_reset, u_stats_dispattr,
};
} /* extern "C" */
