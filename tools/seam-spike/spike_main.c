/* spike_main.c — Windows console exe that loads bridgetest.dll and pings
 * across the seam. Run inside the throwaway bottle. Prints PASS/FAIL lines
 * that the driving script greps.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

struct ping_params
{
    UINT64 magic_in;
    UINT64 magic_out;
    UINT64 unix_pid;
    char   unix_sysname[64];
};

typedef int (*pBridgePing)( UINT64, struct ping_params *, LONG * );

int main( int argc, char **argv )
{
    const UINT64 magic = 0x5EA151DE5EA151DEULL;
    struct ping_params p;
    LONG status = 0;
    HMODULE dll;
    pBridgePing ping;
    int rc;

    /* Bare name on purpose: this exercises Wine's builtin search through
     * WINEDLLPATH, which is the mechanism under test. */
    dll = LoadLibraryA( "bridgetest.dll" );
    if (!dll)
    {
        printf( "SPIKE FAIL: LoadLibrary(bridgetest.dll) error %lu\n", GetLastError() );
        return 1;
    }
    printf( "SPIKE OK: builtin loaded at %p\n", (void *)dll );

    ping = (pBridgePing)GetProcAddress( dll, "bridge_ping" );
    if (!ping)
    {
        printf( "SPIKE FAIL: bridge_ping export missing\n" );
        return 1;
    }

    rc = ping( magic, &p, &status );
    if (rc)
    {
        printf( "SPIKE FAIL: bridge_ping step %d, ntstatus 0x%08lx\n", rc, (unsigned long)status );
        return 1;
    }

    printf( "SPIKE OK: unix call returned\n" );
    printf( "  magic_out    = %016llx (expect %016llx)\n",
            (unsigned long long)p.magic_out,
            (unsigned long long)(magic ^ 0xC0FFEE0DDBA11ULL) );
    printf( "  unix_pid     = %llu (win pid %lu)\n",
            (unsigned long long)p.unix_pid, GetCurrentProcessId() );
    printf( "  unix_sysname = %s\n", p.unix_sysname );

    if (p.magic_out == (magic ^ 0xC0FFEE0DDBA11ULL))
        printf( "SPIKE PASS\n" );
    else
        printf( "SPIKE FAIL: magic mismatch\n" );
    return 0;
}
