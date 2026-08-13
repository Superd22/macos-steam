// steam_ifaces.h — native Steamworks interface declarations for the unix half.
//
// These are the layouts the dylib actually exposes, so they are declared in
// DECLARATION order (Itanium/clang ABI) — the same order the macOS
// steamclient.dylib was compiled with. That is the opposite of the MSVC vtable
// the PE side must present (which reverses same-name overload sets); the two are
// reconciled at the seam, and since we only ever call NON-overloaded methods
// through these classes, only the slot COUNT of the overload blocks matters, not
// which overload sits in which of the two swapped slots.
//
// Every slot up to the deepest method we call must be declared so the compiler
// emits the vtable index we expect. Tails are truncated.
//
// Versions: SteamClient020 (isteamclient.h), SteamUser021 (isteamuser.h),
// STEAMUSERSTATS_INTERFACE_VERSION012 (isteamuserstats.h — still leads with
// RequestCurrentStats; #2 proved v013 is this minus slot 0).
#pragma once
#include <cstdint>

typedef int32_t  HSteamPipe;
typedef int32_t  HSteamUser;
typedef uint64_t CSteamID_t;       // CSteamID: one uint64 by value (pack(1))
typedef uint64_t SteamAPICall_t;
typedef uint32_t AppId_t;

class ISteamUser {
 public:
  virtual HSteamUser GetHSteamUser() = 0;
  virtual bool       BLoggedOn() = 0;
  virtual CSteamID_t GetSteamID() = 0;
  // ... truncated
};

class ISteamUserStats012 {
 public:
  virtual bool RequestCurrentStats() = 0;                                       // 0
  virtual bool GetStatI(const char*, int32_t*) = 0;                             // 1
  virtual bool GetStatF(const char*, float*) = 0;                              // 2
  virtual bool SetStatI(const char*, int32_t) = 0;                             // 3
  virtual bool SetStatF(const char*, float) = 0;                               // 4
  virtual bool UpdateAvgRateStat(const char*, float, double) = 0;               // 5
  virtual bool GetAchievement(const char*, bool*) = 0;                          // 6
  virtual bool SetAchievement(const char*) = 0;                                 // 7
  virtual bool ClearAchievement(const char*) = 0;                              // 8
  virtual bool GetAchievementAndUnlockTime(const char*, bool*, uint32_t*) = 0;  // 9
  virtual bool StoreStats() = 0;                                               // 10
  virtual int  GetAchievementIcon(const char*) = 0;                            // 11
  virtual const char* GetAchievementDisplayAttribute(const char*, const char*) = 0; // 12
  virtual bool IndicateAchievementProgress(const char*, uint32_t, uint32_t) = 0;    // 13
  virtual uint32_t GetNumAchievements() = 0;                                   // 14
  virtual const char* GetAchievementName(uint32_t) = 0;                        // 15
  virtual SteamAPICall_t RequestUserStats(CSteamID_t) = 0;                     // 16
  virtual bool GetUserStatI(CSteamID_t, const char*, int32_t*) = 0;            // 17
  virtual bool GetUserStatF(CSteamID_t, const char*, float*) = 0;              // 18
  virtual bool GetUserAchievement(CSteamID_t, const char*, bool*) = 0;         // 19
  virtual bool GetUserAchievementAndUnlockTime(CSteamID_t, const char*, bool*, uint32_t*) = 0; // 20
  virtual bool ResetAllStats(bool) = 0;                                        // 21
  // ... truncated
};

// ISteamUtils VERSION010: GetAppID sits at slot 9 (steam_api64.dll's init calls
// it to validate the app context against steam_appid.txt). No overloads, so the
// dylib's Itanium order equals the MSVC order steam_api64.dll expects.
class ISteamUtils010 {
 public:
  virtual uint32_t GetSecondsSinceAppActive() = 0;                  // 0
  virtual uint32_t GetSecondsSinceComputerActive() = 0;             // 1
  virtual int      GetConnectedUniverse() = 0;                      // 2
  virtual uint32_t GetServerRealTime() = 0;                         // 3
  virtual const char* GetIPCountry() = 0;                           // 4
  virtual bool     GetImageSize(int, uint32_t*, uint32_t*) = 0;     // 5
  virtual bool     GetImageRGBA(int, uint8_t*, int) = 0;            // 6
  virtual bool     GetCSERIPPort(uint32_t*, uint16_t*) = 0;         // 7
  virtual uint8_t  GetCurrentBatteryPower() = 0;                    // 8
  virtual AppId_t  GetAppID() = 0;                                  // 9
  // ... truncated
};

class ISteamClient {
 public:
  virtual HSteamPipe CreateSteamPipe() = 0;                                    // 0
  virtual bool       BReleaseSteamPipe(HSteamPipe) = 0;                        // 1
  virtual HSteamUser ConnectToGlobalUser(HSteamPipe) = 0;                      // 2
  virtual HSteamUser CreateLocalUser(HSteamPipe*, uint32_t) = 0;               // 3
  virtual void       ReleaseUser(HSteamPipe, HSteamUser) = 0;                  // 4
  virtual ISteamUser* GetISteamUser(HSteamUser, HSteamPipe, const char*) = 0;  // 5
  virtual void*      GetISteamGameServer(HSteamUser, HSteamPipe, const char*) = 0; // 6
  virtual void       SetLocalIPBinding(const void*, uint16_t) = 0;             // 7
  virtual void*      GetISteamFriends(HSteamUser, HSteamPipe, const char*) = 0; // 8
  virtual void*      GetISteamUtils(HSteamPipe, const char*) = 0;              // 9
  virtual void*      GetISteamMatchmaking(HSteamUser, HSteamPipe, const char*) = 0; // 10
  virtual void*      GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char*) = 0; // 11
  virtual void*      GetISteamGenericInterface(HSteamUser, HSteamPipe, const char*) = 0;    // 12
  virtual void*      GetISteamUserStats(HSteamUser, HSteamPipe, const char*) = 0; // 13
  // ... truncated
};

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
typedef bool  (*Fn_BConnected)(HSteamUser, HSteamPipe);
typedef bool  (*Fn_BLoggedOn)(HSteamUser, HSteamPipe);
