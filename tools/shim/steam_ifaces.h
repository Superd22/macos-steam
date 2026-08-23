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
  virtual CSteamID_t GetSteamID() = 0;                                          // 2
  virtual int  InitiateGameConnection_DEPRECATED(void*, int, CSteamID_t, uint32_t, uint16_t, bool) = 0; // 3
  virtual void TerminateGameConnection_DEPRECATED(uint32_t, uint16_t) = 0;      // 4
  virtual void TrackAppUsageEvent(uint64_t, int, const char*) = 0;              // 5
  virtual bool GetUserDataFolder(char*, int) = 0;                               // 6
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

// ISteamUtils VERSION010, transcribed in full (39 slots, 0-38) from Proton's
// generated MSVC vtable, lsteamclient/winISteamUtils.c. No overloads, so the
// dylib's Itanium order equals the MSVC order steam_api64.dll expects.
// GetIPCountry (4) and GetSteamUILanguage (23) are the only const char* returns:
// stubbed they hand the game NULL, which it dereferences.
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
  virtual void     SetOverlayNotificationPosition(int32_t) = 0;      // 10
  virtual bool     IsAPICallCompleted(SteamAPICall_t, bool*) = 0;    // 11
  virtual int32_t  GetAPICallFailureReason(SteamAPICall_t) = 0;      // 12
  virtual bool     GetAPICallResult(SteamAPICall_t, void*, int, int, bool*) = 0; // 13
  virtual void     RunFrame() = 0;                                  // 14
  virtual uint32_t GetIPCCallCount() = 0;                           // 15
  virtual void     SetWarningMessageHook(void*) = 0;                // 16
  virtual bool     IsOverlayEnabled() = 0;                          // 17
  virtual bool     BOverlayNeedsPresent() = 0;                      // 18
  virtual SteamAPICall_t CheckFileSignature(const char*) = 0;       // 19
  virtual bool     ShowGamepadTextInput(int32_t, int32_t, const char*, uint32_t, const char*) = 0; // 20
  virtual uint32_t GetEnteredGamepadTextLength() = 0;               // 21
  virtual bool     GetEnteredGamepadTextInput(char*, uint32_t) = 0; // 22
  virtual const char* GetSteamUILanguage() = 0;                     // 23
  virtual bool     IsSteamRunningInVR() = 0;                        // 24
  virtual void     SetOverlayNotificationInset(int, int) = 0;       // 25
  virtual bool     IsSteamInBigPictureMode() = 0;                   // 26
  virtual void     StartVRDashboard() = 0;                          // 27
  virtual bool     IsVRHeadsetStreamingEnabled() = 0;               // 28
  virtual void     SetVRHeadsetStreamingEnabled(bool) = 0;          // 29
  virtual bool     IsSteamChinaLauncher() = 0;                      // 30
  virtual bool     InitFilterText(uint32_t) = 0;                    // 31
  virtual int      FilterText(int32_t, CSteamID_t, const char*, char*, uint32_t) = 0; // 32
  virtual int32_t  GetIPv6ConnectivityState(int32_t) = 0;           // 33
  virtual bool     IsSteamRunningOnSteamDeck() = 0;                 // 34
  virtual bool     ShowFloatingGamepadTextInput(int32_t, int, int, int, int) = 0;     // 35
  virtual void     SetGameLauncherMode(bool) = 0;                   // 36
  virtual bool     DismissFloatingGamepadTextInput() = 0;           // 37
  virtual bool     DismissGamepadTextInput() = 0;                   // 38
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

// ISteamApps VERSION008 leading block (slots 0-9). No same-name overloads here,
// so the dylib's Itanium order equals the MSVC order steam_api64.dll expects, and
// this block is identical across ISteamApps 006-008. Games call these at init
// (ownership + language); a stubbed const-char* return (GetCurrentGameLanguage)
// hands the game NULL and it faults — the #12 Mars-boot crash. Tail truncated.
class ISteamApps008 {
 public:
  virtual bool BIsSubscribed() = 0;                                    // 0
  virtual bool BIsLowViolence() = 0;                                   // 1
  virtual bool BIsCybercafe() = 0;                                     // 2
  virtual bool BIsVACBanned() = 0;                                     // 3
  virtual const char* GetCurrentGameLanguage() = 0;                    // 4
  virtual const char* GetAvailableGameLanguages() = 0;                 // 5
  virtual bool BIsSubscribedApp(AppId_t) = 0;                          // 6
  virtual bool BIsDlcInstalled(AppId_t) = 0;                           // 7
  virtual uint32_t GetEarliestPurchaseUnixTime(AppId_t) = 0;           // 8
  virtual bool BIsSubscribedFromFreeWeekend() = 0;                     // 9
  virtual int  GetDLCCount() = 0;                                      // 10
  virtual bool BGetDLCDataByIndex(int, AppId_t*, bool*, char*, int) = 0; // 11
  virtual void InstallDLC(AppId_t) = 0;                                // 12
  virtual void UninstallDLC(AppId_t) = 0;                              // 13
  virtual void RequestAppProofOfPurchaseKey(AppId_t) = 0;              // 14
  virtual bool GetCurrentBetaName(char*, int) = 0;                     // 15
  virtual bool MarkContentCorrupt(bool) = 0;                           // 16
  virtual uint32_t GetInstalledDepots(AppId_t, uint32_t*, uint32_t) = 0; // 17
  virtual uint32_t GetAppInstallDir(AppId_t, char*, uint32_t) = 0;     // 18
  virtual bool BIsAppInstalled(AppId_t) = 0;                           // 19
  virtual CSteamID_t GetAppOwner() = 0;                                // 20
  virtual const char* GetLaunchQueryParam(const char*) = 0;            // 21
  // ... truncated
};

// ISteamInput VERSION006 leading block. Mars calls Init() at boot and logs
// "SteamInput failed to initialize!" when it returns false — after which its
// render thread null-writes. Slot order from Proton's winISteamInput.c.
class ISteamInput006 {
 public:
  virtual bool Init(bool bExplicitlyCallRunFrame) = 0;                 // 0
  virtual bool Shutdown() = 0;                                         // 1
  virtual bool SetInputActionManifestFilePath(const char*) = 0;        // 2
  virtual void RunFrame(bool bReservedValue) = 0;                      // 3
  virtual bool BWaitForData(bool, uint32_t) = 0;                       // 4
  virtual bool BNewDataAvailable() = 0;                                // 5
  virtual int  GetConnectedControllers(uint64_t*) = 0;                 // 6
  // ... truncated
};

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
typedef bool  (*Fn_BConnected)(HSteamUser, HSteamPipe);
typedef bool  (*Fn_BLoggedOn)(HSteamUser, HSteamPipe);
