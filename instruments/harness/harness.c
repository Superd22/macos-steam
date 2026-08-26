/*
 * harness.exe — repeatable achievement test loop against Spacewar (appid 480).
 *
 * Runs the destination's exact call path against whatever Steam client the
 * steam_api64.dll beside it can reach:
 *
 *   SteamAPI_Init -> RequestCurrentStats -> [UserStatsReceived_t 1101]
 *     -> SetAchievement -> StoreStats -> [UserStatsStored_t 1102,
 *        UserAchievementStored_t 1103] -> verify -> ResetAllStats(true)
 *     -> RequestCurrentStats -> verify cleared
 *
 * Idempotent by construction: every "loop" run ends with ResetAllStats, so the
 * achievement is never burned. steam_appid.txt (480) beside the exe makes it
 * init as Spacewar with no game installed.
 *
 * Uses the manual-dispatch flat API (SteamAPI_ManualDispatch_*) so callbacks
 * arrive as raw structs we hex-dump — the dump IS the reference trace the
 * bridge shim must reproduce. Every export is resolved via GetProcAddress and
 * missing ones are reported, never assumed.
 *
 * Modes:
 *   harness.exe            full loop (set -> verify -> reset -> verify)
 *   harness.exe status     init + list achievements, mutate nothing
 *   harness.exe set [ACH]  set + store one achievement (default ACH_WIN_ONE_GAME)
 *   harness.exe reset      ResetAllStats(true) only
 *   harness.exe ticket     RequestEncryptedAppTicket -> completion -> fetch
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* The overlay switch is one predicate, owned by src/layout/layout.json (#33).
 * An instrument that re-derives the rule it is measuring can pass while the
 * shipped stack is wrong, which is exactly the divergence #33 closed — so this
 * asks the same generated function the two halves of the shim ask. */
#include "shim_policy.h"

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;

/* CallbackMsg_t, Windows x64 layout (pack 8): 4+4+8+4(+4 pad) = 24 bytes */
typedef struct {
    HSteamUser m_hSteamUser;
    int32_t    m_iCallback;
    uint8_t   *m_pubParam;
    int32_t    m_cubParam;
} CallbackMsg_t;

#define CBID_EncryptedAppTicketResponse 154
#define CBID_SteamAPICallCompleted   703
#define CBID_UserStatsReceived      1101
#define CBID_UserStatsStored        1102
#define CBID_UserAchievementStored  1103

/* ---- resolved exports ------------------------------------------------- */

static int   (*p_Init)(void);
static void  (*p_Shutdown)(void);
static void  (*p_MD_Init)(void);
static void  (*p_MD_RunFrame)(HSteamPipe);
static int   (*p_MD_GetNextCallback)(HSteamPipe, CallbackMsg_t *);
static void  (*p_MD_FreeLastCallback)(HSteamPipe);
static HSteamPipe (*p_GetHSteamPipe)(void);
static HSteamUser (*p_GetHSteamUser)(void);
static void *(*p_FindOrCreateUserInterface)(HSteamUser, const char *);

static int      (*p_User_BLoggedOn)(void *);
static uint64_t (*p_User_GetSteamID)(void *);

/* The encrypted-app-ticket path. A title that authenticates to a third-party
 * backend (EOS for Among Us, Relic Online for AoE IV) asks Steam for this
 * ticket and blocks its sign-in on the answer, so "the request went out and
 * nothing came back" is a whole class of loading-screen hang. */
static uint64_t (*p_User_RequestEncryptedAppTicket)(void *, void *, int);
static int      (*p_User_GetEncryptedAppTicket)(void *, void *, int, uint32_t *);
static int      (*p_Ut_IsAPICallCompleted)(void *, uint64_t, int *);
static int      (*p_Ut_GetAPICallFailureReason)(void *, uint64_t);

static int      (*p_Stats_RequestCurrentStats)(void *);
static int      (*p_Stats_GetAchievement)(void *, const char *, int *);
static int      (*p_Stats_GetAchievementAndUnlockTime)(void *, const char *, int *, uint32_t *);
static int      (*p_Stats_SetAchievement)(void *, const char *);
static int      (*p_Stats_ClearAchievement)(void *, const char *);
static int      (*p_Stats_StoreStats)(void *);
static int      (*p_Stats_ResetAllStats)(void *, int);
static uint32_t (*p_Stats_GetNumAchievements)(void *);
/* The two GetStat/SetStat overloads (#78). Valve declares each pair as one
 * overloaded name; the flat API disambiguates with an Int32/Float suffix, and
 * Proton's vtable with a `_2`. They are the reason this mode exists. */
static int      (*p_Stats_GetStatI)(void *, const char *, int32_t *);
static int      (*p_Stats_GetStatF)(void *, const char *, float *);
static int      (*p_Stats_SetStatI)(void *, const char *, int32_t);
static int      (*p_Stats_SetStatF)(void *, const char *, float);
static const char *(*p_Stats_GetAchievementName)(void *, uint32_t);
static const char *(*p_Stats_GetAchievementDisplayAttribute)(void *, const char *, const char *);

/* ---- overlay API (#23) --------------------------------------------------
 * The twelve slots #23 wires. Every one is reached through Valve's own flat
 * function, which does the MSVC thiscall dispatch into the shim's generated
 * vtable internally — so calling them here exercises the exact path a title
 * takes: right slot, right arity, right calling convention, on both bitnesses.
 * A wrong slot or a wrong `ret N` shows up as a crash or garbage here, not as
 * a subtly wrong pixel three layers away. */
static void *(*p_SteamFriends_v017)(void);
static void *(*p_SteamUtils_v010)(void);

static void (*p_Fr_ActivateGameOverlay)(void *, const char *);
static void (*p_Fr_ActivateGameOverlayToUser)(void *, const char *, uint64_t);
static void (*p_Fr_ActivateGameOverlayToWebPage)(void *, const char *, int);
static void (*p_Fr_ActivateGameOverlayToStore)(void *, uint32_t, int);
static void (*p_Fr_ActivateGameOverlayInviteDialog)(void *, uint64_t);
static void (*p_Fr_ActivateGameOverlayRemotePlay)(void *, uint64_t);
static void (*p_Fr_ActivateGameOverlayConnectString)(void *, const char *);
static int  (*p_Fr_RegisterProtocolInOverlayBrowser)(void *, const char *);

static int  (*p_Ut_IsOverlayEnabled)(void *);
static int  (*p_Ut_BOverlayNeedsPresent)(void *);
static void (*p_Ut_SetOverlayNotificationPosition)(void *, int);
static void (*p_Ut_SetOverlayNotificationInset)(void *, int, int);

static void  (*p_RunCallbacks)(void);
static void  (*p_RegisterCallback)(void *, int);
static void  (*p_UnregisterCallback)(void *);

static void *g_user;   /* ISteamUser* */
static void *g_stats;  /* ISteamUserStats* */
static HSteamPipe g_pipe;
static int g_use_md;   /* 1 = manual dispatch pump, 0 = classic RegisterCallback pump */

#define RESOLVE(var, name, required)                                   \
    do {                                                               \
        *(FARPROC *)&(var) = GetProcAddress(dll, name);                \
        printf("resolve %-52s %s\n", name, (var) ? "ok" : "MISSING"); \
        if (!(var) && (required)) { fprintf(stderr, "FATAL: required export %s missing\n", name); return 0; } \
    } while (0)

static int resolve_all(HMODULE dll)
{
    RESOLVE(p_Init,                "SteamAPI_Init", 1);
    RESOLVE(p_Shutdown,            "SteamAPI_Shutdown", 1);
    RESOLVE(p_MD_Init,             "SteamAPI_ManualDispatch_Init", 1);
    RESOLVE(p_MD_RunFrame,         "SteamAPI_ManualDispatch_RunFrame", 1);
    RESOLVE(p_MD_GetNextCallback,  "SteamAPI_ManualDispatch_GetNextCallback", 1);
    RESOLVE(p_MD_FreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback", 1);
    RESOLVE(p_GetHSteamPipe,       "SteamAPI_GetHSteamPipe", 1);
    RESOLVE(p_GetHSteamUser,       "SteamAPI_GetHSteamUser", 1);
    RESOLVE(p_FindOrCreateUserInterface, "SteamInternal_FindOrCreateUserInterface", 1);

    RESOLVE(p_RunCallbacks,       "SteamAPI_RunCallbacks", 1);
    RESOLVE(p_RegisterCallback,   "SteamAPI_RegisterCallback", 1);
    RESOLVE(p_UnregisterCallback, "SteamAPI_UnregisterCallback", 1);

    RESOLVE(p_User_BLoggedOn,  "SteamAPI_ISteamUser_BLoggedOn", 1);
    RESOLVE(p_User_GetSteamID, "SteamAPI_ISteamUser_GetSteamID", 1);
    RESOLVE(p_User_RequestEncryptedAppTicket, "SteamAPI_ISteamUser_RequestEncryptedAppTicket", 0);
    RESOLVE(p_User_GetEncryptedAppTicket,     "SteamAPI_ISteamUser_GetEncryptedAppTicket", 0);
    RESOLVE(p_Ut_IsAPICallCompleted,          "SteamAPI_ISteamUtils_IsAPICallCompleted", 0);
    RESOLVE(p_Ut_GetAPICallFailureReason,     "SteamAPI_ISteamUtils_GetAPICallFailureReason", 0);

    RESOLVE(p_Stats_RequestCurrentStats,        "SteamAPI_ISteamUserStats_RequestCurrentStats", 1);
    RESOLVE(p_Stats_GetAchievement,             "SteamAPI_ISteamUserStats_GetAchievement", 1);
    RESOLVE(p_Stats_GetAchievementAndUnlockTime,"SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime", 0);
    RESOLVE(p_Stats_SetAchievement,             "SteamAPI_ISteamUserStats_SetAchievement", 1);
    RESOLVE(p_Stats_ClearAchievement,           "SteamAPI_ISteamUserStats_ClearAchievement", 0);
    RESOLVE(p_Stats_StoreStats,                 "SteamAPI_ISteamUserStats_StoreStats", 1);
    RESOLVE(p_Stats_ResetAllStats,              "SteamAPI_ISteamUserStats_ResetAllStats", 1);
    RESOLVE(p_Stats_GetNumAchievements,         "SteamAPI_ISteamUserStats_GetNumAchievements", 0);
    RESOLVE(p_Stats_GetStatI,                   "SteamAPI_ISteamUserStats_GetStatInt32", 0);
    RESOLVE(p_Stats_GetStatF,                   "SteamAPI_ISteamUserStats_GetStatFloat", 0);
    RESOLVE(p_Stats_SetStatI,                   "SteamAPI_ISteamUserStats_SetStatInt32", 0);
    RESOLVE(p_Stats_SetStatF,                   "SteamAPI_ISteamUserStats_SetStatFloat", 0);
    RESOLVE(p_Stats_GetAchievementName,         "SteamAPI_ISteamUserStats_GetAchievementName", 0);
    RESOLVE(p_Stats_GetAchievementDisplayAttribute, "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute", 0);

    /* Overlay (#23). None is `required`: an older redistributable may not
     * export all twelve, and the achievement modes must still run. The overlay
     * mode reports what it could not resolve rather than aborting the run. */
    RESOLVE(p_SteamFriends_v017, "SteamAPI_SteamFriends_v017", 0);
    RESOLVE(p_SteamUtils_v010,   "SteamAPI_SteamUtils_v010", 0);
    RESOLVE(p_Fr_ActivateGameOverlay,              "SteamAPI_ISteamFriends_ActivateGameOverlay", 0);
    RESOLVE(p_Fr_ActivateGameOverlayToUser,        "SteamAPI_ISteamFriends_ActivateGameOverlayToUser", 0);
    RESOLVE(p_Fr_ActivateGameOverlayToWebPage,     "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage", 0);
    RESOLVE(p_Fr_ActivateGameOverlayToStore,       "SteamAPI_ISteamFriends_ActivateGameOverlayToStore", 0);
    RESOLVE(p_Fr_ActivateGameOverlayInviteDialog,  "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", 0);
    RESOLVE(p_Fr_ActivateGameOverlayRemotePlay,    "SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog", 0);
    RESOLVE(p_Fr_ActivateGameOverlayConnectString, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString", 0);
    RESOLVE(p_Fr_RegisterProtocolInOverlayBrowser, "SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser", 0);
    RESOLVE(p_Ut_IsOverlayEnabled,                 "SteamAPI_ISteamUtils_IsOverlayEnabled", 0);
    RESOLVE(p_Ut_BOverlayNeedsPresent,             "SteamAPI_ISteamUtils_BOverlayNeedsPresent", 0);
    RESOLVE(p_Ut_SetOverlayNotificationPosition,   "SteamAPI_ISteamUtils_SetOverlayNotificationPosition", 0);
    RESOLVE(p_Ut_SetOverlayNotificationInset,      "SteamAPI_ISteamUtils_SetOverlayNotificationInset", 0);
    return 1;
}

/* ---- trace output ----------------------------------------------------- */

static DWORD g_t0;
static void tr(const char *fmt, ...)
{
    va_list ap;
    printf("[%6lu ms] ", (unsigned long)(GetTickCount() - g_t0));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void hexdump(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i += 16) {
        printf("      %04x  ", i);
        for (int j = i; j < i + 16 && j < n; j++) printf("%02x ", p[j]);
        printf(" |");
        for (int j = i; j < i + 16 && j < n; j++)
            putchar(p[j] >= 0x20 && p[j] < 0x7f ? p[j] : '.');
        printf("|\n");
    }
    fflush(stdout);
}

static const char *cb_name(int id)
{
    switch (id) {
    case 101:                         return "SteamServersConnected_t";
    case 304:                         return "PersonaStateChange_t";
    case CBID_EncryptedAppTicketResponse: return "EncryptedAppTicketResponse_t";
    case CBID_SteamAPICallCompleted:  return "SteamAPICallCompleted_t";
    case CBID_UserStatsReceived:      return "UserStatsReceived_t";
    case CBID_UserStatsStored:        return "UserStatsStored_t";
    case CBID_UserAchievementStored:  return "UserAchievementStored_t";
    default:                          return "?";
    }
}

/* Decode the callbacks we care about (Windows x64 layouts). */
static void decode_cb(int id, const uint8_t *d, int cub)
{
    switch (id) {
    case CBID_EncryptedAppTicketResponse:   /* EResult m_eResult @0 */
        if (cub >= 4)
            tr("      -> EncryptedAppTicketResponse_t  eresult=%d", *(const int32_t *)d);
        break;
    case CBID_UserStatsReceived:  /* uint64 gameid @0, EResult @8, CSteamID @12 (pack(1)) */
        if (cub >= 20)
            tr("      -> UserStatsReceived_t  gameid=%llu eresult=%d steamid=%llu",
               (unsigned long long)*(const uint64_t *)d, *(const int32_t *)(d + 8),
               (unsigned long long)*(const uint64_t *)(d + 12));
        break;
    case CBID_UserStatsStored:    /* uint64 gameid @0, EResult @8 */
        if (cub >= 12)
            tr("      -> UserStatsStored_t    gameid=%llu eresult=%d",
               (unsigned long long)*(const uint64_t *)d, *(const int32_t *)(d + 8));
        break;
    case CBID_UserAchievementStored: /* uint64 @0, bool @8, char[128] @9, uint32 @140, @144 */
        if (cub >= 148)
            tr("      -> UserAchievementStored_t gameid=%llu group=%d name=\"%s\" cur=%u max=%u",
               (unsigned long long)*(const uint64_t *)d, d[8], (const char *)(d + 9),
               *(const uint32_t *)(d + 140), *(const uint32_t *)(d + 144));
        break;
    }
}

/* ---- classic pump: hand-built CCallbackBase vtable in C ---------------- */
/*
 * MSVC orders same-name overloads in REVERSE declaration order (map trap #2),
 * so which of the two Run slots steam_api calls for a plain callback is not
 * assumed: both slots log which one fired and funnel to the same handler.
 * GetCallbackSizeBytes sits at slot 2 either way.
 */

typedef struct {
    void   **vtbl;
    uint8_t  m_nCallbackFlags;   /* MSVC layout: vtbl @0, flags @8, id @12 */
    int32_t  m_iCallback;
    int32_t  size;               /* ours, past the ABI fields: payload bytes */
} CCallbackBase;

static int g_want;       /* callback id the current pump is waiting for */
static int g_want_seen;

static void on_callback(CCallbackBase *self, void *param, int slot)
{
    tr("CALLBACK id=%d (%s) via Run slot%d", self->m_iCallback,
       cb_name(self->m_iCallback), slot);
    hexdump(param, self->size);
    decode_cb(self->m_iCallback, param, self->size);
    if (self->m_iCallback == g_want) g_want_seen = 1;
}

/* These three ARE an MSVC vtable — steam_api calls them the way it calls any
 * CCallbackBase, i.e. __thiscall: `self` in ECX and CALLEE-cleanup. On x86_64
 * that is just the normal convention with a leading pointer and the plain C
 * signature is already right; on i386 it is not, and without the attribute
 * `self` is read from the stack (so it lands on pvParam) and nothing pops the
 * arguments. The visible symptom is a hexdump of `self->size` bytes of garbage
 * running off the end of the heap. Same rule as the shim's own vtables (#20):
 * anything that PRESENTS a vtable to Steam has to present it thiscall. */
#if defined(__i386__)
# define CB_THISCALL __attribute__((thiscall))
#else
# define CB_THISCALL
#endif
/* Slot order is MSVC's, not the header's. CCallbackBase declares
 *     virtual void Run( void *pvParam );                                  (a)
 *     virtual void Run( void *pvParam, bool, SteamAPICall_t );            (b)
 *     virtual int  GetCallbackSizeBytes();
 * and MSVC emits same-name overloads in REVERSE declaration order, so the
 * vtable is [b, a, GetCallbackSizeBytes] — the same reversal our own
 * docs/research/steamworks-vtable-tables.md warns about for Steam's interfaces.
 * x86_64 forgave having these two swapped (caller-cleanup, and the surplus
 * register args were simply ignored); i386 does not, because slot 1 then popped
 * 16 bytes when the caller had pushed 4 and the stack broke on RETURN from the
 * callback — after its body had already run and printed. */
static void CB_THISCALL run_slot0(CCallbackBase *self, void *param, int io, uint64_t hcall)
{ (void)io; (void)hcall; on_callback(self, param, 0); }
static void CB_THISCALL run_slot1(CCallbackBase *self, void *param)
{ on_callback(self, param, 1); }
static int CB_THISCALL cb_get_size(CCallbackBase *self) { return self->size; }

static void *g_cb_vtbl[3] = { (void *)run_slot0, (void *)run_slot1, (void *)cb_get_size };

static CCallbackBase g_cbs[16];
static int g_ncbs;

static void reg_cb(int id, int size)
{
    CCallbackBase *cb = &g_cbs[g_ncbs++];
    cb->vtbl = g_cb_vtbl;
    cb->m_nCallbackFlags = 0;
    cb->m_iCallback = id;
    cb->size = size;
    p_RegisterCallback(cb, id);
    tr("RegisterCallback(id=%d %s, size=%d)", id, cb_name(id), size);
}

/*
 * Pump until a callback with id `want` arrives, printing + hex-dumping every
 * callback seen. Classic pump by default; manual dispatch with --md.
 * Returns 1 if `want` seen within timeout_ms.
 */
static int pump_until(int want, int timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    g_want = want;
    g_want_seen = 0;
    while (GetTickCount() < deadline) {
        if (g_use_md) {
            p_MD_RunFrame(g_pipe);
            CallbackMsg_t msg;
            while (p_MD_GetNextCallback(g_pipe, &msg)) {
                tr("CALLBACK id=%d (%s) cub=%d huser=%d",
                   msg.m_iCallback, cb_name(msg.m_iCallback), msg.m_cubParam, msg.m_hSteamUser);
                hexdump(msg.m_pubParam, msg.m_cubParam);
                decode_cb(msg.m_iCallback, msg.m_pubParam, msg.m_cubParam);
                if (msg.m_iCallback == want) g_want_seen = 1;
                p_MD_FreeLastCallback(g_pipe);
            }
        } else {
            p_RunCallbacks();
        }
        if (g_want_seen) return 1;
        Sleep(50);
    }
    tr("TIMEOUT after %d ms waiting for callback %d (%s)", timeout_ms, want, cb_name(want));
    return 0;
}

static void print_achievement(const char *name)
{
    int ach = 0;
    if (p_Stats_GetAchievementAndUnlockTime) {
        uint32_t t = 0;
        int ok = p_Stats_GetAchievementAndUnlockTime(g_stats, name, &ach, &t); ach &= 0xff;
        tr("GetAchievementAndUnlockTime(\"%s\") ok=%d achieved=%d unlock=%u", name, ok, ach, t);
    } else {
        int ok = p_Stats_GetAchievement(g_stats, name, &ach); ach &= 0xff;
        tr("GetAchievement(\"%s\") ok=%d achieved=%d", name, ok, ach);
    }
}

static const char *SPACEWAR_ACH[] = {
    "ACH_WIN_ONE_GAME", "ACH_WIN_100_GAMES", "ACH_TRAVEL_FAR_ACCUM", "ACH_TRAVEL_FAR_SINGLE",
};

static void print_all_achievements(void)
{
    if (p_Stats_GetNumAchievements && p_Stats_GetAchievementName) {
        uint32_t n = p_Stats_GetNumAchievements(g_stats);
        tr("GetNumAchievements() = %u", n);
        for (uint32_t i = 0; i < n; i++) {
            const char *name = p_Stats_GetAchievementName(g_stats, i);
            print_achievement(name);
        }
        if (n > 0) return;
    }
    tr("falling back to hardcoded Spacewar achievement list");
    for (size_t i = 0; i < sizeof SPACEWAR_ACH / sizeof *SPACEWAR_ACH; i++)
        print_achievement(SPACEWAR_ACH[i]);
}

static int get_achieved(const char *name)
{
    int ach = 0;
    p_Stats_GetAchievement(g_stats, name, &ach);
    return ach & 0xff;
}

/* ---- stats mode: the two GetStat overloads (#78) --------------------------
 *
 * ISteamUserStats::GetStat is declared twice — once taking int32*, once taking
 * float* — and MSVC lays a same-name overload set out in REVERSE against the
 * order the macOS dylib was compiled in. So the shim cannot send the slot it
 * resolved; it has to send the reversed one. Get that backwards and the call
 * still succeeds, still returns true, and writes a float through an int32_t*.
 *
 * Nothing else in this harness can see that. The achievement modes never touch
 * an overloaded method, so they pass either way — which is precisely why this
 * mode exists: it is the only place the reversal is observable from outside.
 *
 * Spacewar (480) is the right subject because it defines both kinds against the
 * same interface: NumGames/NumWins/NumLosses are int32, FeetTraveled and
 * MaxFeetTraveled are float. A crossed pair shows up immediately as an integer
 * read out of float bits (or the reverse) rather than as an error.
 */
static int mode_stats(void)
{
    int32_t orig_i = 0, got_i = 0;
    float   orig_f = 0.0f, got_f = 0.0f;
    int bad = 0;

    if (!p_Stats_GetStatI || !p_Stats_GetStatF || !p_Stats_SetStatI || !p_Stats_SetStatF) {
        fprintf(stderr, "FATAL: stats mode needs all four GetStat/SetStat overloads; "
                        "this redistributable exports only some.\n");
        return 1;
    }

    /* Why a WRITE and not just a read: on a fresh account every one of these
     * stats is 0, and 0 is 0 in both int and float bits. Reading them proves
     * nothing about which overload answered — measured, the read-only version of
     * this mode passed identically against a shim with the reversal removed.
     *
     * Why an INCREMENT and not a set-and-restore: Spacewar's schema makes these
     * accumulate-only. `SetStat("NumGames", 0)` from 7 returns FALSE — measured —
     * so a mode that set a fixed value and put the old one back would leave the
     * client dirty while reporting that it had tidied up. Advancing the counter
     * is what the stat is for, so this mode does that instead and leaves nothing
     * it has lied about. `run.sh reset` (ResetAllStats) zeroes them.
     *
     * No StoreStats anywhere: SetStat writes the client's LOCAL cache and
     * StoreStats is what uploads it. The round-trip crosses the seam in both
     * directions either way, so committing to the account would buy nothing. */
    p_Stats_GetStatI(g_stats, "NumGames", &orig_i);
    p_Stats_GetStatF(g_stats, "FeetTraveled", &orig_f);
    const int32_t want_i = orig_i + 1;
    const float   want_f = orig_f + 1234.5f;

    tr("round-trip through both overloads (local cache only, no StoreStats)");
    tr("  before: NumGames=%d FeetTraveled=%f", (int)orig_i, (double)orig_f);
    tr("  SetStat<int32>(\"NumGames\", %d) = %d", (int)want_i,
       p_Stats_SetStatI(g_stats, "NumGames", want_i));
    tr("  SetStat<float>(\"FeetTraveled\", %f) = %d", (double)want_f,
       p_Stats_SetStatF(g_stats, "FeetTraveled", want_f));

    int oki = p_Stats_GetStatI(g_stats, "NumGames", &got_i);
    int okf = p_Stats_GetStatF(g_stats, "FeetTraveled", &got_f);
    tr("  GetStat<int32>(\"NumGames\") ok=%d value=%d   (want %d)",
       oki, (int)got_i, (int)want_i);
    tr("  GetStat<float>(\"FeetTraveled\") ok=%d value=%f (want %f)",
       okf, (double)got_f, (double)want_f);

    /* Measured against a deliberately-sabotaged shim (reversal removed): all
     * four calls come back ok=0, because the client's own schema refuses a float
     * write to an int stat and vice versa. So on THIS interface the crossing
     * surfaces as a refusal rather than as garbage. Both are checked, since an
     * interface without that safety net would show the other. */
    if (!oki || got_i != want_i) {
        tr("  ^ int32 round-trip FAILED — the int32 pair dispatched to the float "
           "pair's native slot (ok=0 is the client refusing the type; a wrong "
           "value would be the same crossing where it does not check)");
        bad++;
    }
    if (!okf || got_f != want_f) {
        tr("  ^ float round-trip FAILED — same crossing, other direction");
        bad++;
    }

    if (bad) {
        fprintf(stderr, "FATAL: %d of 2 overload round-trips wrong. The MSVC "
                        "overload reversal is broken (#78/#80).\n", bad);
        return 1;
    }
    tr("both GetStat/SetStat overloads round-tripped their own type exactly");
    return 0;
}

/* ---- overlay mode (#23) --------------------------------------------------
 *
 * Why a fake title and not a real one: these are calls the GAME makes, and a
 * game only makes them when the player clicks something. A harness can call
 * every one of them on demand, including the four (RemotePlayTogether, the
 * connect-string invite, the protocol registration, the notification inset)
 * that no title we own is known to touch — so no slot ships untested at the
 * plumbing level. What it CANNOT show is the overlay compositing: this is a
 * console exe with no swapchain, so the visible outcome here is the degraded
 * one — the native macOS Steam window coming to the front on the right page.
 * Seeing a store page drawn IN the overlay needs a real title (Surviving Mars
 * exposes ActivateGameOverlayToWebPage/ToStore and IsOverlayEnabled as Lua
 * functions, and its OpenUrl() branches on the predicate).
 *
 * Every activation is separated by a beat so a human watching can attribute
 * what appeared to what was called.
 */
static void beat(const char *what)
{
    tr("--- %s ---", what);
    Sleep(2500);
    if (p_RunCallbacks) p_RunCallbacks();
}

static int mode_overlay(uint64_t sid, const char *which)
{
    void *fr = p_SteamFriends_v017 ? p_SteamFriends_v017() : NULL;
    void *ut = p_SteamUtils_v010   ? p_SteamUtils_v010()   : NULL;
    const char *env = getenv(SHIM_ENV_OVERLAY);
    int armed_expected = shim_overlay_enabled();
    int all = (strcmp(which, "all") == 0);
    int rc = 0;

    tr("ISteamFriends(v017)=%p ISteamUtils(v010)=%p", fr, ut);
    if (!fr || !ut) {
        fprintf(stderr, "FATAL: overlay mode needs both interfaces; one is NULL. "
                        "If the shim logged 'NO VTABLE', that version is not generated.\n");
        return 3;
    }

    /* Phase 1: the predicates. This is the load-bearing pair — a title that is
     * told the overlay exists will PAUSE and wait for a panel, so a false
     * `true` is a hang, not a cosmetic bug. */
    tr("--- predicates (%s=%s, overlay %s) ---", SHIM_ENV_OVERLAY,
       env && *env ? env : "(unset)", armed_expected ? "on" : "off");
    if (p_Ut_IsOverlayEnabled) {
        int on = p_Ut_IsOverlayEnabled(ut);
        tr("IsOverlayEnabled() = %d   [expected %s]", on,
           armed_expected ? "1 if injection armed, 0 if it did not" : "0");
        /* Only one direction is unambiguously wrong. With SHIM_OVERLAY off
         * there is no renderer in the process at all, so a `true` here means
         * we are about to hang a title on a panel that cannot exist. With it
         * on, a `false` may simply mean injection lost its race — a real and
         * correctly-reported outcome, not a bug in this ticket. */
        if (!armed_expected && on) {
            fprintf(stderr, "FAIL: IsOverlayEnabled()=true with " SHIM_ENV_OVERLAY " off — "
                            "a title that pauses on activation would hang forever.\n");
            rc = 8;
        }
    } else tr("IsOverlayEnabled: export MISSING, skipped");
    if (p_Ut_BOverlayNeedsPresent)
        tr("BOverlayNeedsPresent() = %d", p_Ut_BOverlayNeedsPresent(ut));

    /* Phase 2: the setters. Void, so the only failure they can have is the one
     * that matters on i386 — a thunk popping the wrong number of bytes, which
     * corrupts the caller's stack and takes the next call with it. Surviving
     * the calls after them IS the assertion. */
    if (p_Ut_SetOverlayNotificationPosition) {
        p_Ut_SetOverlayNotificationPosition(ut, 3);   /* k_EPositionBottomRight */
        tr("SetOverlayNotificationPosition(3) returned");
    }
    if (p_Ut_SetOverlayNotificationInset) {
        p_Ut_SetOverlayNotificationInset(ut, 16, 24);
        tr("SetOverlayNotificationInset(16, 24) returned");
    }

    /* Phase 3: the activators. */
    if ((all || !strcmp(which, "store")) && p_Fr_ActivateGameOverlayToStore) {
        beat("ActivateGameOverlayToStore(480, k_EOverlayToStoreFlag_None)");
        p_Fr_ActivateGameOverlayToStore(fr, 480, 0);
    }
    if ((all || !strcmp(which, "web")) && p_Fr_ActivateGameOverlayToWebPage) {
        beat("ActivateGameOverlayToWebPage(store page, Default)");
        p_Fr_ActivateGameOverlayToWebPage(fr, "https://store.steampowered.com/app/480/", 0);
    }
    if ((all || !strcmp(which, "friends")) && p_Fr_ActivateGameOverlay) {
        beat("ActivateGameOverlay(\"Friends\")");
        p_Fr_ActivateGameOverlay(fr, "Friends");
    }
    if ((all || !strcmp(which, "user")) && p_Fr_ActivateGameOverlayToUser) {
        beat("ActivateGameOverlayToUser(\"steamid\", <self>)");
        p_Fr_ActivateGameOverlayToUser(fr, "steamid", sid);
    }
    if ((all || !strcmp(which, "invite")) && p_Fr_ActivateGameOverlayInviteDialog) {
        /* No lobby exists, so the client will decline to show anything. The
         * point is that the CALL lands: an invalid CSteamID is still eight
         * bytes crossing the seam in the right place. */
        beat("ActivateGameOverlayInviteDialog(0)");
        p_Fr_ActivateGameOverlayInviteDialog(fr, 0);
    }
    if ((all || !strcmp(which, "remoteplay")) && p_Fr_ActivateGameOverlayRemotePlay) {
        beat("ActivateGameOverlayRemotePlayTogetherInviteDialog(0)");
        p_Fr_ActivateGameOverlayRemotePlay(fr, 0);
    }
    if ((all || !strcmp(which, "connect")) && p_Fr_ActivateGameOverlayConnectString) {
        beat("ActivateGameOverlayInviteDialogConnectString(\"+connect 127.0.0.1:27015\")");
        p_Fr_ActivateGameOverlayConnectString(fr, "+connect 127.0.0.1:27015");
    }
    if ((all || !strcmp(which, "protocol")) && p_Fr_RegisterProtocolInOverlayBrowser) {
        beat("RegisterProtocolInOverlayBrowser(\"harness-probe\")");
        tr("RegisterProtocolInOverlayBrowser() = %d",
           p_Fr_RegisterProtocolInOverlayBrowser(fr, "harness-probe"));
    }

    beat("settling");
    tr("=== OVERLAY %s === (no crash means every slot dispatched with the right "
       "arity; check shim_unix.log for 'slot=' lines and the ABSENCE of "
       "'(unmapped)')", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

/* ---- mode: the encrypted app ticket -------------------------------------
 *
 * The exact call AoE IV blocks its Relic Online sign-in on, and the one #20
 * added for Among Us's EOS sign-in. A PASS/FAIL loop rather than a trace,
 * because the symptom it exists to catch is an ABSENCE — a request issued and
 * never answered — and an absence needs a deadline to be a result.
 *
 * Three distinguishable outcomes, which is the point:
 *   no call handle          the request never left
 *   handle, never completes issued and unanswered
 *   completes, no ticket    answered and refused
 */
static int mode_ticket(void)
{
    uint8_t data[5] = { 1, 2, 3, 4, 5 };   /* AoE IV sends 5 bytes; match it */
    uint8_t ticket[4096];
    uint32_t got = 0;
    uint64_t call;

    if (!p_User_RequestEncryptedAppTicket || !p_User_GetEncryptedAppTicket) {
        fprintf(stderr, "FATAL: steam_api64.dll exports no encrypted-app-ticket pair\n");
        return 2;
    }

    call = p_User_RequestEncryptedAppTicket(g_user, data, (int)sizeof data);
    tr("RequestEncryptedAppTicket(cb=%d) -> call=%llu", (int)sizeof data,
       (unsigned long long)call);
    if (!call) {
        tr("=== TICKET FAIL === no SteamAPICall_t issued");
        return 8;
    }

    /* IsAPICallCompleted, not the callback, is the authoritative signal.
     *
     * EncryptedAppTicketResponse_t is a CALL RESULT keyed on the SteamAPICall_t,
     * not a broadcast callback: steam_api only dispatches it to a CCallResult
     * registered against that exact handle, which this harness deliberately does
     * not do. So its absence from the classic pump proves nothing, and polling
     * the client directly is what separates "the client never answered" from
     * "the client answered and the answer did not get delivered". */
    void *utils = p_SteamUtils_v010 ? p_SteamUtils_v010() : NULL;
    int done = 0, failed = 0;
    DWORD deadline = GetTickCount() + 30000;
    while (GetTickCount() < deadline) {
        p_RunCallbacks();
        if (utils && p_Ut_IsAPICallCompleted &&
            p_Ut_IsAPICallCompleted(utils, call, &failed)) { done = 1; break; }
        Sleep(50);
    }
    tr("IsAPICallCompleted(call=%llu) -> done=%d failed=%d",
       (unsigned long long)call, done, failed);
    if (!done) {
        tr("=== TICKET FAIL === issued, client never completed it in 30 s");
        return 8;
    }
    if (failed && p_Ut_GetAPICallFailureReason && utils)
        tr("GetAPICallFailureReason -> %d", p_Ut_GetAPICallFailureReason(utils, call));

    if (!p_User_GetEncryptedAppTicket(g_user, ticket, (int)sizeof ticket, &got)) {
        tr("=== TICKET FAIL === call completed, GetEncryptedAppTicket refused to hand it over");
        return 8;
    }
    tr("GetEncryptedAppTicket -> %u bytes", got);
    tr("=== TICKET PASS ===");
    return 0;
}

int main(int argc, char **argv)
{
    const char *pos[2] = { "loop", "ACH_WIN_ONE_GAME" };
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--md") == 0) g_use_md = 1;
        else if (npos < 2) pos[npos++] = argv[i];
    }
    const char *mode = pos[0], *ach = pos[1];
    /* The second positional is the achievement name for the stats modes and the
     * slot selector for `overlay`; "all" is the default there, not an
     * achievement. */
    const char *ach_or_which = (npos >= 2) ? pos[1] : "all";
    g_t0 = GetTickCount();
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== steamworks harness (%s) ===\n", mode);

    /* Valve ships a different redistributable per bitness, and each looks for a
     * different steamclient under a different registry value — steam_api64.dll
     * -> steamclient64.dll via SteamClientDll64, steam_api.dll ->
     * steamclient.dll via SteamClientDll (#20). Pick by how we were built. */
#ifdef __i386__
    static const char *API_DLL = "steam_api.dll";
#else
    static const char *API_DLL = "steam_api64.dll";
#endif
    HMODULE dll = LoadLibraryA(API_DLL);
    if (!dll) { fprintf(stderr, "FATAL: LoadLibrary(%s) failed err=%lu\n", API_DLL, GetLastError()); return 2; }
    tr("LoadLibrary(%s) ok base=%p", API_DLL, (void *)dll);
    if (!resolve_all(dll)) return 2;

    if (g_use_md) {
        /* Manual dispatch must be armed BEFORE Init. */
        p_MD_Init();
        tr("SteamAPI_ManualDispatch_Init() done");
    } else {
        tr("classic pump (SteamAPI_RegisterCallback + RunCallbacks)");
    }

    int ok = p_Init();
    tr("SteamAPI_Init() = %d", ok);
    if (!ok) {
        fprintf(stderr, "FATAL: SteamAPI_Init failed — is Steam running & logged in, "
                        "and steam_appid.txt present?\n");
        return 3;
    }

    if (!g_use_md) {
        reg_cb(101, 1);                          /* SteamServersConnected_t — pump liveness */
        reg_cb(304, 16);                         /* PersonaStateChange_t   — pump liveness */
        reg_cb(CBID_UserStatsReceived, 24);
        reg_cb(CBID_UserStatsStored, 16);
        reg_cb(CBID_UserAchievementStored, 152);
    }

    g_pipe = p_GetHSteamPipe();
    HSteamUser huser = p_GetHSteamUser();
    tr("HSteamPipe=%d HSteamUser=%d", g_pipe, huser);

    /* Titles do not all ask for the same ISteamUser: Among Us asks for 021,
     * AoE IV for 023. Which one is asked for is therefore a VARIABLE when
     * chasing a per-title bug, not a constant — though note the flat API used
     * below dispatches at the slot THIS redistributable was built against, so
     * asking for a version it does not know calls the wrong native method. */
    const char *user_iface = getenv("HARNESS_USER_IFACE");
    if (!user_iface || !*user_iface) user_iface = "SteamUser021";
    g_user  = p_FindOrCreateUserInterface(huser, user_iface);
    g_stats = p_FindOrCreateUserInterface(huser, "STEAMUSERSTATS_INTERFACE_VERSION012");
    tr("ISteamUser(%s)=%p ISteamUserStats(v012)=%p", user_iface, g_user, g_stats);
    if (!g_user || !g_stats) { fprintf(stderr, "FATAL: interface lookup failed\n"); return 3; }

    int logged = p_User_BLoggedOn(g_user);
    uint64_t sid = p_User_GetSteamID(g_user);
    tr("BLoggedOn=%d SteamID=%llu", logged, (unsigned long long)sid);
    if (!logged) {
        /* Map trap #1: offline Steam is a convincing false negative. */
        fprintf(stderr, "ABORT: BLoggedOn=false — Steam is OFFLINE. Results would be "
                        "meaningless (see map trap #1). Go online and re-run.\n");
        return 4;
    }

    /* The overlay slots have nothing to do with stats; do not make an overlay
     * run depend on a stats callback arriving. */
    if (strcmp(mode, "overlay") != 0 && strcmp(mode, "ticket") != 0) {
        tr("RequestCurrentStats() = %d", p_Stats_RequestCurrentStats(g_stats));
        if (!pump_until(CBID_UserStatsReceived, 10000)) return 5;
    }

    int rc = 0;
    if (strcmp(mode, "stats") == 0) {
        rc = mode_stats();

    } else if (strcmp(mode, "overlay") == 0) {
        rc = mode_overlay(sid, ach_or_which);

    } else if (strcmp(mode, "ticket") == 0) {
        rc = mode_ticket();

    } else if (strcmp(mode, "status") == 0) {
        print_all_achievements();

    } else if (strcmp(mode, "set") == 0) {
        tr("SetAchievement(\"%s\") = %d", ach, p_Stats_SetAchievement(g_stats, ach));
        tr("StoreStats() = %d", p_Stats_StoreStats(g_stats));
        pump_until(CBID_UserStatsStored, 10000);
        print_achievement(ach);

    } else if (strcmp(mode, "reset") == 0) {
        tr("ResetAllStats(true) = %d", p_Stats_ResetAllStats(g_stats, 1));
        tr("RequestCurrentStats() = %d", p_Stats_RequestCurrentStats(g_stats));
        pump_until(CBID_UserStatsReceived, 10000);
        print_all_achievements();

    } else { /* loop: the idempotent set -> verify -> reset -> verify cycle */
        tr("--- phase 0: initial state ---");
        print_all_achievements();

        tr("--- phase 1: set + store ---");
        tr("SetAchievement(\"%s\") = %d", ach, p_Stats_SetAchievement(g_stats, ach));
        tr("StoreStats() = %d", p_Stats_StoreStats(g_stats));
        if (!pump_until(CBID_UserStatsStored, 10000)) rc = 6;

        tr("--- phase 2: verify set ---");
        print_achievement(ach);
        int set_ok = get_achieved(ach) == 1;
        tr("VERIFY set: %s", set_ok ? "PASS" : "FAIL");

        tr("--- phase 3: reset all ---");
        tr("ResetAllStats(true) = %d", p_Stats_ResetAllStats(g_stats, 1));
        tr("RequestCurrentStats() = %d", p_Stats_RequestCurrentStats(g_stats));
        if (!pump_until(CBID_UserStatsReceived, 10000)) rc = 6;

        tr("--- phase 4: verify reset ---");
        print_achievement(ach);
        int reset_ok = get_achieved(ach) == 0;
        tr("VERIFY reset: %s", reset_ok ? "PASS" : "FAIL");

        tr("=== LOOP %s ===", set_ok && reset_ok && rc == 0 ? "PASS" : "FAIL");
        if (!(set_ok && reset_ok)) rc = 7;
    }

    p_Shutdown();
    tr("SteamAPI_Shutdown() done");
    return rc;
}
