/* shim_abi.h — the seam contract shared by the PE half (steamclient64.dll) and
 * the unix half (steamclient64.so) of the achievement shim (#11).
 *
 * Both halves are x86_64 (mingw PE, clang++ Mach-O .so) in one address space, so
 * pointers cross verbatim and there is no bitness or struct-packing question for
 * the scalars and pointers passed here. Every params struct is laid out
 * widest-first (uint64 handles/pointers, then int32, then the small ret) so the
 * two compilers agree field-for-field.
 *
 * The seam is Wine's __wine_unix_call(handle, code, args): `code` indexes
 * __wine_unix_call_funcs[] in shim_unix.cpp, so this enum MUST match that array's
 * order. Callback payload structs (1101/1102/1103) and CallbackMsg_t are NOT
 * marshalled — they are byte-identical macOS<->Windows x64 (#3 §7.3), so the
 * pump forwards native pointers straight through.
 */
#pragma once
#include <stdint.h>

enum shim_call
{
    /* lifecycle / flat exports */
    C_CreateInterface = 0,      /* dylib CreateInterface(name) -> ISteamClient handle   */
    C_BGetCallback,             /* Steam_BGetCallback(pipe, CallbackMsg_t*) passthrough  */
    C_FreeLastCallback,         /* Steam_FreeLastCallback(pipe)                          */
    C_GetAPICallResult,         /* Steam_GetAPICallResult(...) passthrough               */
    C_ReleaseThreadLocalMemory, /* Steam_ReleaseThreadLocalMemory(flag)                  */

    /* ISteamClient */
    C_Client_CreateSteamPipe,
    C_Client_BReleaseSteamPipe,
    C_Client_ConnectToGlobalUser,
    C_Client_ReleaseUser,
    C_Client_GetGeneric,        /* GetISteamGenericInterface(user,pipe,ver) -> handle    */

    /* ISteamUser */
    C_User_GetHSteamUser,
    C_User_BLoggedOn,
    C_User_GetSteamID,

    /* ISteamUtils */
    C_Utils_GetAppID,

    /* ISteamUserStats (VERSION012) */
    C_Stats_RequestCurrentStats,
    C_Stats_GetStatInt,
    C_Stats_SetStatInt,
    C_Stats_GetAchievement,
    C_Stats_SetAchievement,
    C_Stats_ClearAchievement,
    C_Stats_GetAchievementAndUnlockTime,
    C_Stats_StoreStats,
    C_Stats_GetNumAchievements,
    C_Stats_GetAchievementName,
    C_Stats_ResetAllStats,
    C_Stats_GetAchievementDisplayAttribute,

    /* ISteamApps (VERSION008 leading block, slots 0-9) — appended AFTER the
     * stats block so existing opcode indices are undisturbed. */
    C_Apps_BIsSubscribed,
    C_Apps_BIsLowViolence,
    C_Apps_BIsCybercafe,
    C_Apps_BIsVACBanned,
    C_Apps_GetCurrentGameLanguage,
    C_Apps_GetAvailableGameLanguages,
    C_Apps_BIsSubscribedApp,
    C_Apps_BIsDlcInstalled,
    C_Apps_GetEarliestPurchaseUnixTime,
    C_Apps_BIsSubscribedFromFreeWeekend,

    /* Appended after the Apps block; opcode indices above are undisturbed. */
    C_Apps_GetAppOwner,
    C_Apps_GetLaunchQueryParam,

    C_User_GetUserDataFolder,

    /* ISteamUtils VERSION010 beyond GetAppID. */
    C_Utils_GetSecondsSinceAppActive,
    C_Utils_GetSecondsSinceComputerActive,
    C_Utils_GetConnectedUniverse,
    C_Utils_GetServerRealTime,
    C_Utils_GetIPCountry,
    C_Utils_GetCurrentBatteryPower,
    C_Utils_IsAPICallCompleted,
    C_Utils_GetAPICallFailureReason,
    C_Utils_GetAPICallResult,
    C_Utils_RunFrame,
    C_Utils_GetIPCCallCount,
    C_Utils_GetSteamUILanguage,

    /* ISteamInput VERSION006 */
    C_Input_Init,
    C_Input_Shutdown,
    C_Input_RunFrame,
    C_Input_BNewDataAvailable,
    C_Input_GetConnectedControllers,

    C_COUNT
};

/* ---- flat exports ---- */
struct sp_create_interface { uint64_t name; uint64_t ret; };              /* ret = ISteamClient* */
struct sp_bgetcallback     { uint64_t msg; int32_t pipe; int32_t ret; };  /* ret = bool          */
struct sp_freelast         { int32_t pipe; };
struct sp_apicallresult {
    uint64_t call; uint64_t cb; uint64_t failed;                          /* pointers            */
    int32_t pipe; int32_t cub; int32_t expected; int32_t ret;            /* ret = bool          */
};
struct sp_release_tls      { int32_t flag; };

/* ---- ISteamClient ---- */
struct sp_client_noarg     { uint64_t handle; int32_t ret; };             /* CreateSteamPipe      */
struct sp_client_pipe      { uint64_t handle; int32_t pipe; int32_t ret; }; /* BRelease/ConnectGU */
struct sp_client_releaseu  { uint64_t handle; int32_t pipe; int32_t user; };
struct sp_client_getgen    { uint64_t handle; uint64_t ver; uint64_t ret; int32_t user; int32_t pipe; };

/* ---- ISteamUser ---- */
struct sp_user_i32         { uint64_t handle; int32_t ret; };             /* GetHSteamUser/BLoggedOn */
struct sp_user_u64         { uint64_t handle; uint64_t ret; };            /* GetSteamID              */

/* ---- ISteamUtils ---- */
struct sp_utils_u32        { uint64_t handle; uint32_t ret; };            /* GetAppID                */

/* ---- ISteamUserStats ---- */
struct sp_stats_noarg      { uint64_t handle; int32_t ret; };             /* RequestCurrentStats/StoreStats */
struct sp_stats_name       { uint64_t handle; uint64_t name; int32_t ret; };
struct sp_stats_getach     { uint64_t handle; uint64_t name; uint64_t achieved; int32_t ret; };
struct sp_stats_getachtime { uint64_t handle; uint64_t name; uint64_t achieved; uint64_t unlock; int32_t ret; };
struct sp_stats_statint    { uint64_t handle; uint64_t name; uint64_t data; int32_t val; int32_t ret; };
struct sp_stats_u32ret     { uint64_t handle; uint32_t ret; };            /* GetNumAchievements      */
struct sp_stats_nameidx    { uint64_t handle; uint64_t ret; uint32_t idx; }; /* GetAchievementName -> const char* */
struct sp_stats_reset      { uint64_t handle; int32_t achievements_too; int32_t ret; };
struct sp_stats_dispattr   { uint64_t handle; uint64_t name; uint64_t key; uint64_t ret; };

/* ---- ISteamApps (VERSION008) ---- */
struct sp_apps_bool        { uint64_t handle; int32_t ret; };                     /* BIsSubscribed/LowViolence/Cybercafe/VACBanned/FreeWeekend */
struct sp_apps_str         { uint64_t handle; uint64_t ret; };                    /* GetCurrentGameLanguage/GetAvailableGameLanguages -> const char* */
struct sp_apps_appid_bool  { uint64_t handle; uint32_t appid; int32_t ret; };     /* BIsSubscribedApp/BIsDlcInstalled */
struct sp_apps_appid_u32   { uint64_t handle; uint32_t appid; uint32_t ret; };    /* GetEarliestPurchaseUnixTime */
struct sp_apps_u64         { uint64_t handle; uint64_t ret; };                    /* GetAppOwner -> CSteamID by value */
struct sp_apps_qparam      { uint64_t handle; uint64_t key; uint64_t ret; };      /* GetLaunchQueryParam -> const char* */

/* ---- ISteamUser (appended) ---- */
struct sp_user_datafolder  { uint64_t handle; uint64_t buf; int32_t len; int32_t ret; }; /* GetUserDataFolder */

/* ---- ISteamUtils (VERSION010) ---- */
/* sp_utils_u32 (declared above for GetAppID) also carries every no-arg uint32
 * getter: GetSecondsSinceAppActive/ComputerActive, GetServerRealTime,
 * GetIPCCallCount, GetCurrentBatteryPower. */
struct sp_utils_i32        { uint64_t handle; int32_t ret; };                     /* GetConnectedUniverse */
struct sp_utils_str        { uint64_t handle; uint64_t ret; };                    /* GetIPCountry/GetSteamUILanguage -> const char* */
struct sp_utils_call       { uint64_t handle; uint64_t call; uint64_t failed; int32_t ret; }; /* IsAPICallCompleted */
struct sp_utils_callfail   { uint64_t handle; uint64_t call; int32_t ret; };      /* GetAPICallFailureReason */
struct sp_utils_callres    { uint64_t handle; uint64_t call; uint64_t buf; int32_t cub; int32_t expected; uint64_t failed; int32_t ret; }; /* GetAPICallResult */
struct sp_utils_void       { uint64_t handle; };                                  /* RunFrame */

/* ---- ISteamInput (VERSION006) ---- */
struct sp_input_init       { uint64_t handle; int32_t explicit_runframe; int32_t ret; };
struct sp_input_bool       { uint64_t handle; int32_t ret; };
struct sp_input_runframe   { uint64_t handle; int32_t reserved; };
struct sp_input_handles    { uint64_t handle; uint64_t out; int32_t ret; };
