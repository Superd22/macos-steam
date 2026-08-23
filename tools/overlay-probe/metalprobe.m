// Minimal Metal presenter: real nextDrawable -> presentDrawable: -> commit cycles,
// in a real NSWindow, so Valve's renderer sees the exact call pattern it hooks.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

static id<MTLDevice> gDev; static CAMetalLayer *gLayer; static id<MTLCommandQueue> gQ;
static int gFrames = 0;

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
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 640, 480)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered defer:NO];
        [w setTitle:@"metalprobe"];
        gDev = MTLCreateSystemDefaultDevice();
        gLayer = [CAMetalLayer layer];
        gLayer.device = gDev;
        gLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        gLayer.framebufferOnly = YES;
        gLayer.frame = NSMakeRect(0, 0, 640, 480);
        [w.contentView setWantsLayer:YES];
        [w.contentView setLayer:gLayer];
        gQ = [gDev newCommandQueue];
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        P *p = [[P alloc] init];
        [NSTimer scheduledTimerWithTimeInterval:1.0/60.0 target:p
                 selector:@selector(tick:) userInfo:nil repeats:YES];
        [NSApp run];
    }
    return 0;
}
