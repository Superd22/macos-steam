// Mach bootstrap diagnostic for macos-steam issue #2.
//
// steamclient.dylib imports bootstrap_look_up and carries three service names:
// com.valvesoftware.steam, .ipctool and .other. If an unlaunched process cannot
// look those up, that — not code signing — is the mechanism gating us out.

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <cstdio>

int main() {
  const char* names[] = {
      "com.valvesoftware.steam",
      "com.valvesoftware.steam.ipctool",
      "com.valvesoftware.steam.other",
  };

  printf("=== bootstrap_look_up from an unlaunched process ===\n");
  for (const char* n : names) {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port, n, &port);
    printf("  %-34s kr=%d (%s) port=0x%x\n", n, kr,
           kr == KERN_SUCCESS ? "SUCCESS" : bootstrap_strerror(kr), port);
  }
  return 0;
}
