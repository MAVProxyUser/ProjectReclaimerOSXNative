// pageshot: PNG screenshots of parts of a web page (one per CSS selector) with an offscreen WKWebView -- the
// documentation pictures of the stats page and the forensic archive viewer (shim/tools/pageshot.sh runs it).
//
//   pageshot [options] <url> <outdir> <shot>... [-- [-w px] [-j js] [-T secs] <shot>...]...
//     <shot>      <name>[@<scale>]=<selector>: <outdir>/<name>.png is the box around every element the selector
//                 matches (so "h1, #hd, #panel" takes the three together)
//     -w <px>     viewport width in CSS pixels (default 1280)
//     -h <px>     viewport height while the page loads and the script runs (default 900); before the pictures are
//                 taken the view grows to the whole page, so a section below the fold is captured in one piece
//     -t <secs>   wait after the page has loaded (default 6: the live pages fill in from their streams)
//     -j <js>     body of an async function run in the page before the pictures (it may await); a string it returns
//                 is printed
//     -T <secs>   wait after the script (default 1)
//     -s <scale>  output pixels per CSS pixel (default 2); <name>@1.5=<selector> sets it for one picture
//     -p <px>     margin around each element, in CSS pixels (default 8)
//     -x <dir>    also write <dir>/<name>.txt with the elements' text (innerText), to check what a picture shows
// "--" starts another phase on the SAME page load (its own width, script and pictures): a page whose content depends
// on the order data arrived in (e.g. the stats page's ?demo=1 aliases) stays consistent across all its pictures.
// A selector that matches nothing is reported and skipped (exit status 1).
// The window sits far off-screen, never activates and ignores the mouse: it never takes focus from a running game.
// Build: clang -arch arm64 -fobjc-arc -O1 -o pageshot pageshot.m -framework Cocoa -framework WebKit -framework ImageIO
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <ImageIO/ImageIO.h>

@interface PSNav : NSObject <WKNavigationDelegate>
@property BOOL done; @property NSString *err;
@end
@implementation PSNav
- (void)webView:(WKWebView *)w didFinishNavigation:(WKNavigation *)n { self.done = YES; }
- (void)webView:(WKWebView *)w didFailNavigation:(WKNavigation *)n withError:(NSError *)e { self.err = e.localizedDescription; self.done = YES; }
- (void)webView:(WKWebView *)w didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e { self.err = e.localizedDescription; self.done = YES; }
@end

@interface PSPhase : NSObject
@property double width, wait; @property NSString *script; @property NSMutableArray *names; @property NSMutableDictionary *sels, *scales;
@end
@implementation PSPhase
@end

static void spin(double secs, BOOL (^until)(void)) {   // run the app's event loop for up to secs, or until until() is true
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:secs];
    while (end.timeIntervalSinceNow > 0 && !(until && until())) {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode dequeue:YES];
        if (e) [NSApp sendEvent:e];
    }
}

static id runJS(WKWebView *wv, NSString *body, NSDictionary *args, double timeout) {
    __block id res = nil; __block BOOL fin = NO; __block NSString *err = nil;
    [wv callAsyncJavaScript:body arguments:args inFrame:nil inContentWorld:WKContentWorld.pageWorld
          completionHandler:^(id r, NSError *e) { res = r; if (e) err = e.localizedDescription; fin = YES; }];
    spin(timeout, ^{ return fin; });
    if (err) fprintf(stderr, "pageshot: script error: %s\n", err.UTF8String);
    else if (!fin) fprintf(stderr, "pageshot: script timed out after %.0f s\n", timeout);
    return res;
}

static BOOL writePNG(NSImage *img, NSInteger pw, NSInteger ph, NSString *path) {   // RGB PNG of exactly pw x ph pixels
    CGImageRef src = [img CGImageForProposedRect:NULL context:nil hints:nil];
    if (!src) return NO;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(cs);
    if (!ctx) return NO;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, pw, ph), src);
    CGImageRef out = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
    BOOL ok = NO;
    if (d) { CGImageDestinationAddImage(d, out, NULL); ok = CGImageDestinationFinalize(d); CFRelease(d); }
    CGImageRelease(out);
    return ok;
}

static void usage(void) {
    fprintf(stderr, "usage: pageshot [-w px] [-h px] [-t secs] [-j js] [-T secs] [-s scale] [-p px] [-x textdir] <url> <outdir>"
                    " <name>[@scale]=<selector>... [-- [-w px] [-j js] [-T secs] <name>=<selector>...]...\n");
    exit(2);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        double H = 900, wait = 6, scale = 2, pad = 8; NSString *textdir = nil;
        NSMutableArray<PSPhase *> *phases = [NSMutableArray array];
        PSPhase *ph = [PSPhase new]; ph.width = 1280; ph.wait = 1; ph.names = [NSMutableArray array]; ph.sels = [NSMutableDictionary dictionary]; ph.scales = [NSMutableDictionary dictionary];
        [phases addObject:ph];
        NSString *url = nil, *outdir = nil;
        for (int i = 1; i < argc; i++) {
            const char *o = argv[i];
            if (!strcmp(o, "--")) {
                if (!outdir) usage();
                PSPhase *n = [PSPhase new]; n.width = ph.width; n.wait = ph.wait; n.names = [NSMutableArray array]; n.sels = [NSMutableDictionary dictionary]; n.scales = [NSMutableDictionary dictionary];
                [phases addObject:n]; ph = n; continue;
            }
            if (o[0] == '-' && o[1]) {
                if (i + 1 >= argc) usage();
                const char *v = argv[++i];
                if (!strcmp(o, "-w")) ph.width = atof(v); else if (!strcmp(o, "-j")) ph.script = @(v); else if (!strcmp(o, "-T")) ph.wait = atof(v);
                else if (!strcmp(o, "-h")) H = atof(v); else if (!strcmp(o, "-t")) wait = atof(v); else if (!strcmp(o, "-s")) scale = atof(v);
                else if (!strcmp(o, "-p")) pad = atof(v); else if (!strcmp(o, "-x")) textdir = @(v); else usage();
                continue;
            }
            if (!url) { url = @(o); continue; }
            if (!outdir) { outdir = @(o); continue; }
            NSString *a = @(o); NSRange eq = [a rangeOfString:@"="];
            if (eq.location == NSNotFound) usage();
            NSString *n = [a substringToIndex:eq.location], *sel = [a substringFromIndex:eq.location + 1];
            NSRange at = [n rangeOfString:@"@"];
            if (at.location != NSNotFound) { ph.scales[[n substringToIndex:at.location]] = @([n substringFromIndex:at.location + 1].doubleValue); n = [n substringToIndex:at.location]; }
            [ph.names addObject:n]; ph.sels[n] = sel;
        }
        if (!outdir) usage();
        [[NSFileManager defaultManager] createDirectoryAtPath:outdir withIntermediateDirectories:YES attributes:nil error:nil];
        if (textdir) [[NSFileManager defaultManager] createDirectoryAtPath:textdir withIntermediateDirectories:YES attributes:nil error:nil];

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // no Dock icon, never activates
        [NSApp finishLaunching];
        double W = phases[0].width;
        WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
        cfg.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];   // no remembered settings (localStorage) in, none left behind
        WKWebView *wv = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, W, H) configuration:cfg];
        SEL occ = NSSelectorFromString(@"_setWindowOcclusionDetectionEnabled:");   // keep painting although no one can see it
        if ([wv respondsToSelector:occ]) { void (*f)(id, SEL, BOOL) = (void *)[wv methodForSelector:occ]; f(wv, occ, NO); }
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(-30000, -30000, W, H) styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.ignoresMouseEvents = YES; win.contentView = wv; [win orderFrontRegardless];
        PSNav *nav = [PSNav new]; wv.navigationDelegate = nav;
        [wv loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
        spin(30, ^{ return nav.done; });
        if (!nav.done || nav.err) { fprintf(stderr, "pageshot: cannot load %s: %s\n", url.UTF8String, nav.err ? nav.err.UTF8String : "timeout"); return 1; }
        spin(wait, nil);

        int bad = 0;
        for (PSPhase *p in phases) {
            if (p != phases[0]) {   // a later phase: back to the layout height (and its width), as the page was loaded
                W = p.width; [win setContentSize:NSMakeSize(W, H)]; wv.frame = NSMakeRect(0, 0, W, H); spin(1, nil);
            }
            if (p.script) { id r = runJS(wv, p.script, nil, 120); if ([r isKindOfClass:[NSString class]]) printf("script: %s\n", [r UTF8String]); spin(p.wait, nil); }
            if (!p.names.count) continue;
            // the whole page in the view (no scrolling: page coordinates = view coordinates), then every element's box
            NSString *measure = @"const r={}; for(const [k,s] of Object.entries(sels)){ const els=[...document.querySelectorAll(s)]; if(!els.length){ r[k]=null; continue; }"
                " let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9; for(const el of els){ const b=el.getBoundingClientRect(); x0=Math.min(x0,b.left); y0=Math.min(y0,b.top); x1=Math.max(x1,b.right); y1=Math.max(y1,b.bottom); }"
                " r[k]=[x0+scrollX,y0+scrollY,x1-x0,y1-y0,els.map(el=>el.innerText||'').join('\\n')]; }"
                " r._h=Math.ceil(document.documentElement.scrollHeight); return JSON.stringify(r);";
            NSDictionary *m = nil;
            for (int pass = 0; pass < 2; pass++) {
                NSString *js = runJS(wv, measure, @{@"sels": p.sels}, 30);
                m = js ? [NSJSONSerialization JSONObjectWithData:[js dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] : nil;
                if (!m) { fprintf(stderr, "pageshot: cannot measure the page\n"); return 1; }
                double hh = [m[@"_h"] doubleValue];
                if (pass || hh <= wv.frame.size.height) break;
                [win setContentSize:NSMakeSize(W, hh)]; wv.frame = NSMakeRect(0, 0, W, hh);
                spin(1.5, nil);
            }
            double VH = wv.frame.size.height;
            for (NSString *n in p.names) {
                NSArray *b = m[n];
                if (![b isKindOfClass:[NSArray class]]) { fprintf(stderr, "pageshot: %s: nothing matches %s\n", n.UTF8String, [p.sels[n] UTF8String]); bad = 1; continue; }
                double x0 = fmax(0, [b[0] doubleValue] - pad), y0 = fmax(0, [b[1] doubleValue] - pad);
                double x1 = fmin(W, [b[0] doubleValue] + [b[2] doubleValue] + pad), y1 = fmin(VH, [b[1] doubleValue] + [b[3] doubleValue] + pad);
                if (x1 - x0 < 2 || y1 - y0 < 2) { fprintf(stderr, "pageshot: %s: %s is not visible\n", n.UTF8String, [p.sels[n] UTF8String]); bad = 1; continue; }
                double sc = p.scales[n] ? [p.scales[n] doubleValue] : scale;
                NSInteger pw = lround((x1 - x0) * sc), phh = lround((y1 - y0) * sc);
                WKSnapshotConfiguration *c = [WKSnapshotConfiguration new];
                c.rect = NSMakeRect(x0, y0, x1 - x0, y1 - y0);
                c.snapshotWidth = @(pw / win.backingScaleFactor);   // points: rendered at this size, not scaled up afterwards
                c.afterScreenUpdates = YES;
                __block NSImage *img = nil; __block BOOL fin = NO;
                [wv takeSnapshotWithConfiguration:c completionHandler:^(NSImage *im, NSError *e) {
                    img = im; fin = YES; if (e) fprintf(stderr, "pageshot: %s: %s\n", n.UTF8String, e.localizedDescription.UTF8String); }];
                spin(30, ^{ return fin; });
                NSString *path = [outdir stringByAppendingPathComponent:[n stringByAppendingString:@".png"]];
                if (!img || !writePNG(img, pw, phh, path)) { fprintf(stderr, "pageshot: %s: no picture\n", n.UTF8String); bad = 1; continue; }
                if (textdir) [b[4] writeToFile:[textdir stringByAppendingPathComponent:[n stringByAppendingString:@".txt"]] atomically:YES encoding:NSUTF8StringEncoding error:nil];
                unsigned long long sz = [[[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil] fileSize];
                printf("%-28s %5ldx%-5ld %6.0f KB  %s\n", [n stringByAppendingString:@".png"].UTF8String, (long)pw, (long)phh, sz / 1024.0, [p.sels[n] UTF8String]);
            }
        }
        return bad;
    }
}
