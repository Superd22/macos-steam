// Minimal hand-rolled Steamworks interface declarations.
//
// The Steamworks SDK is not vendored here; these are the vtable layouts the
// probe needs, transcribed from the public SDK headers. Only the leading slots
// are declared — every method up to the one we call must be present so the
// vtable indices line up, but the tail can be truncated.
//
// Sources: isteamclient.h (SteamClient017-023), isteamuser.h (SteamUser021-023),
// isteamfriends.h (SteamFriends017-018), isteamuserstats.h (VERSION012/013).

#pragma once

#include <cstdint>

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint64_t CSteamID_t;  // CSteamID is a single uint64 by value
typedef uint32_t AppId_t;

// Opaque interfaces we never call through — declared so the signatures below
// stay honest about what the vtable slot actually returns.
class ISteamGameServer;
class ISteamMatchmaking;
class ISteamMatchmakingServers;
class ISteamUtils;

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

// STEAMUSERSTATS_INTERFACE_VERSION013 drops RequestCurrentStats — the whole
// vtable shifts up by one relative to 012. Proved empirically: calling slot 0
// with no arguments on a v013 pointer lands in GetStat and Steam itself prints
// "[S_API WARN] GetStat() failed, stat (null) does not exist".
class ISteamUserStats013 {
 public:
  virtual bool GetStatInt(const char* name, int32_t* data) = 0;
  virtual bool GetStatFloat(const char* name, float* data) = 0;
  virtual bool SetStatInt(const char* name, int32_t data) = 0;
  virtual bool SetStatFloat(const char* name, float data) = 0;
  virtual bool UpdateAvgRateStat(const char* name, float countThisSession,
                                 double sessionLength) = 0;
  virtual bool GetAchievement(const char* name, bool* achieved) = 0;
  virtual bool SetAchievement(const char* name) = 0;
  virtual bool ClearAchievement(const char* name) = 0;
  virtual bool GetAchievementAndUnlockTime(const char* name, bool* achieved,
                                           uint32_t* unlockTime) = 0;
  virtual bool StoreStats() = 0;
  // ... truncated
};

// VERSION012 and earlier still lead with RequestCurrentStats.
class ISteamUserStats {
 public:
  virtual bool RequestCurrentStats() = 0;
  virtual bool GetStatInt(const char* name, int32_t* data) = 0;
  virtual bool GetStatFloat(const char* name, float* data) = 0;
  virtual bool SetStatInt(const char* name, int32_t data) = 0;
  virtual bool SetStatFloat(const char* name, float data) = 0;
  virtual bool UpdateAvgRateStat(const char* name, float countThisSession,
                                 double sessionLength) = 0;
  virtual bool GetAchievement(const char* name, bool* achieved) = 0;
  virtual bool SetAchievement(const char* name) = 0;
  virtual bool ClearAchievement(const char* name) = 0;
  virtual bool GetAchievementAndUnlockTime(const char* name, bool* achieved,
                                           uint32_t* unlockTime) = 0;
  virtual bool StoreStats() = 0;
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
