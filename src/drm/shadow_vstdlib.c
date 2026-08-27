/* The DRM route's second shadow (ADR 0014). Inert on purpose.
 *
 * Valve's steamclient64.dll imports two libraries of its own, and the other one
 * is where we install the trampolines. This one exists only because it cannot
 * be left real: Valve's own vstdlib initialises against Valve's own tier0, and
 * with tier0 shadowed it dies during load — measured, before the wrapper ever
 * reaches CreateInterface.
 *
 * Nothing here is ever called. Once the trampolines are in and steamclient64's
 * entry point is neutered, none of Valve's code in the process runs at all.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void *reserved)
{
    (void)self; (void)reason; (void)reserved;
    return TRUE;
}
