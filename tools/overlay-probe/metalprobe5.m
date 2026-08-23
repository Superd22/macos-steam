// #21 re-test: does dlopen fail because of *when* it happens?
// metalprobe3 dlopen'd after NSApp, the window and the CAMetalLayer existed.
// This harness is metalprobe.m with the dlopen moved to the earliest point a
// dlopen can happen in our own process — a constructor, before main — which is
// the timing (a2) would really have (our unixlib loads at ntdll init).
//
//   DLOPEN_WHEN=ctor   dlopen from __attribute__((constructor))   [default]
//   DLOPEN_WHEN=main   dlopen at the top of main()
//   DLOPEN_WHEN=nsapp  after [NSApplication sharedApplication]
//   DLOPEN_WHEN=device after MTLCreateSystemDefaultDevice()
//   DLOPEN_WHEN=layer  after the CAMetalLayer is created and attached
//   DLOPEN_WHEN=late   after the window is on screen (metalprobe3's timing)
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>

static id<MTLDevice> gDev; static CAMetalLayer *gLayer; static id<MTLCommandQueue> gQ;
static int gFrames = 0;

static void load_renderer(const char *when) {
    const char *w = getenv("DLOPEN_WHEN"); if (!w) w = "ctor";
    if (strcmp(w, when) != 0) return;
    const char *rp = getenv("OVERLAY_DLOPEN");
    if (!rp) return;
    void *h = dlopen(rp, RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "[%s] dlopen(renderer) -> %p %s\n", when, h, h ? "" : dlerror());
}

__attribute__((constructor)) static void ctor(void) { load_renderer("ctor"); }

@interface P : NSObject @end
@implementation P
- (void)tick:(NSTimer *)t {
    @autoreleasepool {
        id<CAMetalDrawable> d = [gLayer nextDrawable];
        if (!d) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.4, 1.0);
        id<MTLCommandBuffer> cb = [gQ commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        [cb presentDrawable:d];
        [cb commit];
        if (++gFrames % 120 == 0) fprintf(stderr, "frames=%d\n", gFrames);
        if (gFrames > 600) [NSApp terminate:nil];
    }
}
@end

int main(void) {
    @autoreleasepool {
        load_renderer("main");
        [NSApplication sharedApplication];
        load_renderer("nsapp");
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 640, 480)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered defer:NO];
        [w setTitle:@"metalprobe5"];
        gDev = MTLCreateSystemDefaultDevice();
        load_renderer("device");
        gLayer = [CAMetalLayer layer];
        gLayer.device = gDev;
        gLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        gLayer.framebufferOnly = YES;
        gLayer.frame = NSMakeRect(0, 0, 640, 480);
        [w.contentView setWantsLayer:YES];
        [w.contentView setLayer:gLayer];
        load_renderer("layer");
        gQ = [gDev newCommandQueue];
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        load_renderer("late");
        P *p = [[P alloc] init];
        [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 target:p
                 selector:@selector(tick:) userInfo:nil repeats:YES];
        [NSApp run];
    }
    return 0;
}
