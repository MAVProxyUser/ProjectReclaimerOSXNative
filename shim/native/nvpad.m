// A virtual game controller inside the app (2026-11-08; the user: "make 3-4 walkers have a battle ... in a rumble pit match").
// The walker's moves on a JOINED client never reached the server: the app builds its movement / button packets from its own input
// layer (the control record nbot.c writes carries the aim, which does reach the server, but not the throttle). The app reads game
// controllers through the GameController framework (objc2-game-controller: +[GCController controllers], -extendedGamepad and its
// elements' -value / -isPressed, polled), so nhook adds one more controller to that list: Apple's own programmable one,
// +[GCController controllerWithExtendedGamepad], whose sticks and buttons nbot.c sets (nh_vpad_set) -- the app then sends those
// moves like a player's. Test identities only (nbot.c bot_allowed); NHOOK_VPAD=1 turns it on (shim/tools/walker-battle.sh does).
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
void nh_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // nhook.c
static GCController* g_vpad; static IMP g_orig_controllers; static volatile int g_vpad_reads, g_vpad_stick_reads; static IMP g_orig_lstick;
static id my_lstick(id self, SEL cmd) { g_vpad_stick_reads++; return ((id (*)(id, SEL))g_orig_lstick)(self, cmd); }   // (counts the app's reads)
static NSArray* my_controllers(id self, SEL cmd) {
    NSArray* a = ((NSArray* (*)(id, SEL))g_orig_controllers)(self, cmd); g_vpad_reads++;
    return g_vpad ? (a ? [a arrayByAddingObject:g_vpad] : @[g_vpad]) : a; }
int nh_vpad_install(void) {   // 1 = the virtual controller is in +[GCController controllers]
    if (g_vpad) return 1;
    @autoreleasepool {
        if (![GCController respondsToSelector:@selector(controllerWithExtendedGamepad)]) { nh_log("[nhook] [vpad] this macOS has no programmable controller"); return 0; }
        GCController* c = [GCController controllerWithExtendedGamepad]; if (!c || !c.extendedGamepad) { nh_log("[nhook] [vpad] no programmable controller"); return 0; }
        Method m = class_getClassMethod([GCController class], @selector(controllers)); if (!m) { nh_log("[nhook] [vpad] +[GCController controllers] not found"); return 0; }
        g_vpad = [c retain]; g_orig_controllers = method_setImplementation(m, (IMP)my_controllers);
        { Method ls = class_getInstanceMethod(object_getClass(c.extendedGamepad), @selector(leftThumbstick)); if (ls) g_orig_lstick = method_setImplementation(ls, (IMP)my_lstick);
          nh_log("[nhook] [vpad] controller class %s, gamepad class %s, attached %d", class_getName(object_getClass(c)), class_getName(object_getClass(c.extendedGamepad)), (int)c.isAttachedToDevice); }
        nh_log("[nhook] [vpad] a virtual controller is in +[GCController controllers] (the walker's sticks and buttons)"); }
    return 1; }
// sticks -1..1 (y up = forward), triggers 0..1, buttons: bit 0 A, 1 B, 2 X, 3 Y, 4 left shoulder, 5 right shoulder, 6 left stick
// click, 7 right stick click, 8 menu, 9 options, 10-13 dpad up / down / left / right
void nh_vpad_set(float lx, float ly, float rx, float ry, float lt, float rt, uint32_t b) {
    if (!g_vpad) return;
    @autoreleasepool { GCExtendedGamepad* g = g_vpad.extendedGamepad; if (!g) return;
        [g.leftThumbstick setValueForXAxis:lx yAxis:ly]; [g.rightThumbstick setValueForXAxis:rx yAxis:ry];
        [g.leftTrigger setValue:lt]; [g.rightTrigger setValue:rt];
        [g.buttonA setValue:(b & 1) ? 1 : 0]; [g.buttonB setValue:(b & 2) ? 1 : 0]; [g.buttonX setValue:(b & 4) ? 1 : 0]; [g.buttonY setValue:(b & 8) ? 1 : 0];
        [g.leftShoulder setValue:(b & 16) ? 1 : 0]; [g.rightShoulder setValue:(b & 32) ? 1 : 0];
        if (g.leftThumbstickButton) [g.leftThumbstickButton setValue:(b & 64) ? 1 : 0];
        if (g.rightThumbstickButton) [g.rightThumbstickButton setValue:(b & 128) ? 1 : 0];
        [g.buttonMenu setValue:(b & 256) ? 1 : 0]; if (g.buttonOptions) [g.buttonOptions setValue:(b & 512) ? 1 : 0];
        [g.dpad setValueForXAxis:((b & 8192) ? 1 : 0) - ((b & 4096) ? 1 : 0) yAxis:((b & 1024) ? 1 : 0) - ((b & 2048) ? 1 : 0)]; } }
int nh_vpad_reads(void) { return g_vpad_reads; }
int nh_vpad_stick_reads(void) { return g_vpad_stick_reads; }

// ---- taking over the observer's game (2026-11-08; the user: "can you make it so that IF I choose to pickup the controls while you
// sample public games I can play? it seems to only capture the mouse and not the joystick right now"). The observer runs the app
// with --background (a 640 x 360 window below the others that never takes the focus; the real profile's borderless full screen
// otherwise), so the app is never the frontmost app -- and the GameController framework hands a real controller's input only to
// the frontmost app: a click captured the mouse, the controller did nothing. NHOOK_TAKEOVER=1 (shim/tools/observe-public.sh): the
// first click in the app's window turns on +[GCController shouldMonitorBackgroundEvents] (the controller reaches the app whatever is
// in front), activates the app, puts the window at the normal level, makes it 1600 x 900 (centered) when smaller, and tells nhook.c
// (nh_takeover_fire: sound back on, the observer told through NHOOK_TAKEOVER_FILE).
#import <AppKit/AppKit.h>
void nh_takeover_fire(void);   // nhook.c
static id g_tk_monitor;
// THE BRIDGE (2026-11-08, same day: the click worked -- window raised, background events on -- and the controller still did
// nothing): the app enumerates +[GCController controllers] once, early, and a --background app finds no physical controller
// then, so it never reads one. It does read nhook's virtual pad (the walkers' moves work in --background clients), so with
// NHOOK_TAKEOVER the virtual pad is installed at start (nhook.c) and, after the click, every physical controller's changes are
// copied into it (valueChangedHandler on a serial queue -> nh_vpad_set): sticks, triggers, A B X Y, bumpers, stick clicks, menu /
// options, the d-pad. Before the click the virtual pad stays at rest.
static NSMutableSet* g_tk_bridged;
static void tk_copy(GCExtendedGamepad* p) {
    uint32_t b = (p.buttonA.isPressed ? 1 : 0) | (p.buttonB.isPressed ? 2 : 0) | (p.buttonX.isPressed ? 4 : 0) | (p.buttonY.isPressed ? 8 : 0)
               | (p.leftShoulder.isPressed ? 16 : 0) | (p.rightShoulder.isPressed ? 32 : 0) | (p.leftThumbstickButton.isPressed ? 64 : 0) | (p.rightThumbstickButton.isPressed ? 128 : 0)
               | (p.buttonMenu.isPressed ? 256 : 0) | (p.buttonOptions.isPressed ? 512 : 0)
               | (p.dpad.up.isPressed ? 1024 : 0) | (p.dpad.down.isPressed ? 2048 : 0) | (p.dpad.left.isPressed ? 4096 : 0) | (p.dpad.right.isPressed ? 8192 : 0);
    nh_vpad_set(p.leftThumbstick.xAxis.value, p.leftThumbstick.yAxis.value, p.rightThumbstick.xAxis.value, p.rightThumbstick.yAxis.value, p.leftTrigger.value, p.rightTrigger.value, b); }
static void tk_bridge(GCController* c) {
    if (!c || c == g_vpad || !c.extendedGamepad || [g_tk_bridged containsObject:c]) return;
    static dispatch_queue_t q; if (!q) q = dispatch_queue_create("nhook.pad-bridge", DISPATCH_QUEUE_SERIAL);
    [g_tk_bridged addObject:c]; c.handlerQueue = q;
    c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* gp, GCControllerElement* el) { (void)el; tk_copy(gp); };
    nh_log("[nhook] [takeover] your controller (%s) drives the game through the virtual pad", c.vendorName ? c.vendorName.UTF8String : "?"); }
static void tk_bridge_all(void) {
    NSArray* a = g_orig_controllers ? ((NSArray* (*)(id, SEL))g_orig_controllers)([GCController class], @selector(controllers)) : [GCController controllers];
    int n = 0; for (GCController* c in a) if (c != g_vpad) { tk_bridge(c); n++; }
    static int said; if (!n && !said) { said = 1; nh_log("[nhook] [takeover] no physical controller seen yet (press a button on it; checked every 2 s)"); } }
static void tk_take(NSWindow* w) {
    if (@available(macOS 11.3, *)) [GCController setShouldMonitorBackgroundEvents:YES];
    [NSApp activateIgnoringOtherApps:YES];
    if (w) {
        // full resolution (the user: "have full resolution available"): the window fills the screen's visible area at the normal
        // level, resizable, and native full screen allowed (the green button / Ctrl-Cmd-F); the game renders at the window's size
        [w setLevel:NSNormalWindowLevel];
        w.styleMask = w.styleMask | NSWindowStyleMaskResizable | NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskClosable;
        w.collectionBehavior = (w.collectionBehavior & ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone)) | NSWindowCollectionBehaviorFullScreenPrimary;
        NSRect v = (w.screen ?: [NSScreen mainScreen]).visibleFrame; [w setFrame:v display:YES animate:NO];
        [w makeKeyAndOrderFront:nil]; }
    nh_log("[nhook] [takeover] the user has the controls: controller input in the background on (%s), window %s", [GCController respondsToSelector:@selector(setShouldMonitorBackgroundEvents:)] ? "yes" : "this macOS cannot",
           w ? "raised" : "not found");
    if (!g_vpad) nh_log("[nhook] [takeover] no virtual pad (it is installed at start with NHOOK_TAKEOVER): the controller cannot be bridged");
    else {
        if (!g_tk_bridged) g_tk_bridged = [[NSMutableSet alloc] init];
        [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification* n) { tk_bridge(n.object); }];
        tk_bridge_all();
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());   // (a controller connected or found later)
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
        dispatch_source_set_event_handler(t, ^{ tk_bridge_all(); }); dispatch_resume(t); }
    nh_takeover_fire(); }
void nh_takeover_now(void) {   // (stats control do=takeover: the same as a click in the window, from the stats page / a test)
    dispatch_async(dispatch_get_main_queue(), ^{ NSWindow* w = nil; for (NSWindow* x in NSApp.windows) if (x.isVisible && (!w || x.frame.size.width * x.frame.size.height > w.frame.size.width * w.frame.size.height)) w = x;
        static int done; if (!done) { done = 1; tk_take(w); } }); }
void nh_takeover_install(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (g_tk_monitor || !NSApp) { if (!NSApp) nh_takeover_install(); return; }
        g_tk_monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown) handler:^NSEvent* (NSEvent* e) {
            static int done; if (!done && e.window) { done = 1; tk_take(e.window); }   // (a do=takeover first makes this a no-op: tk_take runs once)
            return e; }] retain];
        nh_log("[nhook] [takeover] armed: a click in the game's window gives you the controls (the controller too)"); }); }
