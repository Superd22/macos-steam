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

    /* Copy-out of NATIVE-owned memory (#20, i386 only in practice).
     * Anything the game passes IN is a PE address that zero-extends into the
     * uint64 params below and is written by the native side directly. But
     * anything the native side hands BACK by pointer — a const char* return, a
     * callback payload — lives on the dylib's heap, which on macOS sits well
     * above 4 GB (0x7fd695b48ae0, straight out of our own shim_unix.log). A
     * 32-bit PE cannot dereference that address: it does not fit in a pointer.
     * So the bytes must be copied down into PE memory across the seam. The
     * 64-bit PE has no such problem and does not use these. */
    C_CopyMem,                  /* memcpy(dst, src, len)                        */
    C_CopyStr,                  /* strlcpy(dst, src, cap) -> ret = source length */

    /* ISteamUser encrypted app ticket (#20). Among Us does not use Steam for
     * its own account: it authenticates to Epic Online Services, and EOS's
     * "Auth with Steam" path asks Steam for an ENCRYPTED APP TICKET. Left
     * stubbed, RequestEncryptedAppTicket returns 0, no SteamAPICall_t is ever
     * issued, the EncryptedAppTicketResponse_t never arrives, and the game
     * sits on its loading screen until EOSManager::ShowTimeout() fires. */
    C_User_RequestEncryptedAppTicket,
    C_User_GetEncryptedAppTicket,

    /* ISteamFriends (#29). Space Marine reads its own persona name during
     * startup; stubbed, that is a NULL const char* and the title faults on it —
     * the same shape as the ISteamApps::GetCurrentGameLanguage crash in #12. */
    C_Friends_GetPersonaName,

    /* ISteamFriends overlay activation (#23). Unlike everything above, these are
     * not questions the title asks — they are the title telling Steam to put a
     * panel on screen: "Buy DLC", "View profile", "Invite friend". Stubbed, every
     * one of those is a dead button.
     *
     * Each params struct carries `slot`, the native vtable index, and that is
     * load-bearing rather than defensive. ISteamFriends moved ActivateGameOverlay
     * through slots 19, 20, 21, 22, 28 and 27 across the fifteen versions that
     * declare it, so casting the native handle to one fixed C++ class — the
     * pattern every other block here uses — would dispatch to a DIFFERENT method
     * on thirteen of them. The PE half already resolves each method against its
     * own version's generated table, so it sends the slot it found. ISteamFriends
     * has no same-name overload in any version (checked against vtables.json),
     * which is what makes that slot number transferable: it is the one case where
     * the MSVC order the PE side holds and the native Itanium order cannot
     * diverge. */
    C_Friends_ActivateOverlay,
    C_Friends_ActivateOverlayToUser,
    C_Friends_ActivateOverlayToWebPage,
    C_Friends_ActivateOverlayToStore,
    C_Friends_ActivateOverlayInviteDialog,
    C_Friends_ActivateOverlayRemotePlay,
    C_Friends_ActivateOverlayConnectString,
    C_Friends_RegisterProtocolInOverlayBrowser,

    /* Overlay predicates (#23) — answered by Valve's RENDERER, not by
     * steamclient.dylib, and so they take no interface handle. The renderer is
     * already in our address space (overlay_load below dlopens it) and exports
     * exactly these, so forwarding makes the answer correct BY CONSTRUCTION:
     * no renderer loaded, no symbol, false. That matters more here than
     * anywhere else in this enum, because IsOverlayEnabled is the one call
     * whose wrong answer is not a wrong pixel — a title told `true` with
     * nothing to draw pauses forever waiting for a panel that never comes. */
    C_Overlay_IsEnabled,
    C_Overlay_BNeedsPresent,
    C_Overlay_SetNotificationPosition,
    C_Overlay_SetNotificationInset,

    /* ISteamRemoteStorage (slots 0-23 of VERSION014/016) — the Steam Cloud file
     * surface (#43). Appended after the overlay block so existing opcode indices
     * are undisturbed.
     *
     * This is the interface a title with cloud saves reads its save THROUGH: the
     * bytes on disk in userdata/<id>/<appid>/remote are Steam's business, not the
     * game's. Space Marine (3169520) never opens that directory — it asks
     * FileExists("smsave0.dsav"). Left stubbed, that is `false`, and the title
     * offers only "New Campaign" with a complete save sitting on disk. FileWrite
     * being stubbed alongside it is why the same run cannot save either.
     *
     * Unlike ISteamFriends' overlay block, these need no `slot` field: no version
     * of ISteamRemoteStorage declares a same-name overload (checked against
     * vtables.json), so the dylib's Itanium order IS the MSVC order the PE side
     * holds, and casting the handle to one fixed class dispatches correctly on
     * every version. Slots 0-23 are also shape-identical from v001 through v016,
     * which is what lets one wire_all per method reach them all.
     *
     * Slots 24-58 (UGC, workshop publishing, video, local-file-change batching)
     * are deliberately absent: a different subsystem, unexercised by any title in
     * hand, and each needs types the seam does not carry. */
    C_RS_FileWrite,
    C_RS_FileRead,
    C_RS_FileWriteAsync,
    C_RS_FileReadAsync,
    C_RS_FileReadAsyncComplete,
    C_RS_FileForget,
    C_RS_FileDelete,
    C_RS_FileShare,
    C_RS_SetSyncPlatforms,
    C_RS_FileWriteStreamOpen,
    C_RS_FileWriteStreamWriteChunk,
    C_RS_FileWriteStreamClose,
    C_RS_FileWriteStreamCancel,
    C_RS_FileExists,
    C_RS_FilePersisted,
    C_RS_GetFileSize,
    C_RS_GetFileTimestamp,
    C_RS_GetSyncPlatforms,
    C_RS_GetFileCount,
    C_RS_GetFileNameAndSize,
    C_RS_GetQuota,
    C_RS_IsCloudEnabledForAccount,
    C_RS_IsCloudEnabledForApp,
    C_RS_SetCloudEnabledForApp,

    /* Diagnostics (#45). The PE half's own dbg() writes to OutputDebugStringA and
     * to a file only when SHIM_PE_LOG is set, so in a normal run it writes
     * nowhere. That is fine for tracing and wrong for the one message that must
     * never be missed: a title calling a vtable slot nothing is wired into. That
     * call silently returns 0, and "no error" then reads as "works" — which is
     * how a complete cloud save stayed invisible through a whole play session
     * (#43). This opcode puts that one line in shim-unix.log, where every other
     * half of the stack already reports. */
    C_Log,

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
/* ret is the SteamAPICall_t handle; `data` and `ticket`/`cbticket` are PE
 * addresses the game supplies, so they zero-extend and the native side writes
 * through them directly — the copy-down path is not involved. */
struct sp_user_reqticket   { uint64_t handle; uint64_t data; uint64_t ret; int32_t cb; };
struct sp_user_getticket   { uint64_t handle; uint64_t ticket; uint64_t cbticket; int32_t max; int32_t ret; };

/* ---- ISteamFriends (VERSION017) ---- */
struct sp_friends_str      { uint64_t handle; uint64_t ret; };                    /* GetPersonaName -> const char* */
/* The overlay activators (#23). `slot` is the native vtable index the PE half
 * resolved for the version the title actually asked for — see the enum comment.
 * Strings are PE addresses the title owns, so they zero-extend and the native
 * side reads them in place; nothing is handed back, so no copy-down. */
struct sp_fr_ov_str        { uint64_t handle; uint64_t str; int32_t slot; };      /* ActivateGameOverlay, ...InviteDialogConnectString */
struct sp_fr_ov_user       { uint64_t handle; uint64_t str; uint64_t steamid; int32_t slot; }; /* ActivateGameOverlayToUser */
struct sp_fr_ov_web        { uint64_t handle; uint64_t url; int32_t slot; int32_t mode; };     /* ActivateGameOverlayToWebPage */
struct sp_fr_ov_store      { uint64_t handle; uint32_t appid; int32_t flag; int32_t slot; };   /* ActivateGameOverlayToStore */
struct sp_fr_ov_id         { uint64_t handle; uint64_t steamid; int32_t slot; };  /* ...InviteDialog, ...RemotePlayTogetherInviteDialog */
struct sp_fr_ov_proto      { uint64_t handle; uint64_t str; int32_t slot; int32_t ret; };      /* RegisterProtocolInOverlayBrowser */

/* ---- overlay predicates (#23) — renderer state, so no interface handle ---- */
struct sp_overlay_bool     { int32_t ret; };                                      /* IsOverlayEnabled/BOverlayNeedsPresent */
struct sp_overlay_pos      { int32_t pos; };                                      /* SetOverlayNotificationPosition */
struct sp_overlay_inset    { int32_t x; int32_t y; };                             /* SetOverlayNotificationInset */

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

/* ---- copy-out of native memory (#20) ---- */
struct sp_copymem          { uint64_t src; uint64_t dst; int32_t len; };
struct sp_copystr          { uint64_t src; uint64_t dst; int32_t cap; int32_t ret; };

/* ---- ISteamRemoteStorage (slots 0-23) (#43) ----
 * `data` and the out-params (`size_out`, `total`/`avail`) are addresses the GAME
 * supplies, so on i386 they zero-extend and the native side reads and writes
 * through them directly — same as GetUserDataFolder, and the copy-down path is
 * not involved. The one exception is GetFileNameAndSize, whose const char*
 * RETURN points into the dylib's heap above 4 GB; that one goes through
 * native_str on the PE side like GetIPCountry does. */
struct sp_rs_filedata      { uint64_t handle; uint64_t name; uint64_t data; int32_t count; int32_t ret; }; /* FileWrite -> bool, FileRead -> bytes read */
struct sp_rs_writeasync    { uint64_t handle; uint64_t name; uint64_t data; uint64_t ret; int32_t count; }; /* FileWriteAsync -> SteamAPICall_t */
struct sp_rs_readasync     { uint64_t handle; uint64_t name; uint64_t ret; int32_t offset; int32_t toread; }; /* FileReadAsync -> SteamAPICall_t */
struct sp_rs_readasyncdone { uint64_t handle; uint64_t call; uint64_t data; int32_t toread; int32_t ret; }; /* FileReadAsyncComplete -> bool */
struct sp_rs_name_i32      { uint64_t handle; uint64_t name; int32_t ret; };      /* FileForget/FileDelete/FileExists/FilePersisted -> bool; GetFileSize -> int32; GetSyncPlatforms -> ERemoteStoragePlatform */
struct sp_rs_name_u64      { uint64_t handle; uint64_t name; uint64_t ret; };     /* FileShare -> SteamAPICall_t; FileWriteStreamOpen -> UGCFileWriteStreamHandle_t */
struct sp_rs_name_i64      { uint64_t handle; uint64_t name; int64_t ret; };      /* GetFileTimestamp -> int64 */
struct sp_rs_syncplat      { uint64_t handle; uint64_t name; int32_t platform; int32_t ret; }; /* SetSyncPlatforms -> bool */
struct sp_rs_streamchunk   { uint64_t handle; uint64_t stream; uint64_t data; int32_t count; int32_t ret; }; /* FileWriteStreamWriteChunk -> bool */
struct sp_rs_stream        { uint64_t handle; uint64_t stream; int32_t ret; };    /* FileWriteStreamClose/Cancel -> bool */
struct sp_rs_noarg         { uint64_t handle; int32_t ret; };                     /* GetFileCount -> int32; IsCloudEnabledForAccount/App -> bool */
struct sp_rs_namesize      { uint64_t handle; uint64_t size_out; uint64_t ret; int32_t index; }; /* GetFileNameAndSize -> const char* */
struct sp_rs_quota         { uint64_t handle; uint64_t total; uint64_t avail; int32_t ret; };    /* GetQuota -> bool */
struct sp_rs_setcloud      { uint64_t handle; int32_t enabled; };                 /* SetCloudEnabledForApp -> void */

/* ---- diagnostics (#45) ---- */
struct sp_log              { uint64_t msg; };                                     /* a PE-side line, into shim-unix.log */
