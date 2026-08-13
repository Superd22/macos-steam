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
