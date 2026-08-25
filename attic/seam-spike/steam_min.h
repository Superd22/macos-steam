// Minimal Steamworks vtable declarations for the seam spike's unix half.
//
// Copied down from instruments/native-probe/steam_min.h (proven against the live
// dylib in #2) and trimmed to exactly the interfaces stage 2 calls through:
// ISteamClient (to GetISteamFriends), ISteamUser, ISteamFriends. Only the
// leading vtable slots up to the method we call need to be declared; the tail
// is truncated. Every declared slot must match the SDK order or the call lands
// in the wrong function and corrupts (see #2's vtable trap).
//
// Deliberately NOT declared: ISteamUtils / GetAppID. Its vtable slot is
// version-specific and a wrong index segfaults with no clean error, and GetAppID
// needs a real app context to return 3215050 anyway (no game runs here). The
// unmistakable crossed value for this spike is GetSteamID, proven in #2.

#pragma once

#include <cstdint>

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint64_t CSteamID_t;  // CSteamID is a single uint64 by value

class ISteamGameServer;
class ISteamMatchmaking;
class ISteamMatchmakingServers;
class ISteamUtils;
class ISteamUserStats;

class ISteamUser {
 public:
  virtual HSteamUser GetHSteamUser() = 0;
  virtual bool BLoggedOn() = 0;
  virtual CSteamID_t GetSteamID() = 0;
  // ... truncated
};

class ISteamFriends {
 public:
  virtual const char* GetPersonaName() = 0;
  // ... truncated
};

class ISteamClient {
 public:
  virtual HSteamPipe CreateSteamPipe() = 0;
  virtual bool BReleaseSteamPipe(HSteamPipe pipe) = 0;
  virtual HSteamUser ConnectToGlobalUser(HSteamPipe pipe) = 0;
  virtual HSteamUser CreateLocalUser(HSteamPipe* pipe, uint32_t accountType) = 0;
  virtual void ReleaseUser(HSteamPipe pipe, HSteamUser user) = 0;
  virtual ISteamUser* GetISteamUser(HSteamUser u, HSteamPipe p,
                                    const char* version) = 0;
  virtual ISteamGameServer* GetISteamGameServer(HSteamUser u, HSteamPipe p,
                                                const char* version) = 0;
  virtual void SetLocalIPBinding(const void* addr, uint16_t port) = 0;
  virtual ISteamFriends* GetISteamFriends(HSteamUser u, HSteamPipe p,
                                          const char* version) = 0;
  virtual ISteamUtils* GetISteamUtils(HSteamPipe p, const char* version) = 0;
  virtual ISteamMatchmaking* GetISteamMatchmaking(HSteamUser u, HSteamPipe p,
                                                  const char* version) = 0;
  virtual ISteamMatchmakingServers* GetISteamMatchmakingServers(
      HSteamUser u, HSteamPipe p, const char* version) = 0;
  virtual void* GetISteamGenericInterface(HSteamUser u, HSteamPipe p,
                                          const char* version) = 0;
  virtual ISteamUserStats* GetISteamUserStats(HSteamUser u, HSteamPipe p,
                                              const char* version) = 0;
  // ... truncated
};

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
