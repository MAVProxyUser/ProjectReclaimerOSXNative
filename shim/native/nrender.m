// nrender.m -- the NATIVE app's frames (part of nhook.dylib): our overlay, clip recording and frame captures, drawn into
// the frame the app itself presents. The app renders with Metal (its own Direct3D 11 in Rust) and ends every frame
// with -[MTLCommandBuffer presentDrawable:]; that method is swizzled on the GPU's command buffer class (found by making
// one command buffer on the system device). Before the original runs, on the app's own command buffer (not yet
// committed, so ordered after the frame's drawing):
//   1. a requested capture: the drawable copied into a shared texture -> ~/reclaimer_cap_<pid>_<n>.bmp when the GPU is done
//   2. the overlay: noverlay.c's lines (NDC + colour) in one render pass (load, store) on the drawable
//   3. a recording: the drawable scaled into the recorder's next IOSurface pixel buffer (shim/src/rec.m, REC_NO_GL:
//      rec_video_begin / rec_video_end) -- the overlay included, as in the loader
// Reading the drawable (1, 3) needs a CAMetalLayer with framebufferOnly = NO: the first capture / recording switches the
// layer (its next frames are readable; the first request waits for one) and -[CAMetalLayer setFramebufferOnly:] keeps it
// NO from then on. Until then the app's layer stays as the app made it (framebuffer-only is the faster one to display). NHOOK_NO_RENDER=1 = none of this.
// ARC.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../src/rec.h"

void nh_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // nhook.c
int nov_build(float* verts, int cap, int w, int h, float scale);           // noverlay.c
void nov_aim_tick(void);                                                   // noverlay.c: aim vs hits, measured against the hitboxes
int nov_wanted(void);
volatile int g_nr_cap_req;                                                  // the stats page's Capture frame (do=capture)
volatile int g_nr_cap_depth;                                                // ... &depth=1: also every full-size depth texture of the frame (.f32)
volatile int g_nr_w, g_nr_h; volatile double g_nr_last;                    // the last presented frame: size (pixels), when (mono s)
volatile unsigned long g_nr_frames;
static IMP g_orig_present, g_orig_fbo;
// frame pacing for the stats page's fps line: every present's time (ring)
static double g_nr_pt[512]; static volatile unsigned g_nr_pn;
// fps over the last second, frame-interval median / 95th percentile / max (ms); 0 = no presents in that second
int nr_perf(double* fps, double* p50, double* p95, double* mx) {
    double now = 0; { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); now = t.tv_sec + t.tv_nsec * 1e-9; }
    unsigned n = g_nr_pn; double iv[512]; int k = 0, frames = 0;
    for (unsigned i = 1; i < 511 && i < n; i++) { double a = g_nr_pt[(n - i) & 511], b = g_nr_pt[(n - i - 1) & 511]; if (now - a > 1.0) break; frames++; if (a > b && b > 0) iv[k++] = (a - b) * 1e3; }
    if (!frames) return 0;
    for (int i = 1; i < k; i++) { double x = iv[i]; int j = i - 1; while (j >= 0 && iv[j] > x) { iv[j + 1] = iv[j]; j--; } iv[j + 1] = x; }
    *fps = frames; *p50 = k ? iv[k / 2] : 0; *p95 = k ? iv[(k * 95) / 100 < k ? (k * 95) / 100 : k - 1] : 0; *mx = k ? iv[k - 1] : 0; return 1;
}
static id<MTLDevice> g_dev;
static NSMutableDictionary* g_pso_lines, *g_pso_blit;                       // pixel format -> pipeline
static CVMetalTextureCacheRef g_vcache;
static unsigned long g_ncap;
static volatile int g_nr_readback;                                          // a capture / recording asked to read frames: the layer stays readable

static double nr_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static NSString* const kShaders = @
    "#include <metal_stdlib>\n using namespace metal;\n"
    "struct VO { float4 p [[position]]; float3 c; float d; };\n"
    "vertex VO nv_vs(uint vid [[vertex_id]], const device float* v [[buffer(0)]], constant uint& srgb [[buffer(1)]]) {\n"
    "  VO o; const device float* q = v + vid * 6; o.p = float4(q[0], q[1], 0, 1); float3 c = float3(q[2], q[3], q[4]);\n"
    "  o.c = srgb ? pow(c, 2.2) : c; o.d = q[5]; return o; }\n"
    // the scene's depth (reverse-Z, near 1/128): a pixel whose own depth, moved 0.3 units toward the camera, is farther than the scene there is hidden
    "fragment float4 nv_fs(VO i [[stage_in]], depth2d<float> dt [[texture(0)]], constant float4& u [[buffer(0)]]) {\n"
    "  if (i.d > 0 && u.x > 0) { constexpr sampler sm(coord::normalized, filter::nearest, address::clamp_to_edge);\n"
    "    float sc = dt.sample(sm, i.p.xy / u.yz); float w = 0.0078125 / i.d - 0.3; float dd = 0.0078125 / max(w, 0.02);\n"
    "    if (sc > dd) discard_fragment(); }\n"
    "  return float4(i.c, 1); }\n"
    "struct BO { float4 p [[position]]; float2 uv; };\n"
    "vertex BO nb_vs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); BO o; o.p = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o; }\n"
    "fragment float4 nb_fs(BO i [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::linear, address::clamp_to_edge); return float4(t.sample(s, i.uv).rgb, 1); }\n";
static id<MTLLibrary> nr_lib(void) {
    static id<MTLLibrary> lib; static int tried; if (lib || tried) return lib; tried = 1; NSError* e = nil;
    lib = [g_dev newLibraryWithSource:kShaders options:nil error:&e];
    if (!lib) nh_log("[nhook] render: shaders did not compile: %s", e.localizedDescription.UTF8String);
    return lib;
}
static id<MTLRenderPipelineState> nr_pso(NSMutableDictionary* cache, NSString* vs, NSString* fs, MTLPixelFormat pf, BOOL blend) {
    NSNumber* k = @(pf); id<MTLRenderPipelineState> p = cache[k]; if (p) return p;
    id<MTLLibrary> lib = nr_lib(); if (!lib) return nil;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs]; d.fragmentFunction = [lib newFunctionWithName:fs]; d.colorAttachments[0].pixelFormat = pf;
    (void)blend; NSError* e = nil; p = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) { nh_log("[nhook] render: no pipeline for pixel format %lu: %s", (unsigned long)pf, e.localizedDescription.UTF8String); return nil; }
    cache[k] = p; return p;
}
static BOOL nr_srgb(MTLPixelFormat f) { return f == MTLPixelFormatBGRA8Unorm_sRGB || f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGR10_XR_sRGB || f == MTLPixelFormatBGRA10_XR_sRGB; }
// tex drawn into dst (scaled, any format) as one full-screen triangle
static void nr_blit(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst) {
    id<MTLRenderPipelineState> p = nr_pso(g_pso_blit, @"nb_vs", @"nb_fs", dst.pixelFormat, NO); if (!p) return;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp]; if (!en) return;
    [en setRenderPipelineState:p]; [en setFragmentTexture:src atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
}
static void nr_bmp(const char* path, const uint8_t* bgra, int w, int h, size_t bpr) {   // 32-bit top-down BMP
    FILE* f = fopen(path, "wb"); if (!f) return; uint32_t sz = 54 + (uint32_t)w * h * 4;
    uint8_t hd[54] = {'B', 'M'}; memcpy(hd + 2, &sz, 4); uint32_t off = 54, dib = 40, img = (uint32_t)w * h * 4; int32_t ww = w, hh = -h; uint16_t pl = 1, bpp = 32;
    memcpy(hd + 10, &off, 4); memcpy(hd + 14, &dib, 4); memcpy(hd + 18, &ww, 4); memcpy(hd + 22, &hh, 4); memcpy(hd + 26, &pl, 2); memcpy(hd + 28, &bpp, 2); memcpy(hd + 34, &img, 4);
    fwrite(hd, 1, 54, f); for (int y = 0; y < h; y++) fwrite(bgra + (size_t)y * bpr, 1, (size_t)w * 4, f); fclose(f);
}

// ---- the scene's depth (for hiding the overlay behind the world): every depth texture the app attaches to a render pass
// is seen here (-[MTLRenderPassDepthAttachmentDescriptor setTexture:] swizzled); NHOOK_DEPTH_PROBE=1 logs each new one
static IMP g_orig_dtex; static NSMutableSet* g_dseen; static id<MTLTexture> g_dlast; static volatile unsigned long g_dpasses;
// full-size depth textures (the biggest seen): the scene's is among them; frame = the present count when last attached,
// order = how many full-size attachments came before it in that frame
typedef struct { __unsafe_unretained id<MTLTexture> t; unsigned long frame, first_frame; int order, n_in_frame; } NrDepth;
static NrDepth g_dfull[8]; static int g_ndfull; static unsigned long g_dfull_frame; static int g_dfull_order; static NSUInteger g_dbig;
static void my_set_dtex(id self, SEL cmd, id<MTLTexture> t) {
    ((void (*)(id, SEL, id))g_orig_dtex)(self, cmd, t);
    if (!t) return; g_dpasses++;
    @synchronized (g_dseen) {
        NSUInteger area = t.width * t.height; if (area > g_dbig) { g_dbig = area; g_ndfull = 0; }
        if (area == g_dbig) { unsigned long fr = g_nr_frames; if (fr != g_dfull_frame) { g_dfull_frame = fr; g_dfull_order = 0; }
            int k = 0; for (; k < g_ndfull; k++) if (g_dfull[k].t == t) break;
            if (k == g_ndfull && g_ndfull < 8) { g_dfull[k].t = t; g_dfull[k].first_frame = fr; g_ndfull++; }
            if (k < 8) { if (g_dfull[k].frame != fr) g_dfull[k].n_in_frame = 0; g_dfull[k].frame = fr; g_dfull[k].order = g_dfull_order; g_dfull[k].n_in_frame++; }
            g_dfull_order++; } }
    @synchronized (g_dseen) {
        if (![g_dseen containsObject:t]) { [g_dseen addObject:t];
            if (getenv("NHOOK_DEPTH_PROBE")) nh_log("[nhook] [depth] new depth texture %p %lux%lu format %lu usage 0x%lx storage %lu samples %lu (frame %lu)", (__bridge void*)t,
                (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.usage, (unsigned long)t.storageMode, (unsigned long)t.sampleCount, g_nr_frames); }
        g_dlast = t; }
}
static void nr_capture(id<MTLCommandBuffer> cb, id<MTLTexture> tex) {
    g_nr_cap_req = 0; int w = (int)tex.width, h = (int)tex.height;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:nr_srgb(tex.pixelFormat) ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:td]; if (!dst) return;
    nr_blit(cb, tex, dst); unsigned long n = ++g_ncap; int pid = getpid();
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; size_t bpr = (size_t)w * 4; uint8_t* px = malloc(bpr * h); if (!px) return;
        [dst getBytes:px bytesPerRow:bpr fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        const char* home = getenv("HOME"); char p[1100]; snprintf(p, sizeof p, "%s/reclaimer_cap_%d_%lu.bmp", home ? home : "/tmp", pid, n);
        nr_bmp(p, px, w, h, bpr); free(px); nh_log("[capture] %s (%dx%d)", p, w, h); }];
}

// The scene's depth for this frame: the full-size depth texture attached most often in the frame being presented (or the last
// one) -- 0 when none was seen this frame or its shape is not the drawable's (split screen, a different render aspect)
static id<MTLTexture> nr_scene_depth(void) {
    if (!g_dseen || getenv("NHOOK_NO_OCCLUSION")) return nil; id<MTLTexture> best = nil; int bn = -1; unsigned long fr = g_nr_frames;
    @synchronized (g_dseen) { for (int k = 0; k < g_ndfull; k++) if (g_dfull[k].t && g_dfull[k].frame + 1 >= fr && g_dfull[k].n_in_frame > bn) { bn = g_dfull[k].n_in_frame; best = g_dfull[k].t; } }
    if (best && g_nr_w > 0 && g_nr_h > 0 && fabs((double)best.width / best.height - (double)g_nr_w / g_nr_h) > 0.02) return nil;
    return best;
}
static id<MTLTexture> nr_dummy_depth(void) {   // (a depth texture must be bound even when unused)
    static id<MTLTexture> t; if (!t) { MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:1 height:1 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate; t = [g_dev newTextureWithDescriptor:d]; }
    return t;
}
static void nr_depth_dump(id<MTLCommandBuffer> cb) {   // every full-size depth texture attached in this frame -> ~/reclaimer_dep_<pid>_<k>.f32 + stats
    g_nr_cap_depth = 0; unsigned long fr = g_nr_frames; int pid = getpid();
    @synchronized (g_dseen) { for (int k = 0; k < g_ndfull; k++) { id<MTLTexture> t = g_dfull[k].t; if (!t || g_dfull[k].frame + 1 < fr) continue;
        NSUInteger w = t.width, h = t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared]; if (!b) continue;
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; if (!be) continue;
        [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:MTLBlitOptionDepthFromDepthStencil];
        [be endEncoding]; int order = g_dfull[k].order, nin = g_dfull[k].n_in_frame; void* tp = (__bridge void*)t;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const float* d = b.contents; double sum = 0; float mn = 1e30f, mx = -1e30f; size_t nz = 0;
            for (size_t i = 0; i < w * h; i++) { float v = d[i]; if (v < mn) mn = v; if (v > mx) mx = v; if (v != 0) { nz++; sum += v; } }
            const char* home = getenv("HOME"); char p[1100]; snprintf(p, sizeof p, "%s/reclaimer_dep_%d_%d.f32", home ? home : "/tmp", pid, k);
            FILE* f = fopen(p, "wb"); if (f) { fwrite(d, 4, w * h, f); fclose(f); }
            nh_log("[capture] depth %d (%p %lux%lu, attached as full-size #%d of the frame, %d times): centre %.6f, min %.6f max %.6f, %.1f%% non-zero, mean %.6f -> %s",
                k, tp, (unsigned long)w, (unsigned long)h, order, nin, d[(h / 2) * w + w / 2], mn, mx, 100.0 * nz / (w * h), nz ? sum / nz : 0.0, p); }]; } }
}

static void nr_frame(id<MTLCommandBuffer> cb, id<CAMetalDrawable> drawable) {
    id<MTLTexture> tex = drawable.texture; if (!tex) return;
    int w = (int)tex.width, h = (int)tex.height; g_nr_w = w; g_nr_h = h; g_nr_last = nr_now(); g_nr_frames++;
    { unsigned i = g_nr_pn; g_nr_pt[i & 511] = g_nr_last; __atomic_store_n(&g_nr_pn, i + 1, __ATOMIC_RELEASE); }
    if (!g_dev) g_dev = tex.device;
    CAMetalLayer* layer = [(id)drawable respondsToSelector:@selector(layer)] ? (CAMetalLayer*)drawable.layer : nil;
    if ((g_nr_cap_req || g_rec_video) && !g_nr_readback) g_nr_readback = 1;
    if (tex.framebufferOnly && layer && g_nr_readback) { static int told; if (!told) { told = 1; nh_log("[nhook] render: the layer is readable from now on (captures, recordings)"); }
        ((void (*)(id, SEL, BOOL))g_orig_fbo)(layer, @selector(setFramebufferOnly:), NO); }
    BOOL readable = !tex.framebufferOnly;
    // 1. capture (the game's picture, no overlay: as the loader's; do=capture&overlay=1 = after the overlay)
    if (g_nr_cap_depth && g_dseen) nr_depth_dump(cb);
    if (g_nr_cap_req == 1 && readable) nr_capture(cb, tex);
    nov_aim_tick();   // (every frame, overlay on or off)
    // 2. the overlay
    if (nov_wanted()) {
        enum { CAP = 400000 }; static float* verts; if (!verts) verts = malloc(CAP * 6 * sizeof(float));
        float scale = layer ? (float)layer.contentsScale : 1.0f;
        int nv = verts ? nov_build(verts, CAP, w, h, scale) : 0;
        id<MTLRenderPipelineState> p = nv ? nr_pso(g_pso_lines, @"nv_vs", @"nv_fs", tex.pixelFormat, NO) : nil;
        if (p) {
            id<MTLBuffer> vb = [g_dev newBufferWithBytes:verts length:(NSUInteger)nv * 24 options:MTLResourceStorageModeShared];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = vb ? [cb renderCommandEncoderWithDescriptor:rp] : nil;
            if (en) { uint32_t srgb = nr_srgb(tex.pixelFormat); [en setRenderPipelineState:p]; [en setVertexBuffer:vb offset:0 atIndex:0]; [en setVertexBytes:&srgb length:4 atIndex:1];
                id<MTLTexture> dtex = nr_scene_depth(); float u[4] = {dtex ? 1.0f : 0.0f, (float)w, (float)h, 0};   // (the scene's depth: hides what is behind the world)
                if (dtex) [en setFragmentTexture:dtex atIndex:0]; else [en setFragmentTexture:nr_dummy_depth() atIndex:0];
                [en setFragmentBytes:u length:sizeof u atIndex:0];
                [en drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:(NSUInteger)nv]; [en endEncoding];
                [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; (void)vb; }]; }   // (kept until the GPU is done: the app may not retain references)
        }
    }
    if (g_nr_cap_req == 2 && readable) nr_capture(cb, tex);
    // 3. recording
    void* pb = NULL; int64_t pts = 0; int sess = 0, ow = 0, oh = 0;
    if (g_rec_video && rec_video_begin(&pb, &pts, &sess, &ow, &oh)) {
        CVMetalTextureRef mt = NULL;
        if (!g_vcache && CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, g_dev, NULL, &g_vcache)) g_vcache = NULL;
        if (readable && g_vcache && !CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, g_vcache, (CVPixelBufferRef)pb, NULL,
                nr_srgb(tex.pixelFormat) ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm, ow, oh, 0, &mt) && mt) {
            id<MTLTexture> dst = CVMetalTextureGetTexture(mt); nr_blit(cb, tex, dst);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { rec_video_end(pb, pts, sess, c.status == MTLCommandBufferStatusCompleted); CFRelease(mt); }];
        } else rec_video_end(pb, pts, sess, 0);
        if (g_vcache) CVMetalTextureCacheFlush(g_vcache, 0);
    }
}
static void my_present(id self, SEL cmd, id drawable) {
    @autoreleasepool { @try { if (drawable) nr_frame((id<MTLCommandBuffer>)self, (id<CAMetalDrawable>)drawable); }
        @catch (NSException* ex) { static int told; if (!told++) nh_log("[nhook] render: exception %s (%s)", ex.name.UTF8String, ex.reason.UTF8String); } }
    ((void (*)(id, SEL, id))g_orig_present)(self, cmd, drawable);
}
static void my_set_fbo(id self, SEL cmd, BOOL v) { ((void (*)(id, SEL, BOOL))g_orig_fbo)(self, cmd, g_nr_readback ? NO : v); }

void nr_install(void) {
    if (getenv("NHOOK_NO_RENDER")) return;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); if (!dev) { nh_log("[nhook] render: no Metal device"); return; }
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; if (!cb) return;
        Class c = object_getClass(cb); Method m = class_getInstanceMethod(c, @selector(presentDrawable:));
        Method fm = class_getInstanceMethod([CAMetalLayer class], @selector(setFramebufferOnly:));
        if (!m || !fm) { nh_log("[nhook] render: presentDrawable: / setFramebufferOnly: not found"); return; }
        g_dev = dev; g_pso_lines = [NSMutableDictionary new]; g_pso_blit = [NSMutableDictionary new];
        g_orig_fbo = method_setImplementation(fm, (IMP)my_set_fbo);
        { MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];   // the scene's depth (overlay hiding)
          Method dm = class_getInstanceMethod(object_getClass(rp.depthAttachment), @selector(setTexture:));
          if (dm && !getenv("NHOOK_NO_DEPTH")) { g_dseen = [NSMutableSet new]; g_orig_dtex = method_setImplementation(dm, (IMP)my_set_dtex);
              nh_log("[nhook] [depth] watching -[%s setTexture:]", class_getName(object_getClass(rp.depthAttachment))); } }
        g_orig_present = method_setImplementation(m, (IMP)my_present);
        nh_log("[nhook] render: hooked -[%s presentDrawable:] (overlay, recording, captures)", class_getName(c));
    }
}
