// Connection diagnostic for macos-steam issue #2.
//
// The main probe gets a pipe, a user, and a plausible SteamID out of
// steamclient.dylib — but BLoggedOn is false and the persona name comes back as
// filler bytes. That pattern is what you would see if CreateInterface handed us
// a *local, disconnected* client engine rather than a live link to the running
// Steam.app: anything answerable from local config succeeds, anything needing
// IPC returns nothing.
//
// steamclient.dylib exports the legacy flat C API, which answers the question
// without any vtable guesswork: Steam_BConnected / Steam_BLoggedOn.

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "steam_min.h"

typedef HSteamPipe (*Fn_CreateSteamPipe)();
typedef HSteamUser (*Fn_ConnectToGlobalUser)(HSteamPipe);
typedef bool (*Fn_BConnected)(HSteamUser, HSteamPipe);
typedef bool (*Fn_BLoggedOn)(HSteamUser, HSteamPipe);
typedef bool (*Fn_BGetCallback)(HSteamPipe, void*, int*);

static const char* g_stage = "startup";
static void on_sig(int) {
  printf("\n!! CRASH during: %s\n", g_stage);
  fflush(stdout);
  _exit(70);
}

int main() {
  signal(SIGSEGV, on_sig);
  signal(SIGBUS, on_sig);

  std::string path = std::string(getenv("HOME")) +
                     "/Library/Application Support/Steam/Steam.AppBundle/Steam/"
                     "Contents/MacOS/steamclient.dylib";
  void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("dlopen failed: %s\n", dlerror());
    return 1;
  }

  auto need = [&](const char* n) {
    void* p = dlsym(h, n);
    printf("  %-28s = %p\n", n, p);
    return p;
  };

  printf("=== flat-API connection diagnostic ===\n");
  auto fCreatePipe = (Fn_CreateSteamPipe)need("Steam_CreateSteamPipe");
  auto fConnect = (Fn_ConnectToGlobalUser)need("Steam_ConnectToGlobalUser");
  auto fBConnected = (Fn_BConnected)need("Steam_BConnected");
  auto fBLoggedOn = (Fn_BLoggedOn)need("Steam_BLoggedOn");
  auto fBGetCallback = (Fn_BGetCallback)need("Steam_BGetCallback");
  if (!fCreatePipe || !fConnect || !fBConnected) return 1;

  g_stage = "Steam_CreateSteamPipe";
  HSteamPipe pipe = fCreatePipe();
  printf("\n  pipe = %d\n", pipe);
  if (!pipe) return 2;

  g_stage = "Steam_ConnectToGlobalUser";
  HSteamUser user = fConnect(pipe);
  printf("  user = %d\n", user);
  if (!user) return 3;

  g_stage = "Steam_BConnected";
  printf("\n  *** Steam_BConnected = %s ***\n",
         fBConnected(user, pipe) ? "TRUE (live IPC to running Steam)"
                                 : "FALSE (no live connection)");
  g_stage = "Steam_BLoggedOn";
  if (fBLoggedOn) printf("  Steam_BLoggedOn  = %d\n", fBLoggedOn(user, pipe));

  // If IPC is live, the pipe should eventually surface callbacks. Pump briefly.
  if (fBGetCallback) {
    g_stage = "Steam_BGetCallback pump";
    printf("\n  pumping callbacks for ~2s...\n");
    int got = 0;
    for (int i = 0; i < 200; i++) {
      alignas(16) unsigned char msg[512] = {0};
      int callbackId = 0;
      if (fBGetCallback(pipe, msg, &callbackId)) {
        printf("    callback id=%d\n", callbackId);
        if (++got > 20) break;
      }
      usleep(10000);
    }
    printf("  callbacks received: %d\n", got);
  }
  return 0;
}
