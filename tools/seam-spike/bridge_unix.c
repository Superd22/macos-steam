/* bridge_unix.c — the unix half of the seam spike (#8 stage 1).
 *
 * Built as a thin x86_64 Mach-O dylib named bridgetest.so, placed in
 * <dir>/x86_64-unix/. Wine's ntdll.so dlopen()s it when the matching PE
 * builtin bridgetest.dll is loaded, and resolves the two exported arrays
 * below. That contract — two symbol arrays of NTSTATUS (*)(void *) — is
 * the entire unixlib ABI (docs/research/crossover-bridge-surface.md §3).
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/utsname.h>

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)( void *args );

/* Must match struct ping_params in bridge_pe.c field for field.
 * Everything is UINT64/char so there is no packing question. */
struct ping_params
{
    uint64_t magic_in;
    uint64_t magic_out;
    uint64_t unix_pid;
    char     unix_sysname[64];
};

static NTSTATUS bridge_ping( void *args )
{
    struct ping_params *p = args;
    struct utsname u;

    p->magic_out = p->magic_in ^ 0xC0FFEE0DDBA11ULL;
    p->unix_pid  = (uint64_t)getpid();
    if (!uname( &u ))
    {
        strncpy( p->unix_sysname, u.sysname, sizeof(p->unix_sysname) - 1 );
        p->unix_sysname[sizeof(p->unix_sysname) - 1] = 0;
    }
    return 0;
}

/* Optional init hook — ntdll.so calls it if present; nonzero aborts the load. */
NTSTATUS __wine_unix_lib_init( void )
{
    return 0;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    bridge_ping,
};

/* Same order; only consulted from a wow64 process, which we never are. */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    bridge_ping,
};
