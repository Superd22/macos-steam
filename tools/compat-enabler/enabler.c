// compat-enabler — flips CCompatManager::m_bCompatEnabled in a live Steam client.
//
// Steam decides once, in CCompatManager's constructor, whether the compat subsystem
// runs at all:
//
//     m_bCompatEnabled = ( V_stricmp( host_oslist, "linux" ) == 0 );
//
// On macOS that is always false, which switches off compat-tool mapping, launch
// routing and the per-app platform override (compat.vdf). See
// docs/research/compat-vdf-platform-override.md.
//
// We patch the two-instruction tail of that test inside steamclient.dylib as it
// loads, so no file on disk is ever modified:
//
//     cset wN, eq              ->   mov wN, #1
//     strb wN, [xM, #0x7b0]         (anchor, left alone)
//
// The (cset, strb #0x7b0) pair is located by pattern scan, not by offset, so the
// patch survives Steam client updates.
//
// Build:  see build.sh          Load:  DYLD_INSERT_LIBRARIES=<this>  steam_osx

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <libgen.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <libkern/OSCacheControl.h>

#define TARGET_IMAGE  "steamclient.dylib"
#define HOST_PROCESS  "steam_osx"
#define COMPAT_OFF    0x7b0          // offsetof(CCompatManager, m_bCompatEnabled)

static void logf_(const char *fmt, ...) {
    const char *p = getenv("COMPAT_ENABLER_LOG");
    FILE *f = fopen(p ? p : "/tmp/compat-enabler.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    fprintf(f, "[compat-enabler] ");
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    va_end(ap);
    fclose(f);
}

// DYLD_INSERT_LIBRARIES is inherited by children, and Steam launches games as
// children. Only act inside the Steam client itself.
static int in_steam_client(void) {
    char path[4096]; uint32_t n = sizeof(path);
    if (_NSGetExecutablePath(path, &n) != 0) return 0;
    char buf[4096]; strncpy(buf, path, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    return strcmp(basename(buf), HOST_PROCESS) == 0;
}

// Make one instruction writable, replace it, restore RX, flush icache.
static int write_insn(uint32_t *addr, uint32_t insn) {
    vm_address_t page = (vm_address_t)addr & ~(vm_address_t)(PAGE_SIZE - 1);
    vm_size_t    len  = PAGE_SIZE * 2;              // in case we straddle
    // VM_PROT_COPY forces a private copy of the shared, read-only TEXT page.
    kern_return_t kr = vm_protect(mach_task_self(), page, len, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) { logf_("vm_protect(rw) failed: %d", kr); return 0; }

    *addr = insn;

    kr = vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) logf_("vm_protect(rx) failed: %d (continuing)", kr);
    sys_icache_invalidate(addr, sizeof(*addr));
    return 1;
}

// Find   cset wN, eq   immediately followed by   strb wN, [xM, #0x7b0]
// and rewrite the cset to   mov wN, #1.
static int patch_gate(const struct mach_header_64 *mh) {
    unsigned long size = 0;
    uint8_t *sect = getsectiondata(mh, "__TEXT", "__text", &size);
    if (!sect || size < 8) { logf_("no __text section"); return 0; }

    uint32_t *w = (uint32_t *)sect;
    size_t     n = size / 4;
    int hits = 0;

    for (size_t i = 0; i + 1 < n; i++) {
        // cset wN, eq  ==  csinc wN, wzr, wzr, ne  ==  0x1A9F17E0 | N
        if ((w[i] & 0xFFFFFFE0u) != 0x1A9F17E0u) continue;
        uint32_t rt = w[i] & 0x1Fu;

        // strb wRt, [xM, #0x7b0]  (unsigned offset, scale 1)
        uint32_t s = w[i + 1];
        if ((s & 0xFFC00000u) != 0x39000000u) continue;
        if (((s >> 10) & 0xFFFu) != COMPAT_OFF) continue;
        if ((s & 0x1Fu) != rt) continue;

        uint32_t mov = 0x52800020u | rt;            // movz wRt, #1
        logf_("gate found at %p (cset w%u -> mov w%u, #1)", (void *)&w[i], rt, rt);
        if (write_insn(&w[i], mov)) hits++;
    }

    logf_("patched %d site(s)", hits);
    return hits;
}

static void on_image(const struct mach_header *mh, intptr_t slide) {
    (void)slide;
    Dl_info info;
    if (!dladdr((const void *)mh, &info) || !info.dli_fname) return;
    if (!strstr(info.dli_fname, TARGET_IMAGE)) return;
    logf_("target image loaded: %s", info.dli_fname);
    patch_gate((const struct mach_header_64 *)mh);
}

__attribute__((constructor))
static void init(void) {
    if (!in_steam_client()) return;              // silent no-op in games/helpers
    logf_("attached to %s (pid %d)", HOST_PROCESS, getpid());
    // Fires for images already loaded, and for each one loaded later — including
    // the dlopen of steamclient.dylib, before CCompatManager is constructed.
    _dyld_register_func_for_add_image(on_image);
}
