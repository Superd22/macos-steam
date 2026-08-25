/* bridge_unix.cpp — the unix half of the seam spike, stage 2 (#10).
 *
 * Built as a thin x86_64 Mach-O dylib named bridgetest.so and discovered by
 * Wine's ntdll.so via WINEDLLPATH beside the builtin PE (see FINDINGS.md). It
 * exports the two unixlib symbol arrays; that contract — arrays of
 * NTSTATUS(*)(void*) indexed by the call code — is the entire seam.
 *
 * Stage 1 (#8) only XOR'd a magic to prove control crossed. Stage 2 puts the
 * REAL native work on this side: dlopen Valve's steamclient.dylib INSIDE the
 * Wine process, run the exact connection sequence proven standalone in #2
 * (CreateInterface -> CreateSteamPipe -> ConnectToGlobalUser), assert the live
 * link with Steam_BConnected, and read one unmistakable value (GetSteamID) plus
 * the persona name back across the seam. The open question #10 answers is
 * whether steamclient.dylib behaves the same in here as it did in a plain
 * process — same IPC, same run-loop assumptions, no symbol/CoreFoundation clash.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "bridge_abi.h"
#include "steam_min.h"

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);

/* The flat C connection oracle from steamclient.dylib — used exactly as #2's
 * connprobe did, because it answers "is the IPC live?" with no vtable guesswork.
 * Steam_BConnected is the ONLY signal that distinguishes a working bridge from
 * Steam-in-offline-mode, which otherwise still returns a correct cached SteamID
 * and persona (map trap #1). */
typedef bool (*Fn_BConnected)(HSteamUser, HSteamPipe);

/* Connection state, cached across calls so the RTT benchmarks reuse one live
 * link rather than reconnecting per iteration. */
static void          *g_dylib   = nullptr;
static ISteamClient  *g_client  = nullptr;
static ISteamFriends *g_friends = nullptr;
static HSteamPipe     g_pipe    = 0;
static HSteamUser     g_user    = 0;

static const char *kClientVersions[] = {
    "SteamClient023", "SteamClient022", "SteamClient021",
    "SteamClient020", "SteamClient019", "SteamClient018", "SteamClient017",
};
static const char *kUserVersions[] = {
    "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020",
};
static const char *kFriendsVersions[] = {
    "SteamFriends018", "SteamFriends017", "SteamFriends015",
};

static std::string steamclient_path()
{
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) +
           "/Library/Application Support/Steam/Steam.AppBundle/Steam/"
           "Contents/MacOS/steamclient.dylib";
}

/* ---- entry 0: seam RTT baseline, no Steam involvement -------------------- */
static NTSTATUS u_noop(void *args)
{
    struct noop_params *p = (struct noop_params *)args;
    p->magic_out = p->magic_in ^ BRIDGE_NOOP_XOR;
    return 0;
}

/* ---- entry 1: dlopen + connect + read once ------------------------------- */
static NTSTATUS u_init(void *args)
{
    struct steam_init_result *r = (struct steam_init_result *)args;
    memset(r, 0, sizeof(*r));
    r->unix_pid = (int)getpid();
    struct utsname un;
    if (!uname(&un))
        strncpy(r->unix_sysname, un.sysname, sizeof(r->unix_sysname) - 1);

    std::string path = steamclient_path();
    g_dylib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!g_dylib) {
        r->step = -1;
        snprintf(r->err, sizeof(r->err), "dlopen: %s", dlerror());
        return 0;
    }
    r->dlopen_ok = 1;

    CreateInterfaceFn CreateInterface =
        (CreateInterfaceFn)dlsym(g_dylib, "CreateInterface");
    Fn_BConnected Steam_BConnected =
        (Fn_BConnected)dlsym(g_dylib, "Steam_BConnected");
    if (!CreateInterface) {
        r->step = -2;
        snprintf(r->err, sizeof(r->err), "dlsym CreateInterface: %s", dlerror());
        return 0;
    }
    r->create_interface_ok = 1;

    for (const char *v : kClientVersions) {
        int rc = 0;
        void *p = CreateInterface(v, &rc);
        if (p) {
            g_client = (ISteamClient *)p;
            strncpy(r->client_version, v, sizeof(r->client_version) - 1);
            break;
        }
    }
    if (!g_client) {
        r->step = -3;
        snprintf(r->err, sizeof(r->err), "no ISteamClient version served");
        return 0;
    }

    g_pipe = g_client->CreateSteamPipe();
    r->pipe = g_pipe;
    if (!g_pipe) {
        r->step = -4;
        snprintf(r->err, sizeof(r->err), "CreateSteamPipe returned 0");
        return 0;
    }

    g_user = g_client->ConnectToGlobalUser(g_pipe);
    r->user = g_user;
    if (!g_user) {
        r->step = -5;
        snprintf(r->err, sizeof(r->err), "ConnectToGlobalUser returned 0");
        return 0;
    }

    /* The honest online oracle. Correct SteamID/persona below survive OFFLINE
     * from cache, so BConnected is what actually proves the live IPC works from
     * inside Wine — the whole point of stage 2. */
    if (Steam_BConnected)
        r->b_connected = Steam_BConnected(g_user, g_pipe) ? 1 : 0;
    else
        r->b_connected = -1; /* export missing; should not happen on this dylib */

    ISteamUser *su = nullptr;
    for (const char *v : kUserVersions) {
        su = g_client->GetISteamUser(g_user, g_pipe, v);
        if (su) break;
    }
    if (su) {
        r->b_logged_on = su->BLoggedOn() ? 1 : 0;
        r->steam_id = (uint64_t)su->GetSteamID();
    }

    for (const char *v : kFriendsVersions) {
        g_friends = g_client->GetISteamFriends(g_user, g_pipe, v);
        if (g_friends) break;
    }
    if (g_friends) {
        const char *name = g_friends->GetPersonaName();
        if (name) {
            strncpy(r->persona_name, name, sizeof(r->persona_name) - 1);
            r->persona_ok = 1;
        }
    }

    r->step = 0;
    return 0;
}

/* ---- entry 2: re-read persona, timed by the PE side ---------------------- */
static NTSTATUS u_persona(void *args)
{
    struct persona_params *p = (struct persona_params *)args;
    p->ok = 0;
    p->persona_name[0] = 0;
    if (!g_friends) return 0;
    const char *name = g_friends->GetPersonaName();
    if (name) {
        strncpy(p->persona_name, name, sizeof(p->persona_name) - 1);
        p->ok = 1;
    }
    return 0;
}

/* ---- entry 3: teardown --------------------------------------------------- */
static NTSTATUS u_shutdown(void *args)
{
    (void)args;
    if (g_client && g_pipe) {
        if (g_user) g_client->ReleaseUser(g_pipe, g_user);
        g_client->BReleaseSteamPipe(g_pipe);
    }
    g_client = nullptr;
    g_friends = nullptr;
    g_pipe = 0;
    g_user = 0;
    return 0;
}

extern "C" {

NTSTATUS __wine_unix_lib_init(void) { return 0; }

/* Order MUST match enum bridge_call in bridge_abi.h. ntdll.so resolves these by
 * name via dlsym, so they must have external linkage and survive dead-stripping.
 * In C++ a file-scope `const` array is internal linkage by default (unlike C),
 * so `extern` + `used` are load-bearing here, not decoration. */
extern const unixlib_entry_t __wine_unix_call_funcs[]
    __attribute__((used, visibility("default")));
extern const unixlib_entry_t __wine_unix_call_wow64_funcs[]
    __attribute__((used, visibility("default")));

const unixlib_entry_t __wine_unix_call_funcs[] = {
    u_noop, u_init, u_persona, u_shutdown,
};
const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    u_noop, u_init, u_persona, u_shutdown,
};

} /* extern "C" */
