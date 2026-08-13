/* bridge_abi.h — the contract shared by the PE half and the unix half of the
 * seam spike (#10 stage 2).
 *
 * Both halves are compiled x86_64 (mingw PE, clang++ Mach-O .so) so the System V
 * / MS x86_64 struct-layout rules agree field for field. Everything here is
 * fixed-width and laid out widest-first, so there is no packing question to get
 * wrong across the seam.
 *
 * The call code passed to __wine_unix_call indexes __wine_unix_call_funcs[], so
 * these enum values MUST match the order of that array in bridge_unix.cpp.
 */
#pragma once

#include <stdint.h>

enum bridge_call
{
    BRIDGE_CALL_NOOP     = 0, /* echo a magic; the seam-RTT baseline (no Steam) */
    BRIDGE_CALL_INIT     = 1, /* dlopen steamclient.dylib, connect, read once   */
    BRIDGE_CALL_PERSONA  = 2, /* re-read the persona name (a real-getter RTT)   */
    BRIDGE_CALL_SHUTDOWN = 3, /* release the user + pipe                        */
};

/* BRIDGE_CALL_NOOP: proves data crosses intact with zero native work, so the
 * measured time is the bare __wine_unix_call round trip. */
struct noop_params
{
    uint64_t magic_in;
    uint64_t magic_out;   /* = magic_in ^ BRIDGE_NOOP_XOR */
};

#define BRIDGE_NOOP_XOR 0xC0FFEE0DDBA11ULL

/* BRIDGE_CALL_INIT: the whole go/no-go payload for stage 2. Filled entirely on
 * the unix side; the PE half only forwards the pointer. `step` is 0 on success
 * or the negative index of the stage that failed, with `err` carrying detail. */
struct steam_init_result
{
    uint64_t steam_id;               /* ISteamUser::GetSteamID() — the crossed value */
    int32_t  dlopen_ok;
    int32_t  create_interface_ok;
    int32_t  pipe;
    int32_t  user;
    int32_t  b_connected;            /* Steam_BConnected — the HONEST online oracle */
    int32_t  b_logged_on;            /* ISteamUser::BLoggedOn() */
    int32_t  persona_ok;
    int32_t  unix_pid;               /* native getpid() — proves we ran unix-side */
    int32_t  step;                   /* 0 = ok; <0 = failing stage */
    char     persona_name[128];      /* ISteamFriends::GetPersonaName() */
    char     client_version[32];     /* which SteamClientNNN answered */
    char     unix_sysname[32];       /* uname().sysname — "Darwin" */
    char     err[192];               /* empty on success */
};

/* BRIDGE_CALL_PERSONA: cheapest real Steamworks getter, timed in a loop so the
 * per-call cost of an actual interface call across the seam can be reported. */
struct persona_params
{
    int32_t ok;
    char    persona_name[128];
};
