/* spike_main.c — the trivial Windows test exe for the seam spike, stage 2 (#10).
 *
 * This stands in for the game: a plain .exe inside the bottle that loads the
 * builtin bridgetest.dll and drives it. It connects to the native macOS Steam
 * client through the Wine seam, prints the one value that crossed, and reports
 * per-call round-trip latency. Greppable PASS/FAIL lines for run.sh.
 *
 * Reference values are this machine's, established standalone in #2:
 *   SteamID 76561198014230730 for the logged-in account.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#include "bridge_abi.h"

#define REF_STEAM_ID 76561198014230730ULL

typedef int (*pInit)(struct steam_init_result *, LONG *);
typedef int (*pSeamRtt)(UINT64, double *, double *, int *, LONG *);
typedef int (*pPersonaRtt)(UINT64, double *, double *, char *, LONG *);
typedef int (*pShutdown)(LONG *);

int main(void)
{
    HMODULE dll;
    pInit init;
    pSeamRtt seam_rtt;
    pPersonaRtt persona_rtt;
    pShutdown shutdown;
    struct steam_init_result r;
    LONG status = 0;
    double avg = 0, mn = 0;
    char name[128];
    int echo_ok = 0, rc;
    int pass = 1;

    /* Bare name on purpose: exercises Wine's builtin search through WINEDLLPATH,
     * the mechanism the deployment layout depends on. */
    dll = LoadLibraryA("bridgetest.dll");
    if (!dll) {
        printf("SPIKE2 FAIL: LoadLibrary(bridgetest.dll) error %lu\n", GetLastError());
        return 1;
    }
    printf("SPIKE2 OK: builtin loaded at %p\n", (void *)dll);

    init        = (pInit)GetProcAddress(dll, "bridge_steam_init");
    seam_rtt    = (pSeamRtt)GetProcAddress(dll, "bridge_seam_rtt");
    persona_rtt = (pPersonaRtt)GetProcAddress(dll, "bridge_persona_rtt");
    shutdown    = (pShutdown)GetProcAddress(dll, "bridge_steam_shutdown");
    if (!init || !seam_rtt || !persona_rtt || !shutdown) {
        printf("SPIKE2 FAIL: an export is missing\n");
        return 1;
    }

    /* ---- 1. one real Steamworks call across the seam --------------------- */
    rc = init(&r, &status);
    if (rc) {
        printf("SPIKE2 FAIL: bridge_steam_init step %d, ntstatus 0x%08lx\n",
               rc, (unsigned long)status);
        return 1;
    }

    printf("\n--- steamclient.dylib, driven from inside Wine ---\n");
    printf("  unix side       : %s pid %d\n", r.unix_sysname, r.unix_pid);
    printf("  dlopen          : %s\n", r.dlopen_ok ? "ok" : "FAILED");
    printf("  CreateInterface : %s (%s)\n",
           r.create_interface_ok ? "ok" : "FAILED", r.client_version);
    printf("  pipe / user     : %d / %d\n", r.pipe, r.user);
    printf("  Steam_BConnected: %s\n",
           r.b_connected == 1 ? "TRUE (live IPC)" :
           r.b_connected == 0 ? "FALSE" : "unavailable");
    printf("  BLoggedOn       : %d\n", r.b_logged_on);
    printf("  *** SteamID     : %llu ***\n", (unsigned long long)r.steam_id);
    printf("  PersonaName     : %s\n", r.persona_ok ? r.persona_name : "(none)");
    if (r.step != 0)
        printf("  init step %d: %s\n", r.step, r.err);

    /* A correct value has to cross AND the link has to be live. GetSteamID and
     * the persona name survive offline from cache, so BConnected is the gate
     * that separates a working bridge from Steam-in-offline-mode (map trap #1). */
    if (r.step != 0)                     { pass = 0; }
    if (r.steam_id == 0)                 { pass = 0; printf("  ! no SteamID crossed\n"); }
    if (r.b_connected != 1) {
        pass = 0;
        printf("  ! NOT CONNECTED — is Steam running AND ONLINE? Offline mode\n"
               "    returns a cached SteamID/persona and looks like a broken bridge.\n");
    }
    if (r.steam_id && r.steam_id != REF_STEAM_ID)
        printf("  note: SteamID differs from #2's reference %llu (different account?)\n",
               (unsigned long long)REF_STEAM_ID);

    /* ---- 2. seam round-trip latency ------------------------------------- */
    printf("\n--- round-trip latency (measured PE-side, QPC) ---\n");
    rc = seam_rtt(100000, &avg, &mn, &echo_ok, &status);
    if (rc)
        printf("  seam noop RTT   : FAILED step %d ntstatus 0x%08lx\n", rc, (unsigned long)status);
    else
        printf("  seam noop RTT   : avg %.3f us, min %.3f us over 100k calls (echo %s)\n",
               avg / 1000.0, mn / 1000.0, echo_ok ? "ok" : "CORRUPT");
    if (rc || !echo_ok) pass = 0;

    rc = persona_rtt(10000, &avg, &mn, name, &status);
    if (rc)
        printf("  GetPersonaName  : FAILED step %d ntstatus 0x%08lx\n", rc, (unsigned long)status);
    else
        printf("  GetPersonaName  : avg %.3f us, min %.3f us over 10k calls -> \"%s\"\n",
               avg / 1000.0, mn / 1000.0, name);

    shutdown(&status);

    printf("\n%s\n", pass ? "SPIKE2 PASS" : "SPIKE2 FAIL");
    return pass ? 0 : 1;
}
