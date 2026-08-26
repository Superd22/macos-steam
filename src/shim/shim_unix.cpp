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
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <climits>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

/* The deploy contract (#32) — generated from src/layout/layout.json. */
#include "shim_paths.h"
#include "shim_policy.h"

#include "shim_abi.h"
#include "steam_ifaces.h"

/* Layout converters for the few Steamworks structs whose Windows and unix forms
 * genuinely differ on x86_64 (#84). Unix-side only: the conversion happens here,
 * where both layouts are in scope, and the PE half never sees a u64_ form. */
#include "gen/shim_gen_convert.h"

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);

/* ---- diagnostics -------------------------------------------------------- */
static FILE *g_log;

/* Open the log for append, without following a symlink and without exposing it
 * to other users. The default used to be /tmp/shim_unix.log; /tmp is
 * world-writable, so on a shared machine anything could pre-plant a symlink
 * there and have us create-or-append to a file of its choosing with the user's
 * privileges, and the log — full of the user's library paths, app ids and
 * Steam ID — was created world-readable. Silent on failure, as before: this is
 * a diagnostic and must never be why a title does not launch. */
static FILE *open_log()
{
    char path[PATH_MAX];
    const char *env = getenv("SHIM_UNIX_LOG");
    int fd;

    if (env && *env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : nullptr; }
        if (!home) return nullptr;
        snprintf(path, sizeof(path), "%s/" SHIM_PATH_LOG_DIR_REL, home);
        mkdir(path, 0700);                       /* ~/Library/Logs always exists */
        strncat(path, "/" SHIM_PATH_LOG_UNIX, sizeof(path) - strlen(path) - 1);
    }
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    return fd < 0 ? nullptr : fdopen(fd, "a");
}

static void ulog(const char *fmt, ...)
{
    if (!g_log) {
        g_log = open_log();
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
    return std::string(home) + "/" SHIM_PATH_STEAM_DYLIB_REL;
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

/* ---- overlay: load Valve's renderer while we are still early enough (#21) --
 *
 * (a2). The renderer must be in the process before winemac.so instantiates
 * NSApplication — measured in attic/overlay-probe/, Addendum 2 §B2: load before
 * NSApp and it installs its five MTLCommandBuffer hooks and arms; load after and
 * it hooks nothing. A dylib constructor here is the earliest point we own: dyld
 * runs it when ntdll.so dlopens this unixlib, which is what SHIM_OVERLAY=1 is
 * betting happens before the display driver comes up.
 *
 * Deliberately not gated on anything else: no Steamworks call has been made yet,
 * and must not be, or we are already too late.
 */
static void *g_overlay;
/* The renderer's own answers to the overlay predicates (#23). Four exports, of
 * which we want three; there is no SetNotificationInset, which is why the inset
 * setter below is accept-and-ignore rather than a forward. */
static bool (*n_IsOverlayEnabled)(void);
static bool (*n_BOverlayNeedsPresent)(void);
static void (*n_SetNotificationPosition)(uint32_t);

static std::string renderer_path()
{
    const char *home = getenv("HOME");
    if (!home) { struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : "/tmp"; }
    return std::string(home) + "/" SHIM_PATH_OVERLAY_DYLIB_REL;
}

__attribute__((constructor)) static void overlay_load(void)
{
    /* ON unless explicitly disabled. The default flipped with #21: the route is
     * proven, so the shipped behaviour is an overlay. `SHIM_OVERLAY=0` opts out.
     *
     * Note the asymmetry this creates, which every caller above now has to
     * respect: *unset* means ON, so a launcher that wants the overlay off must
     * export a literal 0 rather than simply omit the variable. The compat-tool
     * launch script does exactly that in its no-injector branch, because
     * dlopening the renderer into a process with nothing to place it is the one
     * state worse than not having an overlay at all.
     *
     * The rule itself is not written here any more (#33): this half used to
     * spell it out, the PE half spelled out a different one, and a reader had
     * to compare them to find that out. One predicate, from the manifest. */
    if (!shim_overlay_enabled()) return;
    std::string path = renderer_path();
    g_overlay = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    ulog("overlay: dlopen(%s) -> %p pid=%d%s%s", path.c_str(), g_overlay, (int)getpid(),
         g_overlay ? "" : " err=", g_overlay ? "" : dlerror());
    if (g_overlay) {
        n_IsOverlayEnabled       = (bool(*)(void))dlsym(g_overlay, "IsOverlayEnabled");
        n_BOverlayNeedsPresent   = (bool(*)(void))dlsym(g_overlay, "BOverlayNeedsPresent");
        n_SetNotificationPosition= (void(*)(uint32_t))dlsym(g_overlay, "SetNotificationPosition");
        ulog("overlay: predicates IsOverlayEnabled=%p BOverlayNeedsPresent=%p "
             "SetNotificationPosition=%p (#23)", (void*)n_IsOverlayEnabled,
             (void*)n_BOverlayNeedsPresent, (void*)n_SetNotificationPosition);
    }
    ulog("overlay: set STEAM_OVERLAY_LOGGING=1 and read /tmp/gameoverlayrenderer.%d.log — "
         "'Hooking ...' means we were early enough, its absence means we were not", (int)getpid());
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

/* ---- callback pump instrumentation (#90) --------------------------------
 *
 * Steam_BGetCallback and Steam_GetAPICallResult are pure passthroughs, and
 * until now silent ones: a pump delivering nothing and a pump nobody polls
 * looked identical in shim-unix.log, so "the title waits forever on an async
 * result" had no evidence either way behind it. AoE IV sat on its loading
 * screen with one outstanding RequestEncryptedAppTicket and the log could not
 * say whether a single callback had ever arrived.
 *
 * Both are called every frame, so neither logs per call. What goes in the log
 * is the shape of the answer: that the pump is being polled at all (once), and
 * each DISTINCT k_iCallback that has ever been delivered (capped). Anything
 * finer buries the file it is meant to be found in.
 *
 * Only m_iCallback is read out of CallbackMsg_t — it sits at offset 4 of both
 * the 32- and 64-bit layouts, so this needs no bitness case. The trailing
 * m_cubParam does not, and is not worth a layout assumption for a log line. */
static void pump_note_poll(void)
{
    static bool first = true;
    if (!first) return;
    first = false;
    ulog("callback pump: first Steam_BGetCallback poll (#90)");
}

static void pump_note_delivery(int callback)
{
    enum { CAP = 64 };
    static int seen[CAP];
    static int nseen;
    static bool capped;

    if (capped) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == callback) return;
    seen[nseen++] = callback;
    ulog("callback pump: delivered k_iCallback=%d (first of its id)", callback);
    if (nseen == CAP) {
        capped = true;
        ulog("callback pump: %d distinct callback ids seen — further ids unlogged", CAP);
    }
}

/* SteamAPICallCompleted_t (703) is the one callback worth logging EVERY time.
 *
 * It is not a broadcast — it names one SteamAPICall_t, and it is how a title
 * learns that the async request it is blocked on has an answer waiting. The
 * once-per-id rule above is exactly wrong for it: the second 703 is about a
 * different call than the first, and "did a 703 ever name MY handle" is the
 * question a hang like AoE IV's turns on. Bounded by how many async calls the
 * title makes, and capped anyway.
 *
 * Layout (identical on both sides, #3 §7.3):
 *   SteamAPICall_t m_hAsyncCall @0 (8) | int m_iCallback @8 (4) | uint32 m_cubParam @12 (4) */
static void pump_note_call_completed(int callback, uint64_t msg)
{
    static int logged;
    const char *param;
    uint64_t inner_ptr;
    uint64_t call;
    int inner_cb;

    if (callback != 703 || logged >= 64) return;
    /* m_pubParam @8 of CallbackMsg_t on x86_64; the payload is native memory. */
    memcpy(&inner_ptr, (const char *)(uintptr_t)msg + 8, sizeof inner_ptr);
    if (!inner_ptr) return;
    param = (const char *)(uintptr_t)inner_ptr;
    memcpy(&call, param, sizeof call);
    memcpy(&inner_cb, param + 8, sizeof inner_cb);
    ulog("callback pump: SteamAPICallCompleted_t call=%llu for callback=%d",
         (unsigned long long)call, inner_cb);
    if (++logged == 64) ulog("callback pump: 64 call-completions logged — further ones unlogged");
}

static NTSTATUS u_bgetcallback(void *args)
{
    auto *p = (sp_bgetcallback *)args;
    p->ret = 0;
    if (!n_BGetCallback) return 0;
    pump_note_poll();
    p->ret = n_BGetCallback(p->pipe, (void *)p->msg) ? 1 : 0;
    if (p->ret && p->msg) {
        int callback;
        memcpy(&callback, (const char *)(uintptr_t)p->msg + 4, sizeof callback);
        pump_note_delivery(callback);
        pump_note_call_completed(callback, p->msg);
    }
    return 0;
}
static NTSTATUS u_freelast(void *args)
{
    auto *p = (sp_freelast *)args;
    if (n_FreeLastCallback) n_FreeLastCallback(p->pipe);
    return 0;
}
/* The other half of the pump (#90). A SteamAPICall_t is answered at most once,
 * so logging every completion is bounded by how many the title issues — but a
 * title that polls a call that never completes would still flood, so only the
 * completions are logged, never the misses, and the count is capped. */
static NTSTATUS u_apicallresult(void *args)
{
    auto *p = (sp_apicallresult *)args;
    static int logged;
    p->ret = 0;
    if (!n_GetAPICallResult) return 0;
    p->ret = n_GetAPICallResult(p->pipe, p->call, (void *)p->cb, p->cub, p->expected, (bool *)p->failed) ? 1 : 0;
    if (p->ret && logged < 64) {
        const bool *failed = (const bool *)p->failed;
        ulog("callback pump: GetAPICallResult(call=%llu, expected=%d) -> complete, failed=%d",
             (unsigned long long)p->call, p->expected, failed ? (*failed ? 1 : 0) : -1);
        if (++logged == 64) ulog("callback pump: 64 completions logged — further ones unlogged");
    }
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

/* Defined with the ISteamFriends overlay block below, which is where the
 * slot-transfer reasoning it depends on is written down. */
static void *vslot(uint64_t handle, int32_t slot);

/* ISteamClient::Set_SteamAPI_CCheckCallbackRegisteredInProcess (#90).
 *
 * The client asks this "is callback N registered in this process?" before it
 * bothers queueing one. The real answer lives in steam_api64.dll's PE-side
 * registry, which is on the far side of a seam that carries no upcall — so we
 * do what Proton does (unixlib.cpp:231-239, 286-295) and register a unix stub
 * that says yes to everything. Over-reporting is the safe direction: a callback
 * the title did not register is dropped by its own dispatcher, whereas one it
 * did register and we suppressed never arrives at all.
 *
 * The pointer we hand over is OURS and lives for the process, so nothing about
 * its lifetime crosses the seam. */
static uint32_t shim_check_callback_registered(int32_t callback)
{
    (void)callback;
    return 1;
}

static NTSTATUS u_client_setcheckcb(void *args)
{
    auto *p = (sp_client_checkcb *)args;
    auto fn = (void (*)(void *, uint32_t (*)(int32_t)))vslot(p->handle, p->slot);
    ulog("Set_SteamAPI_CCheckCallbackRegisteredInProcess slot=%d fn=%p -> unix stub "
         "returning 1 (#90)", p->slot, (void *)fn);
    if (!fn) return 0;                 /* PE side could not name the slot */
    fn((void *)(uintptr_t)p->handle, shim_check_callback_registered);
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

/* ---- ISteamInput (VERSION006) — Mars calls Init() at boot; the stub's false
 * makes it log "SteamInput failed to initialize!" and then null-write on its
 * render thread. RunFrame/BNewDataAvailable/GetConnectedControllers are wired
 * too because a game that got a true from Init() will call them next. --------- */
#define INPUT(h) ((ISteamInput006 *)(h))
static NTSTATUS u_input_init(void *args)
{ auto *p = (sp_input_init *)args;
  p->ret = INPUT(p->handle)->Init(p->explicit_runframe != 0) ? 1 : 0;
  ulog("SteamInput Init(%d) -> %d", p->explicit_runframe, p->ret); return 0; }
static NTSTATUS u_input_shutdown(void *args)
{ auto *p = (sp_input_bool *)args; p->ret = INPUT(p->handle)->Shutdown() ? 1 : 0; return 0; }
static NTSTATUS u_input_runframe(void *args)
{ auto *p = (sp_input_runframe *)args; INPUT(p->handle)->RunFrame(p->reserved != 0); return 0; }
static NTSTATUS u_input_newdata(void *args)
{ auto *p = (sp_input_bool *)args; p->ret = INPUT(p->handle)->BNewDataAvailable() ? 1 : 0; return 0; }
static NTSTATUS u_input_connected(void *args)
{ auto *p = (sp_input_handles *)args;
  p->ret = INPUT(p->handle)->GetConnectedControllers((uint64_t *)p->out); return 0; }

/* ---- copy-out of native memory (#20) ------------------------------------
 * The 32-bit PE cannot dereference the dylib's heap (macOS puts it above 4 GB),
 * so these two run on THIS side of the seam, where the address is a real
 * pointer, and copy the bytes down into PE memory the caller already owns.
 * dst is always a PE address, which zero-extends into the uint64 field. */
static NTSTATUS u_copymem(void *args)
{
    auto *p = (sp_copymem *)args;
    if (p->src && p->dst && p->len > 0)
        memcpy((void *)(uintptr_t)p->dst, (const void *)(uintptr_t)p->src, (size_t)p->len);
    return 0;
}
static NTSTATUS u_copystr(void *args)
{
    auto *p = (sp_copystr *)args;
    p->ret = 0;
    if (!p->src || !p->dst || p->cap <= 0) return 0;
    const char *src = (const char *)(uintptr_t)p->src;
    char *dst = (char *)(uintptr_t)p->dst;
    size_t n = strlen(src);
    p->ret = (int32_t)n;
    if (n > (size_t)p->cap - 1) n = (size_t)p->cap - 1;   /* truncate, always NUL */
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

/* ---- ISteamUser encrypted app ticket (#20) --------------------------------
 * EOS's "Auth with Steam" path. RequestEncryptedAppTicket is ASYNC: it returns
 * a SteamAPICall_t and the answer arrives later as EncryptedAppTicketResponse_t
 * (1466) through the existing callback pump, after which the game calls
 * GetEncryptedAppTicket to collect the bytes. Both buffers are PE addresses the
 * game supplies, so the native side writes through them with no copy-down. */
static NTSTATUS u_user_reqencticket(void *args)
{
    auto *p = (sp_user_reqticket *)args;
    p->ret = ((ISteamUser *)p->handle)->RequestEncryptedAppTicket(
                 (void *)(uintptr_t)p->data, p->cb);
    ulog("RequestEncryptedAppTicket(cb=%d) -> call=%llu", p->cb,
         (unsigned long long)p->ret);
    return 0;
}
static NTSTATUS u_user_getencticket(void *args)
{
    auto *p = (sp_user_getticket *)args;
    p->ret = ((ISteamUser *)p->handle)->GetEncryptedAppTicket(
                 (void *)(uintptr_t)p->ticket, p->max,
                 (uint32_t *)(uintptr_t)p->cbticket) ? 1 : 0;
    ulog("GetEncryptedAppTicket(max=%d) -> %d (%u bytes)", p->max, p->ret,
         p->cbticket ? *(uint32_t *)(uintptr_t)p->cbticket : 0u);
    return 0;
}

/* ---- ISteamFriends (#29) ---- */
#define FRIENDS(h) ((ISteamFriends017 *)(h))
static NTSTATUS u_friends_personaname(void *args)
{ auto *p = (sp_friends_str *)args; p->ret = (uint64_t)FRIENDS(p->handle)->GetPersonaName();
  ulog("GetPersonaName() -> %s", p->ret ? (const char*)p->ret : "(null)"); return 0; }

/* ---- ISteamFriends overlay activation (#23) ------------------------------
 *
 * Dispatched by SLOT, not through a declared C++ class, and that is the whole
 * point rather than a shortcut. ISteamFriends put ActivateGameOverlay at slot
 * 19 in v003, 20 in v006, 21 in v010, 22 in v014, 28 in v015/v017 and 27 in
 * v018 — so a single `((ISteamFriends017 *)h)->ActivateGameOverlay(...)` would
 * silently call GetClanTag, or SetPlayedWith, on thirteen of the fifteen
 * versions that have it. The PE half resolves the method against the version
 * the title actually asked for and sends that slot; here we index the native
 * vtable with it. Safe because ISteamFriends has no same-name overload in ANY
 * version, so MSVC's order (what the PE side holds) and the dylib's Itanium
 * order are the same order.
 *
 * Deliberately forwarded whether or not the overlay is armed: unarmed, Valve's
 * client brings the native macOS Steam window to the front on the right page,
 * which is a degraded answer but a real one — and strictly better than the
 * dead button a stub gives.
 *
 * Every string here is a PE address the title owns and keeps alive for the
 * call, so it is read in place; nothing comes back by pointer, so none of the
 * #20 copy-down machinery is involved on either bitness. */
static void *vslot(uint64_t handle, int32_t slot)
{
    if (!handle || slot < 0) return nullptr;
    void **vt = *(void ***)(uintptr_t)handle;
    return vt ? vt[slot] : nullptr;
}
/* A miss is a real outcome, not an assertion: it means the PE side could not
 * name the slot, and the honest response is to do nothing loudly. */
#define OV_FN(T, p) ((T)vslot((p)->handle, (p)->slot))

static NTSTATUS u_fr_ov_activate(void *args)
{
    auto *p = (sp_fr_ov_str *)args;
    auto fn = OV_FN(void (*)(void *, const char *), p);
    ulog("ActivateGameOverlay(\"%s\") slot=%d fn=%p", p->str ? (const char *)(uintptr_t)p->str : "",
         p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, (const char *)(uintptr_t)p->str);
    return 0;
}
static NTSTATUS u_fr_ov_touser(void *args)
{
    auto *p = (sp_fr_ov_user *)args;
    auto fn = OV_FN(void (*)(void *, const char *, CSteamID_t), p);
    ulog("ActivateGameOverlayToUser(\"%s\", %llu) slot=%d fn=%p",
         p->str ? (const char *)(uintptr_t)p->str : "", (unsigned long long)p->steamid,
         p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, (const char *)(uintptr_t)p->str, p->steamid);
    return 0;
}
/* v005-v015 take the URL alone; v017/v018 added EActivateGameOverlayToWebPageMode.
 * One handler serves both: the PE half wires a mode-less thunk to the older
 * versions, which leaves mode 0 (k_EActivateGameOverlayToWebPageMode_Default),
 * and the extra SysV argument register is simply ignored by the older native
 * method. */
static NTSTATUS u_fr_ov_towebpage(void *args)
{
    auto *p = (sp_fr_ov_web *)args;
    auto fn = OV_FN(void (*)(void *, const char *, int32_t), p);
    ulog("ActivateGameOverlayToWebPage(\"%s\", mode=%d) slot=%d fn=%p",
         p->url ? (const char *)(uintptr_t)p->url : "", p->mode, p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, (const char *)(uintptr_t)p->url, p->mode);
    return 0;
}
/* Same two-shape story: v005-v012 take the AppId alone, v013+ added
 * EOverlayToStoreFlag. */
static NTSTATUS u_fr_ov_tostore(void *args)
{
    auto *p = (sp_fr_ov_store *)args;
    auto fn = OV_FN(void (*)(void *, AppId_t, int32_t), p);
    ulog("ActivateGameOverlayToStore(%u, flag=%d) slot=%d fn=%p",
         p->appid, p->flag, p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, p->appid, p->flag);
    return 0;
}
static NTSTATUS u_fr_ov_invite(void *args)
{
    auto *p = (sp_fr_ov_id *)args;
    auto fn = OV_FN(void (*)(void *, CSteamID_t), p);
    ulog("ActivateGameOverlayInviteDialog(%llu) slot=%d fn=%p",
         (unsigned long long)p->steamid, p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, p->steamid);
    return 0;
}
static NTSTATUS u_fr_ov_remoteplay(void *args)
{
    auto *p = (sp_fr_ov_id *)args;
    auto fn = OV_FN(void (*)(void *, CSteamID_t), p);
    ulog("ActivateGameOverlayRemotePlayTogetherInviteDialog(%llu) slot=%d fn=%p",
         (unsigned long long)p->steamid, p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, p->steamid);
    return 0;
}
static NTSTATUS u_fr_ov_connectstring(void *args)
{
    auto *p = (sp_fr_ov_str *)args;
    auto fn = OV_FN(void (*)(void *, const char *), p);
    ulog("ActivateGameOverlayInviteDialogConnectString(\"%s\") slot=%d fn=%p",
         p->str ? (const char *)(uintptr_t)p->str : "", p->slot, (void *)fn);
    if (fn) fn((void *)(uintptr_t)p->handle, (const char *)(uintptr_t)p->str);
    return 0;
}
/* #23 asked whether this one is in scope or an explicit no. It is in scope:
 * it is the same shape as its neighbours, it goes to the same native object,
 * and the alternative — a stub returning 0 — is a title being told its custom
 * protocol was rejected when nobody ever asked. Forwarding lets the client
 * give whatever answer is true. */
static NTSTATUS u_fr_ov_registerprotocol(void *args)
{
    auto *p = (sp_fr_ov_proto *)args;
    auto fn = OV_FN(bool (*)(void *, const char *), p);
    p->ret = fn ? (fn((void *)(uintptr_t)p->handle, (const char *)(uintptr_t)p->str) ? 1 : 0) : 0;
    ulog("RegisterProtocolInOverlayBrowser(\"%s\") slot=%d -> %d",
         p->str ? (const char *)(uintptr_t)p->str : "", p->slot, p->ret);
    return 0;
}

/* ---- overlay predicates (#23) --------------------------------------------
 *
 * These are the load-bearing half of the ticket. IsOverlayEnabled is the answer
 * a title uses to decide whether to PAUSE and wait for a panel, so `true` with
 * no compositor is a hang and `false` with one running is a dead button — and
 * neither is a constant we could hardcode once #25 made the overlay real.
 *
 * So we do not track it. Valve's renderer exports it, and its answer is the
 * truth by construction: static analysis of gameoverlayrenderer.dylib shows the
 * byte IsOverlayEnabled returns starts at 0 and is set to 1 only inside the
 * client handshake loop, after the overlay has actually armed. Loading the
 * renderer is not enough to make it say yes. With SHIM_OVERLAY off we never
 * dlopen it at all, there is no symbol, and the answer is false — which is the
 * well-tested no-overlay branch every title already has. */
static NTSTATUS u_overlay_isenabled(void *args)
{
    auto *p = (sp_overlay_bool *)args;
    p->ret = n_IsOverlayEnabled ? (n_IsOverlayEnabled() ? 1 : 0) : 0;
    ulog("IsOverlayEnabled() -> %d (renderer %s)", p->ret,
         n_IsOverlayEnabled ? "loaded" : "not loaded — " SHIM_ENV_OVERLAY " off or dlopen failed");
    return 0;
}
static NTSTATUS u_overlay_needspresent(void *args)
{
    auto *p = (sp_overlay_bool *)args;
    p->ret = n_BOverlayNeedsPresent ? (n_BOverlayNeedsPresent() ? 1 : 0) : 0;
    return 0;
}
static NTSTATUS u_overlay_setnotifypos(void *args)
{
    auto *p = (sp_overlay_pos *)args;
    if (n_SetNotificationPosition) n_SetNotificationPosition((uint32_t)p->pos);
    ulog("SetOverlayNotificationPosition(%d) -> %s", p->pos,
         n_SetNotificationPosition ? "forwarded" : "no renderer, ignored");
    return 0;
}
/* The renderer exports no inset setter, so this is accept-and-ignore by
 * necessity rather than by choice. It returns void and is pure cosmetics — a
 * notification a few pixels off is not a failure a title can observe, let alone
 * one it stops for. Logged so a misplaced toast has a first thing to check. */
static NTSTATUS u_overlay_setnotifyinset(void *args)
{
    auto *p = (sp_overlay_inset *)args;
    ulog("SetOverlayNotificationInset(%d, %d) -> accepted and ignored "
         "(the renderer exports no inset setter)", p->x, p->y);
    return 0;
}

/* ---- ISteamRemoteStorage (slots 0-23) — the Steam Cloud file surface (#43).
 *
 * A title with cloud saves does not open userdata/<id>/<appid>/remote itself; it
 * asks this interface. Stubbed, FileExists() is false and a complete save is
 * invisible — Space Marine offers only "New Campaign" (#43). The handle casts to
 * one class for every version because ISteamRemoteStorage has no same-name
 * overload anywhere, so Itanium order == MSVC order (see steam_ifaces.h). -------- */
#define RS(h) ((ISteamRemoteStorage016 *)(h))
static NTSTATUS u_rs_filewrite(void *args)
{ auto *p = (sp_rs_filedata *)args;
  p->ret = RS(p->handle)->FileWrite((const char *)p->name, (const void *)p->data, p->count) ? 1 : 0;
  ulog("FileWrite(\"%s\", %d bytes) -> %d", (const char *)p->name, p->count, p->ret); return 0; }
static NTSTATUS u_rs_fileread(void *args)
{ auto *p = (sp_rs_filedata *)args;
  p->ret = RS(p->handle)->FileRead((const char *)p->name, (void *)p->data, p->count);
  ulog("FileRead(\"%s\", %d) -> %d bytes", (const char *)p->name, p->count, p->ret); return 0; }
static NTSTATUS u_rs_filewriteasync(void *args)
{ auto *p = (sp_rs_writeasync *)args;
  p->ret = RS(p->handle)->FileWriteAsync((const char *)p->name, (const void *)p->data, (uint32_t)p->count);
  ulog("FileWriteAsync(\"%s\", %d bytes) -> call 0x%llx", (const char *)p->name, p->count,
       (unsigned long long)p->ret); return 0; }
static NTSTATUS u_rs_filereadasync(void *args)
{ auto *p = (sp_rs_readasync *)args;
  p->ret = RS(p->handle)->FileReadAsync((const char *)p->name, (uint32_t)p->offset, (uint32_t)p->toread);
  ulog("FileReadAsync(\"%s\", off=%d, %d) -> call 0x%llx", (const char *)p->name, p->offset,
       p->toread, (unsigned long long)p->ret); return 0; }
static NTSTATUS u_rs_filereadasyncdone(void *args)
{ auto *p = (sp_rs_readasyncdone *)args;
  p->ret = RS(p->handle)->FileReadAsyncComplete(p->call, (void *)p->data, (uint32_t)p->toread) ? 1 : 0;
  ulog("FileReadAsyncComplete(0x%llx, %d) -> %d", (unsigned long long)p->call, p->toread, p->ret); return 0; }
static NTSTATUS u_rs_fileforget(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->FileForget((const char *)p->name) ? 1 : 0; return 0; }
static NTSTATUS u_rs_filedelete(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->FileDelete((const char *)p->name) ? 1 : 0;
  ulog("FileDelete(\"%s\") -> %d", (const char *)p->name, p->ret); return 0; }
static NTSTATUS u_rs_fileshare(void *args)
{ auto *p = (sp_rs_name_u64 *)args; p->ret = RS(p->handle)->FileShare((const char *)p->name); return 0; }
static NTSTATUS u_rs_setsyncplatforms(void *args)
{ auto *p = (sp_rs_syncplat *)args;
  p->ret = RS(p->handle)->SetSyncPlatforms((const char *)p->name, p->platform) ? 1 : 0; return 0; }
static NTSTATUS u_rs_streamopen(void *args)
{ auto *p = (sp_rs_name_u64 *)args; p->ret = RS(p->handle)->FileWriteStreamOpen((const char *)p->name);
  ulog("FileWriteStreamOpen(\"%s\") -> 0x%llx", (const char *)p->name, (unsigned long long)p->ret); return 0; }
static NTSTATUS u_rs_streamchunk(void *args)
{ auto *p = (sp_rs_streamchunk *)args;
  p->ret = RS(p->handle)->FileWriteStreamWriteChunk(p->stream, (const void *)p->data, p->count) ? 1 : 0; return 0; }
static NTSTATUS u_rs_streamclose(void *args)
{ auto *p = (sp_rs_stream *)args; p->ret = RS(p->handle)->FileWriteStreamClose(p->stream) ? 1 : 0;
  ulog("FileWriteStreamClose(0x%llx) -> %d", (unsigned long long)p->stream, p->ret); return 0; }
static NTSTATUS u_rs_streamcancel(void *args)
{ auto *p = (sp_rs_stream *)args; p->ret = RS(p->handle)->FileWriteStreamCancel(p->stream) ? 1 : 0; return 0; }
static NTSTATUS u_rs_fileexists(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->FileExists((const char *)p->name) ? 1 : 0;
  ulog("FileExists(\"%s\") -> %d", (const char *)p->name, p->ret); return 0; }
static NTSTATUS u_rs_filepersisted(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->FilePersisted((const char *)p->name) ? 1 : 0; return 0; }
static NTSTATUS u_rs_getfilesize(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->GetFileSize((const char *)p->name);
  ulog("GetFileSize(\"%s\") -> %d", (const char *)p->name, p->ret); return 0; }
static NTSTATUS u_rs_getfiletimestamp(void *args)
{ auto *p = (sp_rs_name_i64 *)args; p->ret = RS(p->handle)->GetFileTimestamp((const char *)p->name); return 0; }
static NTSTATUS u_rs_getsyncplatforms(void *args)
{ auto *p = (sp_rs_name_i32 *)args; p->ret = RS(p->handle)->GetSyncPlatforms((const char *)p->name); return 0; }
static NTSTATUS u_rs_getfilecount(void *args)
{ auto *p = (sp_rs_noarg *)args; p->ret = RS(p->handle)->GetFileCount();
  ulog("GetFileCount() -> %d", p->ret); return 0; }
static NTSTATUS u_rs_getfilenameandsize(void *args)
{ auto *p = (sp_rs_namesize *)args;
  p->ret = (uint64_t)RS(p->handle)->GetFileNameAndSize(p->index, (int32_t *)p->size_out);
  ulog("GetFileNameAndSize(%d) -> %s", p->index, p->ret ? (const char *)p->ret : "(null)"); return 0; }
static NTSTATUS u_rs_getquota(void *args)
{ auto *p = (sp_rs_quota *)args;
  p->ret = RS(p->handle)->GetQuota((uint64_t *)p->total, (uint64_t *)p->avail) ? 1 : 0; return 0; }
static NTSTATUS u_rs_cloudforaccount(void *args)
{ auto *p = (sp_rs_noarg *)args; p->ret = RS(p->handle)->IsCloudEnabledForAccount() ? 1 : 0;
  ulog("IsCloudEnabledForAccount() -> %d", p->ret); return 0; }
static NTSTATUS u_rs_cloudforapp(void *args)
{ auto *p = (sp_rs_noarg *)args; p->ret = RS(p->handle)->IsCloudEnabledForApp() ? 1 : 0;
  ulog("IsCloudEnabledForApp() -> %d", p->ret); return 0; }
static NTSTATUS u_rs_setcloudforapp(void *args)
{ auto *p = (sp_rs_setcloud *)args; RS(p->handle)->SetCloudEnabledForApp(p->enabled != 0);
  ulog("SetCloudEnabledForApp(%d)", p->enabled); return 0; }

/* ---- generated handlers (#78) -------------------------------------------
 *
 * One per generated opcode. Each indexes the native vtable with the slot the PE
 * half resolved — the ISteamFriends pattern from #23 — instead of casting the
 * handle to a declared C++ class the way every hand-written block above does.
 *
 * That is not a shortcut around steam_ifaces.h; it is what makes 39 interfaces
 * across 212 versions possible at all. A class cast needs every slot up to the
 * deepest method transcribed by hand so the compiler emits the right index, and
 * the transcription is where the risk lives. The slot the PE side already holds
 * is that index, for free and checked at build time.
 *
 * Safe because MSVC's ONLY reordering against the declaration order the dylib
 * was compiled in is to reverse each contiguous run of same-name overloads, and
 * gen_thunks.py refuses every method that sits in such a run (gen/REPORT.md
 * names them). Outside those runs, the MSVC slot IS the Itanium slot.
 *
 * A slot the PE side could not name arrives as -1 and is dropped loudly. There
 * is no safe guess: the wrong index on a 60-method interface is some entirely
 * different method. */
#include "gen/shim_gen_unix.h"

/* ---- diagnostics (#45) — the PE half's one must-not-be-missed line ---------
 *
 * shim_pe.c's dbg() goes to OutputDebugStringA and, only when SHIM_PE_LOG is
 * set, to a file: in a normal run, nowhere. An unmapped vtable slot needs to be
 * louder than that, because its failure mode is a silent 0 that a title builds
 * on (#43). This lands it in shim-unix.log beside everything else. */
static NTSTATUS u_log(void *args)
{ auto *p = (sp_log *)args; ulog("%s", (const char *)p->msg); return 0; }

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
    u_input_init, u_input_shutdown, u_input_runframe, u_input_newdata, u_input_connected,
    u_copymem, u_copystr,
    u_user_reqencticket, u_user_getencticket,
    u_friends_personaname,
    u_fr_ov_activate, u_fr_ov_touser, u_fr_ov_towebpage, u_fr_ov_tostore,
    u_fr_ov_invite, u_fr_ov_remoteplay, u_fr_ov_connectstring, u_fr_ov_registerprotocol,
    u_overlay_isenabled, u_overlay_needspresent,
    u_overlay_setnotifypos, u_overlay_setnotifyinset,
    u_rs_filewrite, u_rs_fileread, u_rs_filewriteasync, u_rs_filereadasync,
    u_rs_filereadasyncdone, u_rs_fileforget, u_rs_filedelete, u_rs_fileshare,
    u_rs_setsyncplatforms, u_rs_streamopen, u_rs_streamchunk, u_rs_streamclose,
    u_rs_streamcancel, u_rs_fileexists, u_rs_filepersisted, u_rs_getfilesize,
    u_rs_getfiletimestamp, u_rs_getsyncplatforms, u_rs_getfilecount,
    u_rs_getfilenameandsize, u_rs_getquota, u_rs_cloudforaccount,
    u_rs_cloudforapp, u_rs_setcloudforapp,
    u_log,
    u_client_setcheckcb,
#include "gen/shim_gen_dispatch.h"
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
    u_input_init, u_input_shutdown, u_input_runframe, u_input_newdata, u_input_connected,
    u_copymem, u_copystr,
    u_user_reqencticket, u_user_getencticket,
    u_friends_personaname,
    u_fr_ov_activate, u_fr_ov_touser, u_fr_ov_towebpage, u_fr_ov_tostore,
    u_fr_ov_invite, u_fr_ov_remoteplay, u_fr_ov_connectstring, u_fr_ov_registerprotocol,
    u_overlay_isenabled, u_overlay_needspresent,
    u_overlay_setnotifypos, u_overlay_setnotifyinset,
    u_rs_filewrite, u_rs_fileread, u_rs_filewriteasync, u_rs_filereadasync,
    u_rs_filereadasyncdone, u_rs_fileforget, u_rs_filedelete, u_rs_fileshare,
    u_rs_setsyncplatforms, u_rs_streamopen, u_rs_streamchunk, u_rs_streamclose,
    u_rs_streamcancel, u_rs_fileexists, u_rs_filepersisted, u_rs_getfilesize,
    u_rs_getfiletimestamp, u_rs_getsyncplatforms, u_rs_getfilecount,
    u_rs_getfilenameandsize, u_rs_getquota, u_rs_cloudforaccount,
    u_rs_cloudforapp, u_rs_setcloudforapp,
    u_log,
    u_client_setcheckcb,
#include "gen/shim_gen_dispatch.h"
};

/* A short array silently maps every opcode past the end onto garbage, and a
 * long one hides a missing handler — both look like a hang, not an error (#11). */
static_assert(sizeof(__wine_unix_call_funcs) / sizeof(*__wine_unix_call_funcs) == C_COUNT,
              "__wine_unix_call_funcs is out of step with enum shim_call");
static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(*__wine_unix_call_wow64_funcs) == C_COUNT,
              "__wine_unix_call_wow64_funcs is out of step with enum shim_call");
} /* extern "C" */
