// (a2) spike: dlopen Valve's renderer LATE, then recover its dyld interposes by
// rebinding GOT slots ourselves. Drives its own run loop so the CFRunLoop call
// sites live in THIS binary (representative of ntdll.so / winemetal.so in the
// bottle) rather than inside AppKit's shared-cache code, which cannot be rebound.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include "fishhook.h"

// The 15 symbols in the renderer's __DATA,__interpose section.
static const char *kInterposed[] = {
    "CGLChoosePixelFormat", "CGLFlushDrawable", "glSwapAPPLE",
    "HideCursor", "InitCursor", "ShowCursor",
    "CFRunLoopRun", "CFRunLoopRunInMode",
    "CGAssociateMouseAndMouseCursorPosition", "CGCursorIsVisible",
    "CGDisplayHideCursor", "CGDisplayShowCursor",
    "CGDisplayMoveCursorToPoint", "CGGetLastMouseDelta",
    "CGWarpMouseCursorPosition",
};
static const int kNInterposed = sizeof(kInterposed) / sizeof(kInterposed[0]);

struct interpose_tuple { void *replacement; void *original; };

// Find the loaded renderer image and walk its interpose table, matching each
// entry's `original` (bound by dyld at load) against a known symbol address.
static int recover_interposes(const char *needle) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm || !strstr(nm, needle)) continue;
        const struct mach_header_64 *h =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        unsigned long size = 0;
        uint8_t *sect = getsectiondata(h, "__DATA", "__interpose", &size);
        if (!sect) { fprintf(stderr, "no __interpose section\n"); return 0; }
        struct interpose_tuple *t = (struct interpose_tuple *)sect;
        int n = (int)(size / sizeof(struct interpose_tuple));
        fprintf(stderr, "__interpose: %d entries\n", n);

        struct rebinding binds[32]; int nb = 0;
        for (int e = 0; e < n && nb < 32; e++) {
            for (int s = 0; s < kNInterposed; s++) {
                // CFRunLoopRun/InMode replacements stall a tight self-driven
                // pump; skip them when asked so the rest can be measured.
                if (getenv("SKIP_RUNLOOP") &&
                    strncmp(kInterposed[s], "CFRunLoop", 9) == 0) continue;
                void *real = dlsym(RTLD_DEFAULT, kInterposed[s]);
                if (real && real == t[e].original) {
                    binds[nb].name = kInterposed[s];
                    binds[nb].replacement = t[e].replacement;
                    binds[nb].replaced = NULL;
                    fprintf(stderr, "  [%02d] %-42s -> %p\n",
                            e, kInterposed[s], t[e].replacement);
                    nb++;
                    break;
                }
            }
        }
        fprintf(stderr, "identified %d/%d, rebinding...\n", nb, n);
        if (nb) rebind_symbols(binds, nb);
        return nb;
    }
    fprintf(stderr, "renderer image not found\n");
    return 0;
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 640, 480)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered defer:NO];
        [w setTitle:@"metalprobe3 (dlopen + rebind)"];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.frame = NSMakeRect(0, 0, 640, 480);
        [w.contentView setWantsLayer:YES];
        [w.contentView setLayer:layer];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp finishLaunching];

        const char *rp = getenv("OVERLAY_DLOPEN");
        if (rp) {
            void *h = dlopen(rp, RTLD_NOW | RTLD_LOCAL);
            fprintf(stderr, "dlopen(renderer) -> %p %s\n", h, h ? "" : dlerror());
            if (h && !getenv("NO_REBIND")) recover_interposes("gameoverlayrenderer");
            else fprintf(stderr, "rebinding SKIPPED (NO_REBIND)\n");
        }

        int frames = 0;
        while (frames < 60 * 60 * 5) {
            @autoreleasepool {
                // Call site in OUR binary — rebindable, unlike AppKit's internal one.
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, true);
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES]))
                    [NSApp sendEvent:ev];

                id<CAMetalDrawable> d = [layer nextDrawable];
                if (!d) continue;
                MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd.colorAttachments[0].texture = d.texture;
                rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.35, 0.2, 1.0);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rpd];
                [e endEncoding];
                [cb presentDrawable:d];
                [cb commit];
                if (++frames % 300 == 0) fprintf(stderr, "frames=%d\n", frames);
            }
        }
    }
    return 0;
}
