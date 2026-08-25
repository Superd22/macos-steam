// dyld interposer for macos-steam issue #2.
//
// The probe gets a pipe and a user out of steamclient.dylib but Steam_BConnected
// is false — and yet com.valvesoftware.steam.ipctool looks up fine from our
// process. So either the dylib never attempts the lookup, or it attempts a name
// that isn't registered. This logs every bootstrap_look_up the dylib makes.
//
// Build as a dylib and DYLD_INSERT_LIBRARIES it into the probe. Only works
// because the probe is unsigned / not hardened.

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <cstdio>
#include <cstdlib>

extern "C" {

static kern_return_t my_bootstrap_look_up(mach_port_t bp, const char* name,
                                          mach_port_t* sp) {
  kern_return_t kr = bootstrap_look_up(bp, name, sp);
  fprintf(stderr, "[interpose] bootstrap_look_up(\"%s\") -> kr=%d port=0x%x\n",
          name, kr, kr == KERN_SUCCESS ? *sp : 0);

  // The dylib asks for com.valvesoftware.steam.other, which nothing registers.
  // The only Valve endpoint that exists is the launchd-managed .ipctool.
  // Redirect and see whether the protocol on the other end is the same one.
  if (kr != KERN_SUCCESS && getenv("PROBE_REDIRECT_IPCTOOL")) {
    char alt[] = "com.valvesoftware.steam.ipctool";
    kr = bootstrap_look_up(bp, alt, sp);
    fprintf(stderr, "[interpose]   -> REDIRECTED to \"%s\" kr=%d port=0x%x\n",
            alt, kr, kr == KERN_SUCCESS ? *sp : 0);
  }
  fflush(stderr);
  return kr;
}

__attribute__((used)) static struct {
  const void* replacement;
  const void* replacee;
} _interpose_blu __attribute__((section("__DATA,__interpose"))) = {
    (const void*)&my_bootstrap_look_up, (const void*)&bootstrap_look_up};

}  // extern "C"
