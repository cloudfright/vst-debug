// host_window.mm
// Minimal NSWindow + NSView host for VST3 IPlugView::attached().
// Compiled as Objective-C++ and linked with vst3_debugger.cpp.
#import <AppKit/AppKit.h>

static NSWindow* g_window = nullptr;
static NSView*   g_view   = nullptr;

extern "C" void* hostview_create(int w, int h)
{
    if (!NSApp) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }
    NSRect frame = NSMakeRect(100, 100, w, h);
    g_window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled |
                   NSWindowStyleMaskClosable |
                   NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered
        defer:NO];
    g_view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    [g_window setContentView:g_view];
    [g_window setTitle:@"VST3 Debugger – Plugin View"];
    return (__bridge void*)g_view;
}

extern "C" void hostview_show_and_run(double seconds)
{
    [g_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    [[NSRunLoop currentRunLoop] runUntilDate:until];
}

extern "C" void hostview_destroy()
{
    if (g_window) {
        [g_window close];
        g_window = nullptr;
        g_view   = nullptr;
    }
}
