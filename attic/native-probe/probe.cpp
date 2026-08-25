// Go/no-go probe for macos-steam issue #2.
//
// Question: can a plain macOS process — one Steam did NOT launch — dlopen
// Valve's native steamclient.dylib, obtain a working ISteamClient, connect to
// the already-running Steam.app, and read back real account state?
//
// Staged on purpose: every stage announces itself and flushes before the risky
// call, so a crash still tells us exactly how far we got. A signal handler
// turns a segfault into a labelled report rather than a bare "Segmentation
// fault: 11".
//
// Usage: probe [appid]        (appid defaults to 480 / Spacewar)
// Read-only: never writes to Steam's data directories.

#include <dlfcn.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "steam_min.h"

// ---------------------------------------------------------------- reporting

static const char* g_stage = "startup";

static void stage(const char* name) {
  g_stage = name;
  printf("\n[stage] %s\n", name);
  fflush(stdout);
}

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("  ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
  fflush(stdout);
}

static void on_fatal_signal(int sig) {
  const char* n = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGILL";
  printf("\n!! CRASH (%s) during stage: %s\n", n, g_stage);
  fflush(stdout);
  _exit(70);
}

// ------------------------------------------------------------------ helpers

static std::string steamclient_path() {
  const char* home = getenv("HOME");
  if (!home) {
    struct passwd* pw = getpwuid(getuid());
    home = pw ? pw->pw_dir : "/tmp";
  }
  return std::string(home) +
         "/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/"
         "MacOS/steamclient.dylib";
}

static const char* arch_name() {
#if defined(__aarch64__) || defined(__arm64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

// Which ISteamClient versions to try, newest first.
static const char* kClientVersions[] = {
    "SteamClient023", "SteamClient022", "SteamClient021", "SteamClient020",
    "SteamClient019", "SteamClient018", "SteamClient017",
};

static const char* kUserVersions[] = {
    "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020",
};

static const char* kFriendsVersions[] = {
    "SteamFriends018", "SteamFriends017", "SteamFriends015",
};

static const char* kUserStatsVersions[] = {
    "STEAMUSERSTATS_INTERFACE_VERSION013",
    "STEAMUSERSTATS_INTERFACE_VERSION012",
    "STEAMUSERSTATS_INTERFACE_VERSION011",
};

// ---------------------------------------------------------------------- main

int main(int argc, char** argv) {
  signal(SIGSEGV, on_fatal_signal);
  signal(SIGBUS, on_fatal_signal);
  signal(SIGILL, on_fatal_signal);

  const char* appid = (argc > 1) ? argv[1] : "480";

  printf("=== macos-steam native probe ===\n");
  printf("  caller arch : %s\n", arch_name());
  printf("  pid         : %d\n", getpid());
  printf("  target appid: %s\n", appid);
  for (const char* v : {"SteamAppId", "SteamGameId", "SteamClientLaunch",
                        "SteamPath", "SteamOverlayGameId", "SteamEnv"}) {
    const char* got = getenv(v);
    printf("  env %-18s = %s\n", v, got ? got : "(unset)");
  }
  fflush(stdout);

  // -- 1. dlopen ------------------------------------------------------------
  stage("dlopen steamclient.dylib");
  std::string path = steamclient_path();
  say("path: %s", path.c_str());
  void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    say("FAIL dlopen: %s", dlerror());
    return 1;
  }
  say("OK handle=%p", h);

  // -- 2. dlsym CreateInterface --------------------------------------------
  stage("dlsym CreateInterface");
  CreateInterfaceFn CreateInterface =
      (CreateInterfaceFn)dlsym(h, "CreateInterface");
  if (!CreateInterface) {
    say("FAIL dlsym: %s", dlerror());
    return 1;
  }
  say("OK CreateInterface=%p", (void*)CreateInterface);

  // -- 3. CreateInterface(SteamClientNNN) -----------------------------------
  stage("CreateInterface -> ISteamClient");
  ISteamClient* client = nullptr;
  const char* clientVersion = nullptr;
  for (const char* v : kClientVersions) {
    int rc = 0;
    void* p = CreateInterface(v, &rc);
    say("%-16s -> %p (rc=%d)", v, p, rc);
    if (p && !client) {
      client = (ISteamClient*)p;
      clientVersion = v;
    }
  }
  if (!client) {
    say("FAIL: no ISteamClient version was served");
    return 2;
  }
  say("using %s at %p", clientVersion, (void*)client);
  say("vtable ptr = %p", *(void**)client);

  // -- 4. CreateSteamPipe ---------------------------------------------------
  stage("CreateSteamPipe");
  HSteamPipe pipe = client->CreateSteamPipe();
  say("pipe = %d", pipe);
  if (pipe == 0) {
    say("FAIL: no pipe. Steam refused the connection or is not running.");
    return 3;
  }

  // -- 5. ConnectToGlobalUser ----------------------------------------------
  stage("ConnectToGlobalUser");
  HSteamUser user = client->ConnectToGlobalUser(pipe);
  say("user = %d", user);
  if (user == 0) {
    say("FAIL: pipe opened but the global user was refused.");
    say("      -> Steam gates unlaunched processes at the user, not the pipe.");
    client->BReleaseSteamPipe(pipe);
    return 4;
  }

  // -- 6. ISteamUser --------------------------------------------------------
  stage("GetISteamUser + GetSteamID");
  ISteamUser* su = nullptr;
  for (const char* v : kUserVersions) {
    su = client->GetISteamUser(user, pipe, v);
    say("%-14s -> %p", v, (void*)su);
    if (su) break;
  }
  if (su) {
    say("BLoggedOn = %s", su->BLoggedOn() ? "true" : "false");
    CSteamID_t id = su->GetSteamID();
    say("SteamID   = %llu", (unsigned long long)id);
  } else {
    say("WARN: no ISteamUser served");
  }

  // -- 7. ISteamFriends -----------------------------------------------------
  stage("GetISteamFriends + GetPersonaName");
  ISteamFriends* sf = nullptr;
  for (const char* v : kFriendsVersions) {
    sf = client->GetISteamFriends(user, pipe, v);
    say("%-16s -> %p", v, (void*)sf);
    if (sf) break;
  }
  if (sf) {
    const char* name = sf->GetPersonaName();
    say("*** PERSONA NAME = %s ***", name ? name : "(null)");
  } else {
    say("WARN: no ISteamFriends served");
  }

  // -- 8. ISteamUserStats ---------------------------------------------------
  stage("GetISteamUserStats + achievement read");
  // v013 dropped RequestCurrentStats, so drive it through the v013 layout and
  // prove the slot indices by asking for an achievement that cannot exist: a
  // correct binding answers false and names the achievement in its warning.
  ISteamUserStats013* stats = (ISteamUserStats013*)client->GetISteamUserStats(
      user, pipe, "STEAMUSERSTATS_INTERFACE_VERSION013");
  say("v013 iface = %p", (void*)stats);
  if (stats) {
    bool achieved = false;
    bool ok = stats->GetAchievement("PROBE_SLOT_CHECK_NOT_REAL", &achieved);
    say("GetAchievement(PROBE_SLOT_CHECK_NOT_REAL) -> ok=%d achieved=%d", ok,
        achieved);
    const char* probeAch = getenv("PROBE_ACHIEVEMENT");
    if (probeAch) {
      achieved = false;
      uint32_t when = 0;
      ok = stats->GetAchievementAndUnlockTime(probeAch, &achieved, &when);
      say("GetAchievementAndUnlockTime(%s) -> ok=%d achieved=%d unlockTime=%u",
          probeAch, ok, achieved, when);
    }
  }

  // -- 9. teardown ----------------------------------------------------------
  stage("teardown");
  client->ReleaseUser(pipe, user);
  client->BReleaseSteamPipe(pipe);
  say("released");

  printf("\n=== probe completed without crashing ===\n");
  return 0;
}
