// d2d.c -- Direct2D 1.1 on CoreGraphics, drawn into the D3D11->GL translator's textures (2026-10-21). Included by peload.c after
// the translator; it includes wic.c (WIC on ImageIO) and dwrite.c (DirectWrite on CoreText), which share its COM object base.
// First user: Project Gravemind (Halo 2), whose launcher and in-game screens are all Direct2D on the D3D11 back buffer
// (ID2D1Factory1 -> CreateDevice(IDXGIDevice) -> CreateDeviceContext -> CreateBitmapFromDxgiSurface(back buffer) + SetTarget,
// BeginDraw / shapes / DrawTextLayout / DrawBitmap / EndDraw every frame). Before, D2D1CreateFactory failed and it gave up
// ("ui: Direct2D init failed").
//   How a device context draws: everything but two operations goes onto a CPU canvas the size of its target (a CGBitmapContext,
//   premultiplied BGRA, y down) -- rectangles, rounded rectangles, ellipses, geometries (paths with lines / beziers / arcs),
//   strokes with caps / joins / dashes, solid / linear / radial / bitmap brushes with opacity and transforms, text, CPU bitmaps,
//   clips and layers (content bounds, geometric masks, opacity). The canvas goes onto the target texture (premultiplied
//   source-over, only the box drawn into) at EndDraw / Flush and before the two GPU operations: Clear (a GL clear in the clip)
//   and DrawBitmap of a texture (a GL quad: a video frame never comes back to the CPU). A texture is read back only when a CPU
//   path needs it (a texture bitmap in a brush, under an opacity layer or a geometric mask, or into a CPU target).
//   A target that is a CPU bitmap (CreateBitmap with the TARGET option) is drawn directly. Anything not implemented logs once:
//   "[d2d] <interface>::<method> (slot N) not implemented" and answers E_NOTIMPL. PELOAD_NO_D2D = the old failing factories;
//   PELOAD_TRACE_D2D logs creation, draws per EndDraw and composites.
#include "d2d_slots.h"
// ==== COM objects (d2d.c / dwrite.c / wic.c) ===================================================================================
// A class = its interface name, slot names (d2d_slots.h), the IIDs QueryInterface answers, a destructor and the slots it implements;
// every other slot is a logging stub.
typedef struct D2Cls { const char*name; const char*const*slots; int nslots; const GUID_*iids[8]; void(*dtor)(void*); void(*setup)(void**); void**vt; uint8_t logged[128]; } D2Cls;
typedef struct { void**vtbl; D2Cls*cls; volatile int refs; void*fac; } D2Hdr;
static pthread_mutex_t g_d2_mx=PTHREAD_MUTEX_INITIALIZER;
static int64_t d2_unimpl(void*self,int k){ D2Hdr*h=self; D2Cls*c=h?h->cls:0;
    if(c && k<128 && !c->logged[k]){ c->logged[k]=1; char m[200]; int n=snprintf(m,sizeof m,"  [d2d] %s::%s (slot %d) not implemented\n",c->name,k<c->nslots?c->slots[k]:"?",k); (void)!write(2,m,n); }
    return (int64_t)(int32_t)HR_E_NOTIMPL; }
#define D2S(k) static int64_t MSABI d2s_##k(void*s){ return d2_unimpl(s,k); }
#define D2S10(t) D2S(t##0) D2S(t##1) D2S(t##2) D2S(t##3) D2S(t##4) D2S(t##5) D2S(t##6) D2S(t##7) D2S(t##8) D2S(t##9)
D2S(0) D2S(1) D2S(2) D2S(3) D2S(4) D2S(5) D2S(6) D2S(7) D2S(8) D2S(9) D2S10(1) D2S10(2) D2S10(3) D2S10(4) D2S10(5) D2S10(6) D2S10(7) D2S10(8) D2S10(9)
#define D2P10(t) (void*)d2s_##t##0,(void*)d2s_##t##1,(void*)d2s_##t##2,(void*)d2s_##t##3,(void*)d2s_##t##4,(void*)d2s_##t##5,(void*)d2s_##t##6,(void*)d2s_##t##7,(void*)d2s_##t##8,(void*)d2s_##t##9
static void*const g_d2stubs[100]={(void*)d2s_0,(void*)d2s_1,(void*)d2s_2,(void*)d2s_3,(void*)d2s_4,(void*)d2s_5,(void*)d2s_6,(void*)d2s_7,(void*)d2s_8,(void*)d2s_9,
    D2P10(1),D2P10(2),D2P10(3),D2P10(4),D2P10(5),D2P10(6),D2P10(7),D2P10(8),D2P10(9)};
static unsigned MSABI d2_AddRef(void*s){ return (unsigned)__atomic_add_fetch(&((D2Hdr*)s)->refs,1,__ATOMIC_ACQ_REL); }
static unsigned MSABI d2_Release(void*s){ D2Hdr*h=s; int r=__atomic_sub_fetch(&h->refs,1,__ATOMIC_ACQ_REL);
    if(r==0){ if(h->cls->dtor) h->cls->dtor(s); free(s); } return (unsigned)(r<0?0:r); }
static int MSABI d2_QI(void*s,const GUID_*iid,void**pp){ D2Hdr*h=s; if(!pp) return (int)HR_INVALIDARG;
    for(int i=0;i<8 && h->cls->iids[i];i++) if(iid && !memcmp(h->cls->iids[i],iid,16)){ *pp=s; d2_AddRef(s); return 0; }
    *pp=0; static int nl; if(nl++<40 && iid){ char m[200]; int n=snprintf(m,sizeof m,"  [d2d] %s: QueryInterface %08x-%04x-%04x-%02x%02x... not supported\n",h->cls->name,iid->D1,iid->D2,iid->D3,iid->D4[0],iid->D4[1]); (void)!write(2,m,n); }
    return (int)0x80004002; }   // E_NOINTERFACE
static void** d2_vtable(D2Cls*c){ void**v=__atomic_load_n(&c->vt,__ATOMIC_ACQUIRE); if(v) return v;
    pthread_mutex_lock(&g_d2_mx);
    if(!c->vt){ v=calloc((size_t)(c->nslots>3?c->nslots:3),sizeof(void*)); for(int i=0;i<c->nslots && i<100;i++) v[i]=g_d2stubs[i];
        v[0]=(void*)d2_QI; v[1]=(void*)d2_AddRef; v[2]=(void*)d2_Release; if(c->setup) c->setup(v); __atomic_store_n(&c->vt,v,__ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_d2_mx); return c->vt; }
static void* d2_alloc(D2Cls*c,size_t sz){ D2Hdr*h=calloc(1,sz); h->vtbl=d2_vtable(c); h->cls=c; h->refs=1; return h; }
// AddRef / Release of any COM object (ours or the game's own) through its vtable
static void d2_addref(void*p){ if(p) ((unsigned(MSABI*)(void*))(*(void***)p)[1])(p); }
static void d2_release(void*p){ if(p) ((unsigned(MSABI*)(void*))(*(void***)p)[2])(p); }
static int d2_trace(void){ static int t=-1; if(t<0) t=getenv("PELOAD_TRACE_D2D")?1:0; return t; }
#include "wic.c"
#include "dwrite.c"
// ==== Direct2D types ===========================================================================================================
typedef struct { float x,y; } D2Pt;
typedef struct { float l,t,r,b; } D2Rect;
typedef struct { float w,h; } D2SizeF;
typedef struct { uint32_t w,h; } D2SizeU;
typedef struct { float r,g,b,a; } D2Col;
typedef struct { float m11,m12,m21,m22,dx,dy; } D2Mat;
typedef struct { uint32_t fmt, alpha; } D2PixFmt;
typedef struct { D2Rect rect; float rx,ry; } D2RRect;
typedef struct { D2Pt c; float rx,ry; } D2Ell;
typedef struct { float opacity; D2Mat m; } D2BrushProps;
typedef struct { float pos; D2Col c; } D2Stop;
typedef struct { D2PixFmt pf; float dpix,dpiy; } D2BmpProps;
typedef struct { D2PixFmt pf; float dpix,dpiy; uint32_t opts; void*cc; } D2BmpProps1;
typedef struct { D2Rect bounds; void*mask; uint32_t maskaa; D2Mat masktf; float opacity; void*opbrush; uint32_t opts; } D2LayerParams;
typedef struct { uint32_t type; D2PixFmt pf; float dpix,dpiy; uint32_t usage, minlevel; } D2RtProps;
typedef struct { uint32_t cap0,cap1,capd,join; float miter; uint32_t dash; float doff; uint32_t tftype; } D2StrokeProps;
typedef struct { uint32_t aa,taa; uint64_t tag1,tag2; D2Mat tf; uint32_t blend,unit; } D2StateDesc;
static D2Mat d2_ident(void){ D2Mat m={1,0,0,1,0,0}; return m; }
static CGAffineTransform d2_cgm(const D2Mat*m){ return CGAffineTransformMake(m->m11,m->m12,m->m21,m->m22,m->dx,m->dy); }
static D2Cls C_d2fac, C_d2dev, C_d2ctx, C_d2bmp, C_d2solid, C_d2lin, C_d2rad, C_d2bbrush, C_d2stops, C_d2stroke, C_d2layer, C_d2block,
    C_d2grect, C_d2grrect, C_d2gell, C_d2gpath, C_d2gtf, C_d2ggroup, C_d2sink;
// ---- bitmaps: a D3D texture (CreateBitmapFromDxgiSurface) or CPU pixels (premultiplied BGRA, a CGBitmapContext when a target)
typedef struct { D2Hdr h; uint32_t w,hh,fmt,alpha,opts; float dpix,dpiy; D3DObj*tex; uint8_t*px; uint32_t stride; CGContextRef cg; CGImageRef img; uint32_t ver,imgver; int mapped;
    unsigned gltex; uint32_t glver, gltw, glth; } D2Bmp;   // gltex: a big CPU bitmap's GL copy (drawn on the GPU), current while glver == ver+1
static int d2_is_bmp(void*p){ return p && ((D2Hdr*)p)->cls==&C_d2bmp; }
static CGColorSpaceRef d2_srgb(void){ static CGColorSpaceRef cs; if(!cs) cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB); return cs; }
static D3DObj* d2_as_tex(void*p){ D3DObj*o=p; if(o && o->vtbl==(void**)g_dxsurf_vtbl) o=((DxSurf*)p)->tex;   // (its IDXGISurface, or the texture itself)
    return (o && o->vtbl==g_d3dres_vtbl && o->kind==2)?o:0; }
static void d2_bmp_dtor(void*p){ D2Bmp*b=p; if(b->tex) obj_Release(b->tex); if(b->cg) CGContextRelease(b->cg); if(b->img) CGImageRelease(b->img); free(b->px);
    if(b->gltex){ static void(*del)(int,const unsigned*); if(sub_on()) sub_drain(); GLLOCK(); if(d3dgl_ensure()){ if(!del) del=dlsym(RTLD_DEFAULT,"glDeleteTextures"); if(del) del(1,&b->gltex); } GLUNLOCK(); } }
static void d2_bmp_cpu(D2Bmp*b,uint32_t w,uint32_t h,int target){ b->w=w; b->hh=h; b->stride=w*4; b->px=calloc(1,(size_t)w*h*4+16);
    if(target){ b->cg=CGBitmapContextCreate(b->px,w,h,8,b->stride,d2_srgb(),kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
        if(b->cg){ CGContextTranslateCTM(b->cg,0,h); CGContextScaleCTM(b->cg,1,-1); } } }   // (y down, like Direct2D)
// a texture's pixels as premultiplied BGRA, top row first (malloc'd): GL read back when only the GPU has them, else its CPU copy
static uint8_t* d2_tex_read(D3DObj*t){
    uint32_t w=t->w, h=t->h; if(!w||!h||w>16384||h>16384) return 0; uint8_t*o=calloc(1,(size_t)w*h*4+16); if(!o) return 0;
    if(t->glfbo || (t->bindf&0x20) || t->copydst){
        if(sub_on()) sub_drain(); GLLOCK(); if(!d3dgl_ensure()){ GLUNLOCK(); return o; }
        if(!t->glfbo){ D3DObj*one=t; d3dgl_fbo_for(&one,1,0); }
        gl_bindfbo_any(GLE_FBO,t->glfbo); if(G.ReadBuffer) G.ReadBuffer(GLE_CA0); G.ReadPixels(0,0,(int)w,(int)h,GLE_BGRA,GLE_UI8REV,o); gl_bindfbo_any(GLE_FBO,0); GLUNLOCK();
        if(!g_d3dgl_flipy){ uint8_t*row=malloc((size_t)w*4); for(uint32_t y=0;y<h/2;y++){ memcpy(row,o+(size_t)y*w*4,(size_t)w*4); memcpy(o+(size_t)y*w*4,o+(size_t)(h-1-y)*w*4,(size_t)w*4); memcpy(o+(size_t)(h-1-y)*w*4,row,(size_t)w*4); } free(row); }
        return o; }
    if(!t->data) return o; const uint8_t*s=t->data; uint32_t rp=t->rowpitch?t->rowpitch:w*4; int rgba=(t->fmt==28||t->fmt==29||t->fmt==27), x8=(t->fmt==88||t->fmt==93);
    for(uint32_t y=0;y<h;y++){ const uint8_t*sr=s+(size_t)y*rp; uint8_t*d=o+(size_t)y*w*4;
        for(uint32_t x=0;x<w;x++){ d[x*4]=sr[x*4+(rgba?2:0)]; d[x*4+1]=sr[x*4+1]; d[x*4+2]=sr[x*4+(rgba?0:2)]; d[x*4+3]=x8?255:sr[x*4+3]; } }
    return o; }
// the bitmap as a CGImage (its current pixels): CPU bitmaps cache it per version; texture bitmaps read the texture back
static CGImageRef d2_bmp_image(D2Bmp*b){
    if(b->tex){ uint8_t*px=d2_tex_read(b->tex); if(!px) return 0; CFDataRef d=CFDataCreateWithBytesNoCopy(0,px,(CFIndex)b->tex->w*b->tex->h*4,kCFAllocatorMalloc);
        CGDataProviderRef dp=CGDataProviderCreateWithCFData(d); CFRelease(d);
        CGImageRef im=CGImageCreate(b->tex->w,b->tex->h,8,32,b->tex->w*4,d2_srgb(),(b->alpha==3?kCGImageAlphaNoneSkipFirst:kCGImageAlphaPremultipliedFirst)|kCGBitmapByteOrder32Little,dp,0,0,kCGRenderingIntentDefault);
        CGDataProviderRelease(dp); return im; }
    if(b->cg) return CGBitmapContextCreateImage(b->cg);   // (copy-on-write)
    if(b->img && b->imgver==b->ver){ CGImageRetain(b->img); return b->img; }
    if(b->img) CGImageRelease(b->img); CFDataRef d=CFDataCreate(0,b->px,(CFIndex)b->stride*b->hh); CGDataProviderRef dp=CGDataProviderCreateWithCFData(d); CFRelease(d);
    b->img=CGImageCreate(b->w,b->hh,8,32,b->stride,d2_srgb(),(b->alpha==3?kCGImageAlphaNoneSkipFirst:kCGImageAlphaPremultipliedFirst)|kCGBitmapByteOrder32Little,dp,0,0,kCGRenderingIntentDefault);
    CGDataProviderRelease(dp); b->imgver=b->ver; if(b->img) CGImageRetain(b->img); return b->img; }
static float d2_dpi(float v){ return v>0?v:96.0f; }
// ID2D1Bitmap(1): 4 GetSize, 5 GetPixelSize, 6 GetPixelFormat, 7 GetDpi, 8 CopyFromBitmap, 9 CopyFromRenderTarget, 10 CopyFromMemory,
// 11 GetColorContext, 12 GetOptions, 13 GetSurface, 14 Map, 15 Unmap
static D2SizeF* MSABI bmp_GetSize(void*s,D2SizeF*r){ D2Bmp*b=s; r->w=b->w*96.0f/b->dpix; r->h=b->hh*96.0f/b->dpiy; return r; }
static D2SizeU* MSABI bmp_GetPixelSize(void*s,D2SizeU*r){ D2Bmp*b=s; r->w=b->w; r->h=b->hh; return r; }
static D2PixFmt* MSABI bmp_GetPixelFormat(void*s,D2PixFmt*r){ D2Bmp*b=s; r->fmt=b->fmt; r->alpha=b->alpha; return r; }
static void MSABI bmp_GetDpi(void*s,float*x,float*y){ D2Bmp*b=s; if(x)*x=b->dpix; if(y)*y=b->dpiy; }
// write w x h premultiplied BGRA pixels at (x,y): into the CPU pixels, or the texture through the D3D11 context's UpdateSubresource
static void d2_bmp_put(D2Bmp*b,uint32_t x,uint32_t y,uint32_t w,uint32_t h,const uint8_t*src,uint32_t pitch,int src_fmt){
    if(x>=b->w||y>=b->hh) return; if(x+w>b->w) w=b->w-x; if(y+h>b->hh) h=b->hh-y; if(!w||!h) return;
    int rgba=(src_fmt==28||src_fmt==29||src_fmt==27), a8=(src_fmt==65), x8=(src_fmt==88||src_fmt==93||b->alpha==3);
    if(b->tex){ uint32_t box[6]={x,y,0,x+w,y+h,1}; int trgba=(b->tex->fmt==28||b->tex->fmt==29||b->tex->fmt==27);
        uint8_t*tmp=malloc((size_t)w*h*4); for(uint32_t r=0;r<h;r++){ const uint8_t*s=src+(size_t)r*pitch; uint8_t*d=tmp+(size_t)r*w*4;
            for(uint32_t i=0;i<w;i++){ uint8_t B,Gc,R,A; if(a8){ B=Gc=R=A=s[i]; } else { B=s[i*4+(rgba?2:0)]; Gc=s[i*4+1]; R=s[i*4+(rgba?0:2)]; A=x8?255:s[i*4+3]; }
                d[i*4]=trgba?R:B; d[i*4+1]=Gc; d[i*4+2]=trgba?B:R; d[i*4+3]=A; } }
        ctx_UpdateSubresource(0,b->tex,0,box,tmp,w*4,w*h*4); free(tmp); return; }
    for(uint32_t r=0;r<h;r++){ const uint8_t*s=src+(size_t)r*pitch; uint8_t*d=b->px+(size_t)(y+r)*b->stride+(size_t)x*4;
        for(uint32_t i=0;i<w;i++){ if(a8){ d[i*4]=d[i*4+1]=d[i*4+2]=d[i*4+3]=s[i]; continue; }
            d[i*4]=s[i*4+(rgba?2:0)]; d[i*4+1]=s[i*4+1]; d[i*4+2]=s[i*4+(rgba?0:2)]; d[i*4+3]=x8?255:s[i*4+3]; } }
    b->ver++; }
static int MSABI bmp_CopyFromMemory(void*s,const uint32_t*rc,const void*src,uint32_t pitch){ D2Bmp*b=s; if(!src) return (int)HR_INVALIDARG;
    uint32_t x=0,y=0,w=b->w,h=b->hh; if(rc){ x=rc[0]; y=rc[1]; w=rc[2]-rc[0]; h=rc[3]-rc[1]; if(rc[2]<rc[0]||rc[3]<rc[1]) return (int)HR_INVALIDARG; }
    d2_bmp_put(b,x,y,w,h,src,pitch,(int)b->fmt); return 0; }
static int MSABI bmp_CopyFromBitmap(void*s,const uint32_t*dst,void*srcb,const uint32_t*rc){ D2Bmp*b=s; if(!d2_is_bmp(srcb)) return (int)HR_INVALIDARG; D2Bmp*o=srcb;
    uint32_t sx=0,sy=0,w=o->w,h=o->hh; if(rc){ sx=rc[0]; sy=rc[1]; w=rc[2]-rc[0]; h=rc[3]-rc[1]; } uint32_t dx=dst?dst[0]:0, dy=dst?dst[1]:0;
    if(sx>=o->w||sy>=o->hh) return (int)HR_INVALIDARG; if(sx+w>o->w) w=o->w-sx; if(sy+h>o->hh) h=o->hh-sy;
    if(b->tex && o->tex && d3dgl_enabled() && gpu_copy_src_ok(o->tex)){ uint32_t box[6]={sx,sy,0,sx+w,sy+h,1}; ctx_CopySubresourceRegion(0,b->tex,0,dx,dy,0,o->tex,0,box); return 0; }
    uint8_t*px=o->tex?d2_tex_read(o->tex):0; const uint8_t*base=o->tex?px:o->px; uint32_t stride=o->tex?o->tex->w*4:o->stride;
    if(base) d2_bmp_put(b,dx,dy,w,h,base+(size_t)sy*stride+(size_t)sx*4,stride,87); free(px); return 0; }
static void MSABI bmp_GetColorContext(void*s,void**pp){ (void)s; if(pp)*pp=0; }
static uint32_t MSABI bmp_GetOptions(void*s){ return ((D2Bmp*)s)->opts; }
static int MSABI bmp_GetSurface(void*s,void**pp){ D2Bmp*b=s; if(!pp) return (int)HR_INVALIDARG; *pp=b->tex; if(b->tex){ obj_AddRef(b->tex); return 0; } return (int)0x88990024; }   // D2DERR_INVALID_CALL? no surface
static int MSABI bmp_Map(void*s,uint32_t opts,uint32_t*mr){ D2Bmp*b=s; (void)opts; if(!mr) return (int)HR_INVALIDARG;
    if(b->tex){ free(b->px); b->px=d2_tex_read(b->tex); b->stride=b->tex->w*4; }
    if(!b->px) return (int)0x8899000A; mr[0]=b->stride; *(uint8_t**)(mr+2)=b->px; b->mapped=1; return 0; }   // D2D1_MAPPED_RECT {pitch, pad, bits}
static int MSABI bmp_Unmap(void*s){ D2Bmp*b=s; b->mapped=0; b->ver++; return 0; }
static void MSABI d2_GetFactory(void*s,void**pp){ D2Hdr*h=s; if(pp){ *pp=h->fac; if(h->fac) d2_AddRef(h->fac); } }
static void d2_setup_bmp(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)bmp_GetSize; v[5]=(void*)bmp_GetPixelSize; v[6]=(void*)bmp_GetPixelFormat; v[7]=(void*)bmp_GetDpi;
    v[8]=(void*)bmp_CopyFromBitmap; v[10]=(void*)bmp_CopyFromMemory; v[11]=(void*)bmp_GetColorContext; v[12]=(void*)bmp_GetOptions; v[13]=(void*)bmp_GetSurface; v[14]=(void*)bmp_Map; v[15]=(void*)bmp_Unmap; }
// ---- brushes and gradient stops
typedef struct { D2Hdr h; D2Stop*s; int n; uint32_t gamma, extend; CGGradientRef grad; } D2Stops;
typedef struct { D2Hdr h; int kind; float opacity; D2Mat tf; D2Col col; D2Pt p0,p1,off; float rx,ry; D2Stops*stops; D2Bmp*bmp; uint32_t ex,ey,interp; } D2Brush;
enum { BR_SOLID=1, BR_LINEAR, BR_RADIAL, BR_BITMAP };
static D2Brush* d2_as_brush(void*p){ D2Hdr*h=p; return (h && (h->cls==&C_d2solid||h->cls==&C_d2lin||h->cls==&C_d2rad||h->cls==&C_d2bbrush))?p:0; }
static int d2_brush_solid_color(void*brush,float rgba[4]){ D2Brush*b=d2_as_brush(brush); if(!b||b->kind!=BR_SOLID) return 0;
    rgba[0]=b->col.r; rgba[1]=b->col.g; rgba[2]=b->col.b; rgba[3]=b->col.a*b->opacity; return 1; }
static void d2_stops_dtor(void*p){ D2Stops*s=p; free(s->s); if(s->grad) CGGradientRelease(s->grad); }
static void d2_brush_dtor(void*p){ D2Brush*b=p; if(b->stops) d2_release(b->stops); if(b->bmp) d2_release(b->bmp); }
static CGGradientRef d2_gradient(D2Stops*s){ if(s->grad) return s->grad; int n=s->n>0?s->n:1;
    CGFloat*comp=malloc(sizeof(CGFloat)*4*n), *loc=malloc(sizeof(CGFloat)*n);
    for(int i=0;i<n;i++){ D2Stop st=s->n>0?s->s[i]:(D2Stop){0,{0,0,0,0}}; comp[i*4]=st.c.r; comp[i*4+1]=st.c.g; comp[i*4+2]=st.c.b; comp[i*4+3]=st.c.a; loc[i]=st.pos<0?0:st.pos>1?1:st.pos; }
    s->grad=CGGradientCreateWithColorComponents(d2_srgb(),comp,loc,(size_t)n); free(comp); free(loc); return s->grad; }
static D2Brush* d2_brush_new(D2Cls*c,int kind,const D2BrushProps*bp,void*fac){ D2Brush*b=d2_alloc(c,sizeof(D2Brush)); b->h.fac=fac; b->kind=kind;
    b->opacity=bp?bp->opacity:1.0f; b->tf=bp?bp->m:d2_ident(); return b; }
static void MSABI br_SetOpacity(void*s,float o){ ((D2Brush*)s)->opacity=o; }
static void MSABI br_SetTransform(void*s,const D2Mat*m){ if(m) ((D2Brush*)s)->tf=*m; }
static float MSABI br_GetOpacity(void*s){ return ((D2Brush*)s)->opacity; }
static void MSABI br_GetTransform(void*s,D2Mat*m){ if(m) *m=((D2Brush*)s)->tf; }
static void MSABI sb_SetColor(void*s,const D2Col*c){ if(c) ((D2Brush*)s)->col=*c; }
static D2Col* MSABI sb_GetColor(void*s,D2Col*r){ *r=((D2Brush*)s)->col; return r; }
static void MSABI lb_SetStartPoint(void*s,D2Pt p){ ((D2Brush*)s)->p0=p; }
static void MSABI lb_SetEndPoint(void*s,D2Pt p){ ((D2Brush*)s)->p1=p; }
static D2Pt* MSABI lb_GetStartPoint(void*s,D2Pt*r){ *r=((D2Brush*)s)->p0; return r; }
static D2Pt* MSABI lb_GetEndPoint(void*s,D2Pt*r){ *r=((D2Brush*)s)->p1; return r; }
static void MSABI gb_GetStops(void*s,void**pp){ D2Brush*b=s; if(pp){ *pp=b->stops; if(b->stops) d2_AddRef(b->stops); } }
static void MSABI rb_SetCenter(void*s,D2Pt p){ ((D2Brush*)s)->p0=p; }
static void MSABI rb_SetOffset(void*s,D2Pt p){ ((D2Brush*)s)->off=p; }
static void MSABI rb_SetRadiusX(void*s,float r){ ((D2Brush*)s)->rx=r; }
static void MSABI rb_SetRadiusY(void*s,float r){ ((D2Brush*)s)->ry=r; }
static D2Pt* MSABI rb_GetCenter(void*s,D2Pt*r){ *r=((D2Brush*)s)->p0; return r; }
static D2Pt* MSABI rb_GetOffset(void*s,D2Pt*r){ *r=((D2Brush*)s)->off; return r; }
static float MSABI rb_GetRadiusX(void*s){ return ((D2Brush*)s)->rx; }
static float MSABI rb_GetRadiusY(void*s){ return ((D2Brush*)s)->ry; }
static void MSABI bb_SetExtendX(void*s,uint32_t v){ ((D2Brush*)s)->ex=v; }
static void MSABI bb_SetExtendY(void*s,uint32_t v){ ((D2Brush*)s)->ey=v; }
static void MSABI bb_SetInterp(void*s,uint32_t v){ ((D2Brush*)s)->interp=v; }
static void MSABI bb_SetBitmap(void*s,void*bm){ D2Brush*b=s; if(bm && !d2_is_bmp(bm)) return; if(bm) d2_AddRef(bm); if(b->bmp) d2_release(b->bmp); b->bmp=bm; }
static uint32_t MSABI bb_GetExtendX(void*s){ return ((D2Brush*)s)->ex; }
static uint32_t MSABI bb_GetExtendY(void*s){ return ((D2Brush*)s)->ey; }
static uint32_t MSABI bb_GetInterp(void*s){ return ((D2Brush*)s)->interp; }
static void MSABI bb_GetBitmap(void*s,void**pp){ D2Brush*b=s; if(pp){ *pp=b->bmp; if(b->bmp) d2_AddRef(b->bmp); } }
static uint32_t MSABI st_GetCount(void*s){ return (uint32_t)((D2Stops*)s)->n; }
static void MSABI st_GetStops(void*s,D2Stop*o,uint32_t n){ D2Stops*t=s; for(uint32_t i=0;o&&i<n&&i<(uint32_t)t->n;i++) o[i]=t->s[i]; }
static uint32_t MSABI st_GetGamma(void*s){ return ((D2Stops*)s)->gamma; }
static uint32_t MSABI st_GetExtend(void*s){ return ((D2Stops*)s)->extend; }
static uint32_t MSABI st_zero(void*s){ (void)s; return 0; }
static void d2_brush_slots(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)br_SetOpacity; v[5]=(void*)br_SetTransform; v[6]=(void*)br_GetOpacity; v[7]=(void*)br_GetTransform; }
static void d2_setup_solid(void**v){ d2_brush_slots(v); v[8]=(void*)sb_SetColor; v[9]=(void*)sb_GetColor; }
static void d2_setup_lin(void**v){ d2_brush_slots(v); v[8]=(void*)lb_SetStartPoint; v[9]=(void*)lb_SetEndPoint; v[10]=(void*)lb_GetStartPoint; v[11]=(void*)lb_GetEndPoint; v[12]=(void*)gb_GetStops; }
static void d2_setup_rad(void**v){ d2_brush_slots(v); v[8]=(void*)rb_SetCenter; v[9]=(void*)rb_SetOffset; v[10]=(void*)rb_SetRadiusX; v[11]=(void*)rb_SetRadiusY; v[12]=(void*)rb_GetCenter;
    v[13]=(void*)rb_GetOffset; v[14]=(void*)rb_GetRadiusX; v[15]=(void*)rb_GetRadiusY; v[16]=(void*)gb_GetStops; }
static void d2_setup_bbrush(void**v){ d2_brush_slots(v); v[8]=(void*)bb_SetExtendX; v[9]=(void*)bb_SetExtendY; v[10]=(void*)bb_SetInterp; v[11]=(void*)bb_SetBitmap; v[12]=(void*)bb_GetExtendX;
    v[13]=(void*)bb_GetExtendY; v[14]=(void*)bb_GetInterp; v[15]=(void*)bb_GetBitmap; v[16]=(void*)bb_SetInterp; v[17]=(void*)bb_GetInterp; }
static void d2_setup_stops(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)st_GetCount; v[5]=(void*)st_GetStops; v[6]=(void*)st_GetGamma; v[7]=(void*)st_GetExtend;
    v[8]=(void*)st_GetStops; v[9]=(void*)st_zero; v[10]=(void*)st_zero; v[11]=(void*)st_zero; v[12]=(void*)st_zero; }
// ---- stroke styles
typedef struct { D2Hdr h; D2StrokeProps p; float*dashes; int nd; } D2Stroke;
static void d2_stroke_dtor(void*p){ free(((D2Stroke*)p)->dashes); }
static uint32_t MSABI ss_cap0(void*s){ return ((D2Stroke*)s)->p.cap0; }
static uint32_t MSABI ss_cap1(void*s){ return ((D2Stroke*)s)->p.cap1; }
static uint32_t MSABI ss_capd(void*s){ return ((D2Stroke*)s)->p.capd; }
static float MSABI ss_miter(void*s){ return ((D2Stroke*)s)->p.miter; }
static uint32_t MSABI ss_join(void*s){ return ((D2Stroke*)s)->p.join; }
static float MSABI ss_doff(void*s){ return ((D2Stroke*)s)->p.doff; }
static uint32_t MSABI ss_dash(void*s){ return ((D2Stroke*)s)->p.dash; }
static uint32_t MSABI ss_ndash(void*s){ return (uint32_t)((D2Stroke*)s)->nd; }
static void MSABI ss_dashes(void*s,float*o,uint32_t n){ D2Stroke*t=s; for(uint32_t i=0;o&&i<n&&i<(uint32_t)t->nd;i++) o[i]=t->dashes[i]; }
static uint32_t MSABI ss_tftype(void*s){ return ((D2Stroke*)s)->p.tftype; }
static void d2_setup_stroke(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)ss_cap0; v[5]=(void*)ss_cap1; v[6]=(void*)ss_capd; v[7]=(void*)ss_miter; v[8]=(void*)ss_join;
    v[9]=(void*)ss_doff; v[10]=(void*)ss_dash; v[11]=(void*)ss_ndash; v[12]=(void*)ss_dashes; v[13]=(void*)ss_tftype; }
// ---- geometries: each holds its outline as a CGPath (in its own space)
typedef struct D2Geom { D2Hdr h; int kind; CGMutablePathRef path; uint32_t fill; D2Rect rect; D2RRect rr; D2Ell ell; D2Mat tf; struct D2Geom*src; void**kids; int nkids;
    int state, nfig, nseg; CGPoint cur; } D2Geom;   // path state: 0 empty, 1 open (sink handed out), 2 closed
enum { GK_RECT=1, GK_RRECT, GK_ELL, GK_PATH, GK_TF, GK_GROUP };
static int d2_is_geom(void*p){ D2Hdr*h=p; return h && (h->cls==&C_d2grect||h->cls==&C_d2grrect||h->cls==&C_d2gell||h->cls==&C_d2gpath||h->cls==&C_d2gtf||h->cls==&C_d2ggroup); }
static void d2_geom_dtor(void*p){ D2Geom*g=p; if(g->path) CGPathRelease(g->path); if(g->src) d2_release(g->src); for(int i=0;i<g->nkids;i++) d2_release(g->kids[i]); free(g->kids); }
static CGRect d2_cgrect(const D2Rect*r){ return CGRectStandardize(CGRectMake(r->l,r->t,r->r-r->l,r->b-r->t)); }
static void d2_add_rrect(CGMutablePathRef p,const D2RRect*r,const CGAffineTransform*m){ CGRect rc=d2_cgrect(&r->rect); CGFloat rx=fabs(r->rx), ry=fabs(r->ry);
    if(rx>rc.size.width/2) rx=rc.size.width/2; if(ry>rc.size.height/2) ry=rc.size.height/2;
    if(rx<=0||ry<=0) CGPathAddRect(p,m,rc); else CGPathAddRoundedRect(p,m,rc,rx,ry); }
static void d2_add_ell(CGMutablePathRef p,const D2Ell*e,const CGAffineTransform*m){ CGPathAddEllipseInRect(p,m,CGRectMake(e->c.x-fabs(e->rx),e->c.y-fabs(e->ry),2*fabs(e->rx),2*fabs(e->ry))); }
static D2Geom* d2_geom_new(D2Cls*c,int kind,void*fac){ D2Geom*g=d2_alloc(c,sizeof(D2Geom)); g->h.fac=fac; g->kind=kind; g->path=CGPathCreateMutable(); g->fill=1; g->tf=d2_ident(); return g; }
static int d2_geom_eo(D2Geom*g){ return g->kind==GK_PATH?g->fill==0:g->kind==GK_GROUP?g->fill==0:g->kind==GK_TF&&g->src?d2_geom_eo(g->src):0; }
// sink (ID2D1GeometrySink of a path geometry): 3 SetFillMode, 4 SetSegmentFlags, 5 BeginFigure, 6 AddLines, 7 AddBeziers, 8 EndFigure, 9 Close,
// 10 AddLine, 11 AddBezier, 12 AddQuadraticBezier, 13 AddQuadraticBeziers, 14 AddArc
typedef struct { D2Hdr h; D2Geom*g; } D2Sink;
static void d2_sink_dtor(void*p){ D2Sink*k=p; if(k->g) d2_release(k->g); }
static void MSABI sk_SetFillMode(void*s,uint32_t m){ ((D2Sink*)s)->g->fill=m; }
static void MSABI sk_SetSegmentFlags(void*s,uint32_t f){ (void)s;(void)f; }
static void MSABI sk_BeginFigure(void*s,D2Pt p,uint32_t begin){ (void)begin; D2Geom*g=((D2Sink*)s)->g; CGPathMoveToPoint(g->path,0,p.x,p.y); g->cur=CGPointMake(p.x,p.y); g->nfig++; }
static void MSABI sk_AddLines(void*s,const D2Pt*p,uint32_t n){ D2Geom*g=((D2Sink*)s)->g; for(uint32_t i=0;p&&i<n;i++){ CGPathAddLineToPoint(g->path,0,p[i].x,p[i].y); g->cur=CGPointMake(p[i].x,p[i].y); } g->nseg+=(int)n; }
static void MSABI sk_AddBeziers(void*s,const D2Pt*p,uint32_t n){ D2Geom*g=((D2Sink*)s)->g; for(uint32_t i=0;p&&i<n;i++){ const D2Pt*q=p+i*3;
        CGPathAddCurveToPoint(g->path,0,q[0].x,q[0].y,q[1].x,q[1].y,q[2].x,q[2].y); g->cur=CGPointMake(q[2].x,q[2].y); } g->nseg+=(int)n; }
static void MSABI sk_EndFigure(void*s,uint32_t end){ D2Geom*g=((D2Sink*)s)->g; if(end==1) CGPathCloseSubpath(g->path); }
static int MSABI sk_Close(void*s){ D2Geom*g=((D2Sink*)s)->g; g->state=2; return 0; }
static void MSABI sk_AddLine(void*s,D2Pt p){ sk_AddLines(s,&p,1); }
static void MSABI sk_AddBezier(void*s,const D2Pt*b){ sk_AddBeziers(s,b,1); }
static void MSABI sk_AddQuadraticBeziers(void*s,const D2Pt*p,uint32_t n){ D2Geom*g=((D2Sink*)s)->g; for(uint32_t i=0;p&&i<n;i++){ const D2Pt*q=p+i*2;
        CGPathAddQuadCurveToPoint(g->path,0,q[0].x,q[0].y,q[1].x,q[1].y); g->cur=CGPointMake(q[1].x,q[1].y); } g->nseg+=(int)n; }
static void MSABI sk_AddQuadraticBezier(void*s,const D2Pt*q){ sk_AddQuadraticBeziers(s,q,1); }
// D2D1_ARC_SEGMENT {point, size, rotationAngle (degrees), sweepDirection (1 clockwise), arcSize (1 large)}: the SVG endpoint arc
typedef struct { D2Pt p; D2SizeF sz; float rot; uint32_t sweep, large; } D2Arc;
static void MSABI sk_AddArc(void*s,const D2Arc*a){ D2Geom*g=((D2Sink*)s)->g; if(!a) return; g->nseg++;
    double x0=g->cur.x, y0=g->cur.y, x1=a->p.x, y1=a->p.y, rx=fabs(a->sz.w), ry=fabs(a->sz.h), phi=a->rot*M_PI/180.0;
    if((x0==x1&&y0==y1)) return; if(rx==0||ry==0){ CGPathAddLineToPoint(g->path,0,x1,y1); g->cur=CGPointMake(x1,y1); return; }
    double cp=cos(phi), sp=sin(phi), dx=(x0-x1)/2, dy=(y0-y1)/2, x1p=cp*dx+sp*dy, y1p=-sp*dx+cp*dy;
    double lam=(x1p*x1p)/(rx*rx)+(y1p*y1p)/(ry*ry); if(lam>1){ double k=sqrt(lam); rx*=k; ry*=k; }
    double num=rx*rx*ry*ry-rx*rx*y1p*y1p-ry*ry*x1p*x1p, den=rx*rx*y1p*y1p+ry*ry*x1p*x1p; double co=den>0?sqrt(fmax(0,num/den)):0;
    if((a->large!=0)==(a->sweep!=0)) co=-co;
    double cxp=co*rx*y1p/ry, cyp=-co*ry*x1p/rx, cx=cp*cxp-sp*cyp+(x0+x1)/2, cy=sp*cxp+cp*cyp+(y0+y1)/2;
    double ux=(x1p-cxp)/rx, uy=(y1p-cyp)/ry, vx=(-x1p-cxp)/rx, vy=(-y1p-cyp)/ry;
    double th1=atan2(uy,ux), dth=atan2(ux*vy-uy*vx,ux*vx+uy*vy);
    if(!a->sweep && dth>0) dth-=2*M_PI; else if(a->sweep && dth<0) dth+=2*M_PI;
    CGAffineTransform m=CGAffineTransformScale(CGAffineTransformRotate(CGAffineTransformMakeTranslation(cx,cy),phi),rx,ry);
    CGPathAddRelativeArc(g->path,&m,0,0,1,th1,dth); g->cur=CGPointMake(x1,y1); }
static void d2_setup_sink(void**v){ v[3]=(void*)sk_SetFillMode; v[4]=(void*)sk_SetSegmentFlags; v[5]=(void*)sk_BeginFigure; v[6]=(void*)sk_AddLines; v[7]=(void*)sk_AddBeziers;
    v[8]=(void*)sk_EndFigure; v[9]=(void*)sk_Close; v[10]=(void*)sk_AddLine; v[11]=(void*)sk_AddBezier; v[12]=(void*)sk_AddQuadraticBezier; v[13]=(void*)sk_AddQuadraticBeziers; v[14]=(void*)sk_AddArc; }
// ID2D1Geometry: 4 GetBounds, 5 GetWidenedBounds, 6 StrokeContainsPoint, 7 FillContainsPoint
static void d2_rect_out(D2Rect*o,CGRect r){ if(CGRectIsNull(r)){ o->l=o->t=INFINITY; o->r=o->b=-INFINITY; return; }   // (Direct2D's empty bounds)
    o->l=(float)CGRectGetMinX(r); o->t=(float)CGRectGetMinY(r); o->r=(float)CGRectGetMaxX(r); o->b=(float)CGRectGetMaxY(r); }
static int MSABI geo_GetBounds(void*s,const D2Mat*m,D2Rect*o){ D2Geom*g=s; if(!o) return (int)HR_INVALIDARG; CGAffineTransform t=m?d2_cgm(m):CGAffineTransformIdentity;
    CGPathRef p=CGPathCreateCopyByTransformingPath(g->path,&t); d2_rect_out(o,CGPathIsEmpty(p)?CGRectNull:CGPathGetPathBoundingBox(p)); CGPathRelease(p); return 0; }
static int MSABI geo_GetWidenedBounds(void*s,float w,void*ss,const D2Mat*m,float tol,D2Rect*o){ (void)ss;(void)tol; int hr=geo_GetBounds(s,m,o);
    if(hr>=0 && o->l<=o->r){ o->l-=w/2; o->t-=w/2; o->r+=w/2; o->b+=w/2; } return hr; }
static int MSABI geo_FillContainsPoint(void*s,D2Pt p,const D2Mat*m,float tol,int*in){ (void)tol; D2Geom*g=s; CGAffineTransform t=m?d2_cgm(m):CGAffineTransformIdentity;
    if(in)*in=CGPathContainsPoint(g->path,&t,CGPointMake(p.x,p.y),d2_geom_eo(g)); return 0; }
static int MSABI geo_StrokeContainsPoint(void*s,D2Pt p,float w,void*ss,const D2Mat*m,float tol,int*in){ (void)ss; D2Geom*g=s; CGAffineTransform t=m?d2_cgm(m):CGAffineTransformIdentity;
    CGPathRef st=CGPathCreateCopyByStrokingPath(g->path,&t,(w>0?w:1)+(tol>0?tol:0.25f),kCGLineCapButt,kCGLineJoinMiter,10); if(in)*in=CGPathContainsPoint(st,0,CGPointMake(p.x,p.y),0); CGPathRelease(st); return 0; }
static void MSABI geo_GetRect(void*s,D2Rect*o){ if(o) *o=((D2Geom*)s)->rect; }
static void MSABI geo_GetRRect(void*s,D2RRect*o){ if(o) *o=((D2Geom*)s)->rr; }
static void MSABI geo_GetEllipse(void*s,D2Ell*o){ if(o) *o=((D2Geom*)s)->ell; }
static void MSABI geo_GetSource(void*s,void**pp){ D2Geom*g=s; if(pp){ *pp=g->src; if(g->src) d2_AddRef(g->src); } }
static void MSABI geo_GetTransform(void*s,D2Mat*o){ if(o) *o=((D2Geom*)s)->tf; }
static uint32_t MSABI geo_GetFillMode(void*s){ return ((D2Geom*)s)->fill; }
static uint32_t MSABI geo_GetKidCount(void*s){ return (uint32_t)((D2Geom*)s)->nkids; }
static void MSABI geo_GetKids(void*s,void**o,uint32_t n){ D2Geom*g=s; for(uint32_t i=0;o&&i<n&&i<(uint32_t)g->nkids;i++){ o[i]=g->kids[i]; d2_addref(g->kids[i]); } }
static int MSABI geo_Open(void*s,void**pp){ D2Geom*g=s; if(g->state){ *pp=0; return (int)0x88990001; }   // D2DERR_WRONG_STATE
    g->state=1; D2Sink*k=d2_alloc(&C_d2sink,sizeof(D2Sink)); k->h.fac=g->h.fac; k->g=g; d2_AddRef(g); *pp=k; return 0; }
static int MSABI geo_GetSegmentCount(void*s,uint32_t*n){ if(n)*n=(uint32_t)((D2Geom*)s)->nseg; return 0; }
static int MSABI geo_GetFigureCount(void*s,uint32_t*n){ if(n)*n=(uint32_t)((D2Geom*)s)->nfig; return 0; }
static void d2_geo_slots(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)geo_GetBounds; v[5]=(void*)geo_GetWidenedBounds; v[6]=(void*)geo_StrokeContainsPoint; v[7]=(void*)geo_FillContainsPoint; }
static void d2_setup_grect(void**v){ d2_geo_slots(v); v[17]=(void*)geo_GetRect; }
static void d2_setup_grrect(void**v){ d2_geo_slots(v); v[17]=(void*)geo_GetRRect; }
static void d2_setup_gell(void**v){ d2_geo_slots(v); v[17]=(void*)geo_GetEllipse; }
static void d2_setup_gtf(void**v){ d2_geo_slots(v); v[17]=(void*)geo_GetSource; v[18]=(void*)geo_GetTransform; }
static void d2_setup_ggroup(void**v){ d2_geo_slots(v); v[17]=(void*)geo_GetFillMode; v[18]=(void*)geo_GetKidCount; v[19]=(void*)geo_GetKids; }
static void d2_setup_gpath(void**v){ d2_geo_slots(v); v[17]=(void*)geo_Open; v[19]=(void*)geo_GetSegmentCount; v[20]=(void*)geo_GetFigureCount; }
// ---- layers and drawing-state blocks
typedef struct { D2Hdr h; D2SizeF size; } D2Layer;
static D2SizeF* MSABI layer_GetSize(void*s,D2SizeF*r){ *r=((D2Layer*)s)->size; return r; }
static void d2_setup_layer(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)layer_GetSize; }
typedef struct { D2Hdr h; D2StateDesc d; void*params; } D2Block;
static void d2_block_dtor(void*p){ D2Block*b=p; if(b->params) d2_release(b->params); }
static void MSABI blk_GetDesc(void*s,D2StateDesc*o){ if(o) memcpy(o,&((D2Block*)s)->d,48); }
static void MSABI blk_SetDesc(void*s,const D2StateDesc*d){ if(d) memcpy(&((D2Block*)s)->d,d,48); }
static void MSABI blk_GetDesc1(void*s,D2StateDesc*o){ if(o) *o=((D2Block*)s)->d; }
static void MSABI blk_SetDesc1(void*s,const D2StateDesc*d){ if(d) ((D2Block*)s)->d=*d; }
static void MSABI blk_SetParams(void*s,void*p){ D2Block*b=s; if(p) d2_addref(p); if(b->params) d2_release(b->params); b->params=p; }
static void MSABI blk_GetParams(void*s,void**pp){ D2Block*b=s; if(pp){ *pp=b->params; if(b->params) d2_addref(b->params); } }
static void d2_setup_block(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)blk_GetDesc; v[5]=(void*)blk_SetDesc; v[6]=(void*)blk_SetParams; v[7]=(void*)blk_GetParams; v[8]=(void*)blk_GetDesc1; v[9]=(void*)blk_SetDesc1; }
// ==== the device context ======================================================================================================
typedef struct { CGRect dev; int layer, transp, mask; } D2Clip;   // dev: the device-space box it limits drawing to
typedef struct D2Ctx { D2Hdr h; void*dev; D2Bmp*target; CGContextRef cg; uint8_t*canvas; uint32_t cw,ch;
    D2Mat tf; float dpix,dpiy; uint32_t aa,taa,unit,blend; void*tparams; uint64_t tag1,tag2;
    int drawing; double dx0,dy0,dx1,dy1; int dirty; D2Clip clip[64]; int nclip; unsigned long nops;
    unsigned gltex; uint32_t gltw,glth; } D2Ctx;
static void d2_ctx_drop_canvas(D2Ctx*c){ if(c->cg && c->canvas) CGContextRelease(c->cg); free(c->canvas); c->canvas=0; c->cg=0; c->cw=c->ch=0; }
static void d2_ctx_dtor(void*p){ D2Ctx*c=p; d2_ctx_drop_canvas(c); if(c->target) d2_release(c->target); if(c->tparams) d2_release(c->tparams); if(c->dev) d2_release(c->dev); }
// the context's drawing surface for its target: the CPU bitmap's own context, or a canvas the texture's size
static CGContextRef d2_surface(D2Ctx*c){ D2Bmp*t=c->target; if(!t) return 0; if(!t->tex){ if(c->canvas) d2_ctx_drop_canvas(c); return t->cg; }
    if(!c->canvas || c->cw!=t->tex->w || c->ch!=t->tex->h){ d2_ctx_drop_canvas(c); c->cw=t->tex->w; c->ch=t->tex->h; c->canvas=calloc(1,(size_t)c->cw*c->ch*4+16);
        c->cg=CGBitmapContextCreate(c->canvas,c->cw,c->ch,8,c->cw*4,d2_srgb(),kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
        if(c->cg){ CGContextTranslateCTM(c->cg,0,c->ch); CGContextScaleCTM(c->cg,1,-1); } }
    return c->cg; }
// user space -> device pixels: the context transform, then DIPs -> pixels (unit mode DIPS)
static CGAffineTransform d2_ctm(D2Ctx*c){ CGAffineTransform m=d2_cgm(&c->tf); if(c->unit==0) m=CGAffineTransformConcat(m,CGAffineTransformMakeScale(c->dpix/96.0f,c->dpiy/96.0f)); return m; }
static CGRect d2_clipbox(D2Ctx*c){ return c->nclip?c->clip[c->nclip-1].dev:CGRectMake(0,0,c->target?(c->target->tex?c->target->tex->w:c->target->w):0,c->target?(c->target->tex?c->target->tex->h:c->target->hh):0); }
// mark the device-space box `r` (user space, transformed) as drawn on the canvas
static void d2_dirty(D2Ctx*c,CGRect local,double pad){
    if(getenv("PELOAD_TRACE_D2D_OPS")){ static int nl; if(nl++<60){ CGAffineTransform m=d2_ctm(c); CGRect cb=d2_clipbox(c); char t[300]; int k=snprintf(t,sizeof t,"  [d2d-op] local %.1f,%.1f %.1fx%.1f ctm [%.3g %.3g %.3g %.3g %.1f %.1f] clip %.0f,%.0f %.0fx%.0f canvas %p\n",
            local.origin.x,local.origin.y,local.size.width,local.size.height,m.a,m.b,m.c,m.d,m.tx,m.ty,cb.origin.x,cb.origin.y,cb.size.width,cb.size.height,(void*)c->canvas); (void)!write(2,t,k); } }
    if(!c->canvas) return; CGRect r=CGRectApplyAffineTransform(local,d2_ctm(c)); r=CGRectInset(r,-pad-1,-pad-1);
    r=CGRectIntersection(r,d2_clipbox(c)); if(CGRectIsNull(r)||CGRectIsEmpty(r)) return;
    if(!c->dirty){ c->dx0=CGRectGetMinX(r); c->dy0=CGRectGetMinY(r); c->dx1=CGRectGetMaxX(r); c->dy1=CGRectGetMaxY(r); c->dirty=1; return; }
    if(CGRectGetMinX(r)<c->dx0) c->dx0=CGRectGetMinX(r); if(CGRectGetMinY(r)<c->dy0) c->dy0=CGRectGetMinY(r); if(CGRectGetMaxX(r)>c->dx1) c->dx1=CGRectGetMaxX(r); if(CGRectGetMaxY(r)>c->dy1) c->dy1=CGRectGetMaxY(r); }
// one drawing operation: the surface with the user transform applied (balanced by d2_op_end)
static CGContextRef d2_op(D2Ctx*c){ CGContextRef cg=d2_surface(c); if(!cg) return 0; c->nops++; CGContextSaveGState(cg); CGContextConcatCTM(cg,d2_ctm(c));
    CGContextSetShouldAntialias(cg,c->aa==0); CGContextSetAllowsFontSmoothing(cg,false); CGContextSetShouldSmoothFonts(cg,false);
    if(c->blend==1) CGContextSetBlendMode(cg,kCGBlendModeCopy); else if(c->blend==3) CGContextSetBlendMode(cg,kCGBlendModePlusLighter); return cg; }
static void d2_op_end(D2Ctx*c,CGContextRef cg){ (void)c; CGContextRestoreGState(cg); if(c->target && !c->target->tex) c->target->ver++; }
// paint a non-solid brush over the current clip (the shape / glyphs were made the clip)
static void d2_paint(CGContextRef cg,D2Brush*b,CGRect area){
    CGContextSetAlpha(cg,b->opacity); CGContextConcatCTM(cg,d2_cgm(&b->tf));
    if(b->kind==BR_LINEAR && b->stops){ CGContextDrawLinearGradient(cg,d2_gradient(b->stops),CGPointMake(b->p0.x,b->p0.y),CGPointMake(b->p1.x,b->p1.y),kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation); }
    else if(b->kind==BR_RADIAL && b->stops && b->rx>0 && b->ry>0){ CGContextTranslateCTM(cg,b->p0.x,b->p0.y); CGContextScaleCTM(cg,1,b->ry/b->rx);
        CGContextDrawRadialGradient(cg,d2_gradient(b->stops),CGPointMake(b->off.x,b->off.y*b->rx/b->ry),0,CGPointMake(0,0),b->rx,kCGGradientDrawsBeforeStartLocation|kCGGradientDrawsAfterEndLocation); }
    else if(b->kind==BR_BITMAP && b->bmp){ CGImageRef im=d2_bmp_image(b->bmp); if(im){ D2Bmp*bm=b->bmp; double w=bm->w*96.0/bm->dpix, h=bm->hh*96.0/bm->dpiy;
            CGContextSetInterpolationQuality(cg,b->interp==0?kCGInterpolationNone:kCGInterpolationDefault);
            if(b->ex==0 && b->ey==0){ CGContextTranslateCTM(cg,0,h); CGContextScaleCTM(cg,1,-1); CGContextDrawImage(cg,CGRectMake(0,0,w,h),im); }
            else { CGContextScaleCTM(cg,1,-1); CGContextDrawTiledImage(cg,CGRectMake(0,0,w,h),im); }   // (wrap / mirror: tiled; the tile phase is mirrored vertically)
            CGImageRelease(im); } }
    (void)area; }
static void d2_fill_color(CGContextRef cg,D2Brush*b){ if(b && b->kind==BR_SOLID) CGContextSetRGBFillColor(cg,b->col.r,b->col.g,b->col.b,b->col.a*b->opacity); }
// fill / stroke the path made current on cg
static void d2_fillpath(D2Ctx*c,CGContextRef cg,D2Brush*b,int eo,CGRect bounds){
    if(!b) return; if(b->kind==BR_SOLID){ d2_fill_color(cg,b); if(eo) CGContextEOFillPath(cg); else CGContextFillPath(cg); }
    else { if(eo) CGContextEOClip(cg); else CGContextClip(cg); d2_paint(cg,b,bounds); } (void)c; }
static void d2_strokestyle(CGContextRef cg,float w,D2Stroke*ss){
    CGContextSetLineWidth(cg,w); CGLineCap cap=kCGLineCapButt; CGLineJoin join=kCGLineJoinMiter; float miter=10;
    if(ss){ uint32_t k=ss->p.cap0; cap=k==1?kCGLineCapSquare:(k==2||k==3)?kCGLineCapRound:kCGLineCapButt; join=ss->p.join==1?kCGLineJoinBevel:ss->p.join==2?kCGLineJoinRound:kCGLineJoinMiter;
        miter=ss->p.miter>=1?ss->p.miter:1;
        static const CGFloat dash[]={2,2}, dot[]={0,2}, dd[]={2,2,0,2}, ddd[]={2,2,0,2,0,2}; const CGFloat*pat=0; int n=0; CGFloat buf[32];
        switch(ss->p.dash){ case 1: pat=dash; n=2; break; case 2: pat=dot; n=2; break; case 3: pat=dd; n=4; break; case 4: pat=ddd; n=6; break;
            case 5: n=ss->nd<32?ss->nd:32; for(int i=0;i<n;i++) buf[i]=ss->dashes[i]; pat=buf; break; }
        if(n){ CGFloat sc[32]; for(int i=0;i<n;i++) sc[i]=pat[i]*w; CGContextSetLineDash(cg,ss->p.doff*w,sc,(size_t)n);
            if(ss->p.dash==2||ss->p.capd==2) cap=kCGLineCapRound; } }
    else miter=10;
    CGContextSetLineCap(cg,cap); CGContextSetLineJoin(cg,join); CGContextSetMiterLimit(cg,miter); }
static void d2_strokepath(D2Ctx*c,CGContextRef cg,D2Brush*b,float w,D2Stroke*ss,CGRect bounds){
    if(!b) return; d2_strokestyle(cg,w,ss);
    if(b->kind==BR_SOLID){ CGContextSetRGBStrokeColor(cg,b->col.r,b->col.g,b->col.b,b->col.a*b->opacity); CGContextStrokePath(cg); }
    else { CGContextReplacePathWithStrokedPath(cg); CGContextClip(cg); d2_paint(cg,b,bounds); } (void)c; }
// ---- GL: the canvas onto the target texture, texture quads, clears ----------------------------------------------------------------
static unsigned g_d2_prog, g_d2_vao, g_d2_samp[2]; static int g_d2_lpu=-1, g_d2_lop=-1, g_d2_lt=-1;
static void d2gl_init(void){ if(g_d2_prog) return;
    const char*vs="#version 410 core\nuniform vec4 pu[4]; out vec2 uv; void main(){ vec4 p=pu[gl_VertexID]; uv=p.zw; gl_Position=vec4(p.xy,0.0,1.0); }\n";
    const char*fs="#version 410 core\nuniform sampler2D t; uniform vec4 op; in vec2 uv; out vec4 o; void main(){ vec4 c=texture(t,uv);"
        " if(op.y>1.5) c.a=1.0; else if(op.y>0.5) c.rgb*=c.a; o=c*op.x; }\n";
    unsigned v=G.CreateShader(GLE_VS), f=G.CreateShader(GLE_FS); G.ShaderSource(v,1,&vs,0); G.CompileShader(v); G.ShaderSource(f,1,&fs,0); G.CompileShader(f);
    unsigned p=G.CreateProgram(); G.AttachShader(p,v); G.AttachShader(p,f); G.LinkProgram(p); G.DeleteShader(v); G.DeleteShader(f);
    g_d2_lpu=G.GetUniformLocation(p,"pu"); g_d2_lop=G.GetUniformLocation(p,"op"); g_d2_lt=G.GetUniformLocation(p,"t");
    G.GenVertexArrays(1,&g_d2_vao);
    if(G.GenSamplers){ G.GenSamplers(2,g_d2_samp); for(int i=0;i<2;i++){ unsigned s=g_d2_samp[i]; unsigned flt=i?0x2601/*LINEAR*/:0x2600/*NEAREST*/;
            G.SamplerParameteri(s,0x2801,(int)flt); G.SamplerParameteri(s,0x2800,(int)flt); G.SamplerParameteri(s,0x2802,0x812F); G.SamplerParameteri(s,0x2803,0x812F);
            G.SamplerParameteri(s,0x8A48/*TEXTURE_SRGB_DECODE*/,0x8A4A/*SKIP_DECODE*/); } }   // Direct2D draws the stored bytes (UNORM views)
    g_d2_prog=p; }
// state for drawing into target texture rt (g_glmx held, context current)
static void d2gl_begin(D3DObj*rt){ d2gl_init(); d3dgl_ensure_rt(rt); gl_bindfbo_any(GLE_FBO,rt->glfbo); d3dgl_reset_raster_ops();
    gs_viewport(0,0,(int)rt->w,(int)rt->h); gs_useprog(0); G.BindProgramPipeline(0); g_gs.pipe=0; G.UseProgram(g_d2_prog); g_gs.prog=g_d2_prog; G.BindVertexArray(g_d2_vao);
    TEXCACHE_INVALIDATE(); G.ActiveTexture(GLE_TEX0); g_active_unit=0; }
static void d2gl_end(void){ gs_cap(0x0BE2,0); gs_cap(0x0C11,0); G.BindVertexArray(g_d3dgl_vao); if(G.BindSampler) G.BindSampler(0,0); gl_bindfbo_any(GLE_FBO,0); gs_invalidate(); TEXCACHE_INVALIDATE(); }
static void d2gl_scissor(D3DObj*rt,CGRect r){ r=CGRectIntegral(CGRectIntersection(r,CGRectMake(0,0,rt->w,rt->h))); if(CGRectIsNull(r)) r=CGRectZero;
    gs_cap(0x0C11,1); gs_scissor((int)r.origin.x,d3dgl_fbo_y(rt->h,(uint32_t)r.origin.y,(uint32_t)r.size.height),(int)r.size.width,(int)r.size.height); }
// a quad: device corners (pixels, y down) q[4] = top-left, top-right, bottom-left, bottom-right; uv likewise
static void d2gl_quad(D3DObj*rt,const CGPoint q[4],const CGPoint uv[4],float opacity,int mode){ float pu[16];
    for(int i=0;i<4;i++){ pu[i*4]=(float)(q[i].x/rt->w*2-1); double y=q[i].y/rt->h*2-1; pu[i*4+1]=(float)(g_d3dgl_flipy?y:-y); pu[i*4+2]=(float)uv[i].x; pu[i*4+3]=(float)uv[i].y; }
    float op[4]={opacity,(float)mode,0,0}; G.ProgramUniform4fv(g_d2_prog,g_d2_lpu,4,pu); G.ProgramUniform4fv(g_d2_prog,g_d2_lop,1,op); G.ProgramUniform1i(g_d2_prog,g_d2_lt,0);
    gs_cap(0x0BE2,1); gs_blendfunc(1,0x0303,1,0x0303); gs_blendeq(0x8006,0x8006); G.DrawArrays(5,0,4); }
static void d3dgl_latch_present(D3DObj*rt,int vw,int vh,int nattr);
// the drawn part of the canvas -> the target texture (premultiplied source-over), then that part of the canvas is cleared
static void d2_flush(D2Ctx*c){
    for(int i=0;i<c->nclip;i++) if(c->clip[i].transp) return;   // (inside an opacity layer its content is not on the canvas yet: at the pop)
    D2Bmp*t=c->target; if(!c->dirty || !t || !t->tex || !c->canvas){ c->dirty=0; return; } D3DObj*rt=t->tex;
    int x0=(int)floor(c->dx0), y0=(int)floor(c->dy0), x1=(int)ceil(c->dx1), y1=(int)ceil(c->dy1); c->dirty=0;
    if(x0<0) x0=0; if(y0<0) y0=0; if(x1>(int)c->cw) x1=(int)c->cw; if(y1>(int)c->ch) y1=(int)c->ch; if(x1<=x0||y1<=y0) return;
    int w=x1-x0, h=y1-y0; uint8_t*pk=malloc((size_t)w*h*4); for(int y=0;y<h;y++) memcpy(pk+(size_t)y*w*4,c->canvas+((size_t)(y0+y)*c->cw+x0)*4,(size_t)w*4);
    if(d3dgl_enabled()){ if(sub_on()) sub_drain(); GLLOCK();
        if(d3dgl_ensure()){ static void(*texsub)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*); if(!texsub) texsub=dlsym(RTLD_DEFAULT,"glTexSubImage2D");
            d2gl_begin(rt);
            if(!c->gltex || c->gltw!=c->cw || c->glth!=c->ch){ if(!c->gltex) G.GenTextures(1,&c->gltex); G.BindTexture(GLE_TEX2D,c->gltex);
                G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,(int)c->cw,(int)c->ch,0,GLE_BGRA,GLE_UI8REV,0); G.TexParameteri(GLE_TEX2D,0x813D,0); c->gltw=c->cw; c->glth=c->ch; }
            G.BindTexture(GLE_TEX2D,c->gltex); if(texsub) texsub(GLE_TEX2D,0,x0,y0,w,h,GLE_BGRA,GLE_UI8REV,pk); if(G.BindSampler) G.BindSampler(0,g_d2_samp[0]);
            d2gl_scissor(rt,CGRectMake(x0,y0,w,h));
            CGPoint q[4]={{0,0},{c->cw,0},{0,c->ch},{c->cw,c->ch}}, uv[4]={{0,0},{1,0},{0,1},{1,1}}; d2gl_quad(rt,q,uv,1.0f,0);
            d2gl_end(); if(rt==g_d3d_backbuf) d3dgl_latch_present(rt,(int)rt->w,(int)rt->h,0);
            if(d2_trace()){ static int nl; if(nl++<20){ char m[160]; int k=snprintf(m,sizeof m,"  [d2d] composite %dx%d at %d,%d onto %ux%u f%u%s\n",w,h,x0,y0,rt->w,rt->h,rt->fmt,rt==g_d3d_backbuf?" (back buffer)":""); (void)!write(2,m,k); } } }
        GLUNLOCK(); }
    free(pk);
    for(int y=y0;y<y1;y++) memset(c->canvas+((size_t)y*c->cw+x0)*4,0,(size_t)w*4); }
// can the next operation go straight to the GPU? (a texture target, no opacity layer, no geometric mask in the clip stack)
static int d2_gpu_ok(D2Ctx*c){ if(!c->target||!c->target->tex||!d3dgl_enabled()) return 0; for(int i=0;i<c->nclip;i++) if(c->clip[i].transp||c->clip[i].mask) return 0; return 1; }
// ---- ID2D1RenderTarget / ID2D1DeviceContext methods ----------------------------------------------------------------------
static D2Ctx* d2_ctx_new(void*fac,void*dev){ D2Ctx*c=d2_alloc(&C_d2ctx,sizeof(D2Ctx)); c->h.fac=fac; c->dev=dev; if(dev) d2_AddRef(dev); c->tf=d2_ident(); c->dpix=c->dpiy=96; return c; }
static D2Bmp* d2_bmp_new(void*fac){ D2Bmp*b=d2_alloc(&C_d2bmp,sizeof(D2Bmp)); b->h.fac=fac; b->dpix=b->dpiy=96; b->fmt=87; b->alpha=1; return b; }
static int MSABI d2c_CreateBitmap1(void*s,D2SizeU sz,const void*src,uint32_t pitch,const D2BmpProps1*p,void**pp){ D2Ctx*c=s; if(!pp) return (int)HR_INVALIDARG;
    if(!sz.w||!sz.h||sz.w>16384||sz.h>16384){ *pp=0; return (int)HR_INVALIDARG; }
    D2Bmp*b=d2_bmp_new(c->h.fac); if(p){ b->fmt=p->pf.fmt?p->pf.fmt:87; b->alpha=p->pf.alpha?p->pf.alpha:1; b->dpix=d2_dpi(p->dpix); b->dpiy=d2_dpi(p->dpiy); b->opts=p->opts; }
    d2_bmp_cpu(b,sz.w,sz.h,(b->opts&1)!=0); if(src) d2_bmp_put(b,0,0,sz.w,sz.h,src,pitch,(int)b->fmt);
    if(d2_trace()){ char m[120]; int k=snprintf(m,sizeof m,"  [d2d] CreateBitmap %ux%u fmt %u opts %u%s\n",sz.w,sz.h,b->fmt,b->opts,src?" (with pixels)":""); (void)!write(2,m,k); }
    *pp=b; return 0; }
static int MSABI d2c_CreateBitmap(void*s,D2SizeU sz,const void*src,uint32_t pitch,const D2BmpProps*p,void**pp){ D2BmpProps1 p1={{87,1},96,96,0,0}; if(p){ p1.pf=p->pf; p1.dpix=p->dpix; p1.dpiy=p->dpiy; }
    return d2c_CreateBitmap1(s,sz,src,pitch,&p1,pp); }
static int MSABI d2c_CreateBitmapFromWic1(void*s,void*src,const D2BmpProps1*p,void**pp){ D2Ctx*c=s; if(!pp||!src) return (int)HR_INVALIDARG;
    uint32_t w,h; uint8_t*px=wic_pull(src,WK_PBGRA,&w,&h); if(!px){ *pp=0; return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    D2Bmp*b=d2_bmp_new(c->h.fac); if(p){ b->dpix=d2_dpi(p->dpix); b->dpiy=d2_dpi(p->dpiy); b->opts=p->opts; if(p->pf.alpha) b->alpha=p->pf.alpha; }
    d2_bmp_cpu(b,w,h,(b->opts&1)!=0); memcpy(b->px,px,(size_t)w*h*4); free(px); b->ver++; *pp=b;
    if(d2_trace()){ char m[100]; int k=snprintf(m,sizeof m,"  [d2d] CreateBitmapFromWicBitmap %ux%u\n",w,h); (void)!write(2,m,k); } return 0; }
static int MSABI d2c_CreateBitmapFromWic(void*s,void*src,const D2BmpProps*p,void**pp){ D2BmpProps1 p1={{87,1},96,96,0,0}; if(p){ p1.pf=p->pf; p1.dpix=p->dpix; p1.dpiy=p->dpiy; }
    return d2c_CreateBitmapFromWic1(s,src,&p1,pp); }
static int MSABI d2c_CreateBitmapFromDxgiSurface(void*s,void*surf,const D2BmpProps1*p,void**pp){ D2Ctx*c=s; if(!pp) return (int)HR_INVALIDARG; D3DObj*t=d2_as_tex(surf);
    if(!t){ *pp=0; static int nl; if(nl++<8){ char m[120]; int k=snprintf(m,sizeof m,"  [d2d] CreateBitmapFromDxgiSurface: %p is not a 2D texture\n",surf); (void)!write(2,m,k); } return (int)HR_INVALIDARG; }
    D2Bmp*b=d2_bmp_new(c->h.fac); b->tex=t; obj_AddRef(t); b->w=t->w; b->hh=t->h; b->fmt=t->fmt;
    if(p){ if(p->pf.fmt) b->fmt=p->pf.fmt; if(p->pf.alpha) b->alpha=p->pf.alpha; b->dpix=d2_dpi(p->dpix); b->dpiy=d2_dpi(p->dpiy); b->opts=p->opts; }
    if(d2_trace()){ char m[160]; int k=snprintf(m,sizeof m,"  [d2d] CreateBitmapFromDxgiSurface %ux%u f%u alpha %u opts %u%s\n",t->w,t->h,t->fmt,b->alpha,b->opts,t==g_d3d_backbuf?" (back buffer)":""); (void)!write(2,m,k); }
    *pp=b; return 0; }
static int MSABI d2c_CreateSharedBitmap(void*s,const GUID_*iid,void*data,const D2BmpProps*p,void**pp){ (void)iid; D2Ctx*c=s;
    if(d2_is_bmp(data)){ d2_AddRef(data); *pp=data; return 0; }
    D2BmpProps1 p1={{0,1},96,96,0,0}; if(p){ p1.pf=p->pf; p1.dpix=p->dpix; p1.dpiy=p->dpiy; } return d2c_CreateBitmapFromDxgiSurface(c,data,&p1,pp); }
static int MSABI d2c_CreateSolidColorBrush(void*s,const D2Col*col,const D2BrushProps*bp,void**pp){ D2Ctx*c=s; D2Brush*b=d2_brush_new(&C_d2solid,BR_SOLID,bp,c->h.fac); if(col) b->col=*col; *pp=b; return 0; }
static int MSABI d2c_CreateGradientStops(void*s,const D2Stop*st,uint32_t n,uint32_t gamma,uint32_t ext,void**pp){ D2Ctx*c=s; D2Stops*g=d2_alloc(&C_d2stops,sizeof(D2Stops)); g->h.fac=c->h.fac;
    g->s=malloc(sizeof(D2Stop)*(n?n:1)); if(st&&n) memcpy(g->s,st,sizeof(D2Stop)*n); g->n=(int)n; g->gamma=gamma; g->extend=ext;
    for(int i=1;i<g->n;i++) for(int j=i;j>0 && g->s[j].pos<g->s[j-1].pos;j--){ D2Stop t=g->s[j]; g->s[j]=g->s[j-1]; g->s[j-1]=t; } *pp=g; return 0; }
static int MSABI d2c_CreateGradientStops1(void*s,const D2Stop*st,uint32_t n,uint32_t pre,uint32_t post,uint32_t prec,uint32_t ext,uint32_t im,void**pp){ (void)pre;(void)post;(void)prec;(void)im;
    return d2c_CreateGradientStops(s,st,n,0,ext,pp); }
static int MSABI d2c_CreateLinearGradientBrush(void*s,const D2Pt*lp,const D2BrushProps*bp,void*stops,void**pp){ D2Ctx*c=s; D2Brush*b=d2_brush_new(&C_d2lin,BR_LINEAR,bp,c->h.fac);
    if(lp){ b->p0=lp[0]; b->p1=lp[1]; } if(stops && ((D2Hdr*)stops)->cls==&C_d2stops){ b->stops=stops; d2_AddRef(stops); } *pp=b; return 0; }
static int MSABI d2c_CreateRadialGradientBrush(void*s,const float*rp,const D2BrushProps*bp,void*stops,void**pp){ D2Ctx*c=s; D2Brush*b=d2_brush_new(&C_d2rad,BR_RADIAL,bp,c->h.fac);
    if(rp){ b->p0.x=rp[0]; b->p0.y=rp[1]; b->off.x=rp[2]; b->off.y=rp[3]; b->rx=rp[4]; b->ry=rp[5]; } if(stops && ((D2Hdr*)stops)->cls==&C_d2stops){ b->stops=stops; d2_AddRef(stops); } *pp=b; return 0; }
static int MSABI d2c_CreateBitmapBrush(void*s,void*bmp,const uint32_t*bbp,const D2BrushProps*bp,void**pp){ D2Ctx*c=s; D2Brush*b=d2_brush_new(&C_d2bbrush,BR_BITMAP,bp,c->h.fac);
    if(bbp){ b->ex=bbp[0]; b->ey=bbp[1]; b->interp=bbp[2]; } else b->interp=1; if(d2_is_bmp(bmp)){ b->bmp=bmp; d2_AddRef(bmp); } *pp=b; return 0; }
static int MSABI d2c_CreateLayer(void*s,const D2SizeF*sz,void**pp){ D2Ctx*c=s; D2Layer*l=d2_alloc(&C_d2layer,sizeof(D2Layer)); l->h.fac=c->h.fac; if(sz) l->size=*sz; *pp=l; return 0; }
static void MSABI d2c_DrawLine(void*s,D2Pt p0,D2Pt p1,void*br,float w,void*ss){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!b||!(cg=d2_op(c))) return;
    CGContextMoveToPoint(cg,p0.x,p0.y); CGContextAddLineToPoint(cg,p1.x,p1.y); CGRect bb=CGRectStandardize(CGRectMake(p0.x,p0.y,p1.x-p0.x,p1.y-p0.y));
    d2_strokepath(c,cg,b,w,(ss&&((D2Hdr*)ss)->cls==&C_d2stroke)?ss:0,bb); d2_op_end(c,cg); d2_dirty(c,bb,w*2+2); }
static void MSABI d2c_DrawRectangle(void*s,const D2Rect*r,void*br,float w,void*ss){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!r||!b||!(cg=d2_op(c))) return;
    CGRect rc=d2_cgrect(r); CGContextAddRect(cg,rc); d2_strokepath(c,cg,b,w,(ss&&((D2Hdr*)ss)->cls==&C_d2stroke)?ss:0,rc); d2_op_end(c,cg); d2_dirty(c,rc,w*2+2); }
static void MSABI d2c_FillRectangle(void*s,const D2Rect*r,void*br){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!r||!b||!(cg=d2_op(c))) return;
    CGRect rc=d2_cgrect(r); CGContextAddRect(cg,rc); d2_fillpath(c,cg,b,0,rc); d2_op_end(c,cg); d2_dirty(c,rc,1); }
static void MSABI d2c_DrawRoundedRectangle(void*s,const D2RRect*r,void*br,float w,void*ss){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!r||!b||!(cg=d2_op(c))) return;
    CGMutablePathRef p=CGPathCreateMutable(); d2_add_rrect(p,r,0); CGContextAddPath(cg,p); CGPathRelease(p); CGRect rc=d2_cgrect(&r->rect);
    d2_strokepath(c,cg,b,w,(ss&&((D2Hdr*)ss)->cls==&C_d2stroke)?ss:0,rc); d2_op_end(c,cg); d2_dirty(c,rc,w*2+2); }
static void MSABI d2c_FillRoundedRectangle(void*s,const D2RRect*r,void*br){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!r||!b||!(cg=d2_op(c))) return;
    CGMutablePathRef p=CGPathCreateMutable(); d2_add_rrect(p,r,0); CGContextAddPath(cg,p); CGPathRelease(p); CGRect rc=d2_cgrect(&r->rect);
    d2_fillpath(c,cg,b,0,rc); d2_op_end(c,cg); d2_dirty(c,rc,1); }
static void MSABI d2c_DrawEllipse(void*s,const D2Ell*e,void*br,float w,void*ss){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!e||!b||!(cg=d2_op(c))) return;
    CGMutablePathRef p=CGPathCreateMutable(); d2_add_ell(p,e,0); CGContextAddPath(cg,p); CGRect rc=CGPathGetBoundingBox(p); CGPathRelease(p);
    d2_strokepath(c,cg,b,w,(ss&&((D2Hdr*)ss)->cls==&C_d2stroke)?ss:0,rc); d2_op_end(c,cg); d2_dirty(c,rc,w*2+2); }
static void MSABI d2c_FillEllipse(void*s,const D2Ell*e,void*br){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!e||!b||!(cg=d2_op(c))) return;
    CGMutablePathRef p=CGPathCreateMutable(); d2_add_ell(p,e,0); CGContextAddPath(cg,p); CGRect rc=CGPathGetBoundingBox(p); CGPathRelease(p);
    d2_fillpath(c,cg,b,0,rc); d2_op_end(c,cg); d2_dirty(c,rc,1); }
static void MSABI d2c_DrawGeometry(void*s,void*geo,void*br,float w,void*ss){ D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!d2_is_geom(geo)||!b||!(cg=d2_op(c))) return;
    D2Geom*g=geo; CGContextAddPath(cg,g->path); CGRect rc=CGPathIsEmpty(g->path)?CGRectZero:CGPathGetPathBoundingBox(g->path);
    d2_strokepath(c,cg,b,w,(ss&&((D2Hdr*)ss)->cls==&C_d2stroke)?ss:0,rc); d2_op_end(c,cg); d2_dirty(c,rc,w*2+2); }
static void MSABI d2c_FillGeometry(void*s,void*geo,void*br,void*opbr){ (void)opbr; D2Ctx*c=s; D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!d2_is_geom(geo)||!b||!(cg=d2_op(c))) return;
    D2Geom*g=geo; if(CGPathIsEmpty(g->path)){ d2_op_end(c,cg); return; } CGContextAddPath(cg,g->path); CGRect rc=CGPathGetPathBoundingBox(g->path);
    d2_fillpath(c,cg,b,d2_geom_eo(g),rc); d2_op_end(c,cg); d2_dirty(c,rc,1); }
// DrawBitmap: a texture bitmap goes on the GPU when nothing in the clip stack needs the CPU; everything else onto the canvas
static void d2_drawbitmap(D2Ctx*c,void*bm,const D2Rect*dst,float opacity,uint32_t interp,const D2Rect*src){
    if(!d2_is_bmp(bm) || !c->target) return; D2Bmp*b=bm; double bw=b->w*96.0/b->dpix, bh=b->hh*96.0/b->dpiy;
    CGRect d=dst?d2_cgrect(dst):CGRectMake(0,0,bw,bh), sr=src?d2_cgrect(src):CGRectMake(0,0,bw,bh); if(d.size.width<=0||d.size.height<=0||sr.size.width<=0||sr.size.height<=0) return;
    // a big CPU bitmap (Gravemind's 2560x1280 menu video, rewritten every video frame) goes the same way: CoreGraphics scaling it
    // onto the canvas every frame took most of a 15 ms frame; its GL copy is refreshed only when the pixels changed
    int bigcpu = !b->tex && b->px && (uint64_t)b->w*b->hh>=65536 && !getenv("PELOAD_D2D_CPU_BITMAPS");
    if((b->tex ? b->tex!=c->target->tex : bigcpu) && d2_gpu_ok(c)){
        d2_flush(c); D3DObj*rt=c->target->tex, *tx=b->tex; CGAffineTransform m=d2_ctm(c); c->nops++;
        CGPoint q[4]={CGPointApplyAffineTransform(CGPointMake(CGRectGetMinX(d),CGRectGetMinY(d)),m),CGPointApplyAffineTransform(CGPointMake(CGRectGetMaxX(d),CGRectGetMinY(d)),m),
            CGPointApplyAffineTransform(CGPointMake(CGRectGetMinX(d),CGRectGetMaxY(d)),m),CGPointApplyAffineTransform(CGPointMake(CGRectGetMaxX(d),CGRectGetMaxY(d)),m)};
        double u0=sr.origin.x/bw, v0=sr.origin.y/bh, u1=CGRectGetMaxX(sr)/bw, v1=CGRectGetMaxY(sr)/bh; CGPoint uv[4]={{u0,v0},{u1,v0},{u0,v1},{u1,v1}};
        if(sub_on()) sub_drain(); GLLOCK();
        if(d3dgl_ensure()){ unsigned gt=0; int mode;
            if(tx){ d3dgl_ensure_tex(tx); gt=tx->gltex; mode=(b->alpha==3||tx->fmt==88||tx->fmt==93)?2:b->alpha==2?1:0; }
            else { static void(*texsub)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*); if(!texsub) texsub=dlsym(RTLD_DEFAULT,"glTexSubImage2D");
                TEXCACHE_INVALIDATE(); if(!b->gltex){ G.GenTextures(1,&b->gltex); b->glver=0; }
                G.BindTexture(GLE_TEX2D,b->gltex);
                if(b->gltw!=b->w || b->glth!=b->hh){ G.TexImage2D(GLE_TEX2D,0,GLE_RGBA8,(int)b->w,(int)b->hh,0,GLE_BGRA,GLE_UI8REV,b->px); G.TexParameteri(GLE_TEX2D,0x813D,0); b->gltw=b->w; b->glth=b->hh; b->glver=b->ver+1; }
                else if(b->glver!=b->ver+1 && texsub){ texsub(GLE_TEX2D,0,0,0,(int)b->w,(int)b->hh,GLE_BGRA,GLE_UI8REV,b->px); b->glver=b->ver+1; }
                gt=b->gltex; mode=b->alpha==3?2:0; }   // (CPU bitmaps hold premultiplied BGRA)
            d2gl_begin(rt); G.BindTexture(GLE_TEX2D,gt); if(G.BindSampler) G.BindSampler(0,g_d2_samp[interp==0?0:1]);
            d2gl_scissor(rt,d2_clipbox(c)); d2gl_quad(rt,q,uv,opacity,mode); d2gl_end(); if(rt==g_d3d_backbuf) d3dgl_latch_present(rt,(int)rt->w,(int)rt->h,0); }
        GLUNLOCK(); return; }
    CGContextRef cg=d2_op(c); if(!cg) return; CGImageRef im=d2_bmp_image(b);
    if(im){ CGImageRef sub=im; CGRect px=CGRectMake(sr.origin.x*b->dpix/96.0,sr.origin.y*b->dpiy/96.0,sr.size.width*b->dpix/96.0,sr.size.height*b->dpiy/96.0);
        if(src) sub=CGImageCreateWithImageInRect(im,px);
        CGContextSetAlpha(cg,opacity); CGContextSetInterpolationQuality(cg,interp==0?kCGInterpolationNone:interp>=2?kCGInterpolationHigh:kCGInterpolationDefault);
        CGContextTranslateCTM(cg,d.origin.x,CGRectGetMaxY(d)); CGContextScaleCTM(cg,1,-1); if(sub) CGContextDrawImage(cg,CGRectMake(0,0,d.size.width,d.size.height),sub);
        if(sub && sub!=im) CGImageRelease(sub); CGImageRelease(im); }
    d2_op_end(c,cg); d2_dirty(c,d,1); }
static void MSABI d2c_DrawBitmap(void*s,void*bm,const D2Rect*dst,float op,uint32_t interp,const D2Rect*src){ d2_drawbitmap(s,bm,dst,op,interp,src); }
static void MSABI d2c_DrawBitmap1(void*s,void*bm,const D2Rect*dst,float op,uint32_t interp,const D2Rect*src,const void*persp){ (void)persp;
    d2_drawbitmap(s,bm,dst,op,interp==0?0:interp==1?1:2,src); }
static void MSABI d2c_DrawImage(void*s,void*img,const D2Pt*off,const D2Rect*ir,uint32_t interp,uint32_t comp){ (void)comp; D2Ctx*c=s;
    if(!d2_is_bmp(img)){ d2_unimpl(s,83); return; } D2Bmp*b=img; double bw=b->w*96.0/b->dpix, bh=b->hh*96.0/b->dpiy; D2Rect sr=ir?*ir:(D2Rect){0,0,(float)bw,(float)bh};
    D2Rect d={(off?off->x:0),(off?off->y:0),(off?off->x:0)+(sr.r-sr.l),(off?off->y:0)+(sr.b-sr.t)}; d2_drawbitmap(c,img,&d,1.0f,interp==0?0:interp==1?1:2,&sr); }
// text: a solid brush fills the glyphs; any other brush paints through them
static void d2_drawlayout(D2Ctx*c,DwLayout*L,double ox,double oy,void*br,uint32_t opts){
    D2Brush*b=d2_as_brush(br); CGContextRef cg; if(!b||!(cg=d2_op(c))) return; dw_build(L);
    CGContextSetShouldAntialias(cg,c->taa!=3);
    double x0=1e30,x1=-1e30,y0=1e30,y1=-1e30; for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; if(ln->x<x0) x0=ln->x; if(ln->x+ln->w>x1) x1=ln->x+ln->w; if(ln->top<y0) y0=ln->top; if(ln->top+ln->h>y1) y1=ln->top+ln->h; }
    if(x1<x0){ d2_op_end(c,cg); return; } CGRect bb=CGRectMake(ox+x0,oy+y0,x1-x0,y1-y0); double pad=L->size*0.5+2;
    if(opts&2) CGContextClipToRect(cg,CGRectMake(ox,oy,L->maxw,L->maxh));
    if(b->kind==BR_SOLID){ d2_fill_color(cg,b); dw_draw(cg,L,ox,oy,0); } else { dw_draw(cg,L,ox,oy,1); d2_paint(cg,b,bb); }
    d2_op_end(c,cg); d2_dirty(c,bb,pad); }
static void MSABI d2c_DrawTextLayout(void*s,D2Pt o,void*layout,void*br,uint32_t opts){ if(!dw_is_layout(layout)) return; d2_drawlayout(s,layout,o.x,o.y,br,opts); }
static void MSABI d2c_DrawText(void*s,const uint16_t*str,uint32_t len,void*fmt,const D2Rect*r,void*br,uint32_t opts,uint32_t mm){ (void)mm;
    if(!dw_is_fmt(fmt)||!r) return; void*L=0; if(dwfac_CreateTextLayout(0,str,len,fmt,r->r-r->l,r->b-r->t,&L)<0||!L) return;
    d2_drawlayout(s,L,r->l,r->t,br,opts); d2_Release(L); }
static void MSABI d2c_SetTransform(void*s,const D2Mat*m){
    if(getenv("PELOAD_TRACE_D2D_OPS")){ static int nl; if(nl++<12){ char at[80]="      ?\n"; dump_attr((uint64_t)__builtin_return_address(0),at,sizeof at); char*q=at; while(*q==' ') q++; char*nl2=strchr(q,'\n'); if(nl2) *nl2=0;
            const uint32_t*u=(const uint32_t*)m; char t[300]; int k=snprintf(t,sizeof t,"  [d2d-op] SetTransform from %s: %p -> %g %g %g %g %g %g (raw %08x %08x %08x %08x %08x %08x)\n",q,(void*)m,m?m->m11:0,m?m->m12:0,m?m->m21:0,m?m->m22:0,m?m->dx:0,m?m->dy:0,
            m?u[0]:0,m?u[1]:0,m?u[2]:0,m?u[3]:0,m?u[4]:0,m?u[5]:0); (void)!write(2,t,k); } }
    if(m) ((D2Ctx*)s)->tf=*m; }
static void MSABI d2c_GetTransform(void*s,D2Mat*m){ if(m) *m=((D2Ctx*)s)->tf; }
static void MSABI d2c_SetAA(void*s,uint32_t v){ ((D2Ctx*)s)->aa=v; }
static uint32_t MSABI d2c_GetAA(void*s){ return ((D2Ctx*)s)->aa; }
static void MSABI d2c_SetTextAA(void*s,uint32_t v){ ((D2Ctx*)s)->taa=v; }
static uint32_t MSABI d2c_GetTextAA(void*s){ return ((D2Ctx*)s)->taa; }
static void MSABI d2c_SetTextParams(void*s,void*p){ D2Ctx*c=s; if(p) d2_addref(p); if(c->tparams) d2_release(c->tparams); c->tparams=p; }
static void MSABI d2c_GetTextParams(void*s,void**pp){ D2Ctx*c=s; if(pp){ *pp=c->tparams; if(c->tparams) d2_addref(c->tparams); } }
static void MSABI d2c_SetTags(void*s,uint64_t a,uint64_t b){ D2Ctx*c=s; c->tag1=a; c->tag2=b; }
static void MSABI d2c_GetTags(void*s,uint64_t*a,uint64_t*b){ D2Ctx*c=s; if(a)*a=c->tag1; if(b)*b=c->tag2; }
// clips and layers: a saved CG state each (plus a transparency layer for opacity)
static void d2_push(D2Ctx*c,CGRect devbox,int layer,int transp,int mask){ if(c->nclip>=64) return; CGRect parent=d2_clipbox(c);
    D2Clip*k=&c->clip[c->nclip++]; k->dev=CGRectIntersection(devbox,parent); if(CGRectIsNull(k->dev)) k->dev=CGRectZero; k->layer=layer; k->transp=transp; k->mask=mask; }
static void MSABI d2c_PushAxisAlignedClip(void*s,const D2Rect*r,uint32_t aa){ (void)aa; D2Ctx*c=s; CGContextRef cg=d2_surface(c); if(!cg||!r) return;
    CGRect rc=d2_cgrect(r); CGAffineTransform m=d2_ctm(c); CGContextSaveGState(cg); CGContextConcatCTM(cg,m); CGContextClipToRect(cg,rc); CGContextConcatCTM(cg,CGAffineTransformInvert(m));
    d2_push(c,CGRectApplyAffineTransform(rc,m),0,0,!(m.b==0&&m.c==0)); }
static void d2_pop(D2Ctx*c,int layer){ CGContextRef cg=d2_surface(c); if(!c->nclip||!cg) return; D2Clip*k=&c->clip[c->nclip-1]; (void)layer;
    if(k->transp) CGContextEndTransparencyLayer(cg);   // (what was drawn inside already marked its boxes)
    CGContextRestoreGState(cg); c->nclip--; }
static void MSABI d2c_PopAxisAlignedClip(void*s){ d2_pop(s,0); }
static void d2_pushlayer(D2Ctx*c,const D2LayerParams*lp){ CGContextRef cg=d2_surface(c); if(!cg) return; CGAffineTransform m=d2_ctm(c);
    CGRect box=CGRectInfinite; int mask=0, transp=0; CGContextSaveGState(cg); CGContextConcatCTM(cg,m);
    if(lp){ if(isfinite(lp->bounds.l)&&isfinite(lp->bounds.r)&&lp->bounds.r-lp->bounds.l<1e30f){ CGRect rc=d2_cgrect(&lp->bounds); CGContextClipToRect(cg,rc); box=CGRectApplyAffineTransform(rc,m); }
        if(lp->mask && d2_is_geom(lp->mask)){ D2Geom*g=lp->mask; CGAffineTransform mt=d2_cgm(&lp->masktf); CGPathRef p=CGPathCreateCopyByTransformingPath(g->path,&mt);
            CGContextAddPath(cg,p); if(d2_geom_eo(g)) CGContextEOClip(cg); else CGContextClip(cg); if(!CGPathIsEmpty(p)) box=CGRectIntersection(box,CGRectApplyAffineTransform(CGPathGetPathBoundingBox(p),m)); CGPathRelease(p); mask=1; }
        if(lp->opacity<0.999f || lp->opbrush){ CGContextConcatCTM(cg,CGAffineTransformInvert(m)); CGContextSetAlpha(cg,lp->opacity); CGContextBeginTransparencyLayer(cg,0); transp=1; }
        else CGContextConcatCTM(cg,CGAffineTransformInvert(m)); }
    else CGContextConcatCTM(cg,CGAffineTransformInvert(m));
    d2_push(c,box,1,transp,mask); }
static void MSABI d2c_PushLayer(void*s,const D2LayerParams*lp,void*layer){ (void)layer; d2_pushlayer(s,lp); }
static void MSABI d2c_PopLayer(void*s){ d2_pop(s,1); }
static void MSABI d2c_SaveState(void*s,void*blk){ D2Ctx*c=s; if(!blk||((D2Hdr*)blk)->cls!=&C_d2block) return; D2Block*b=blk;
    b->d.aa=c->aa; b->d.taa=c->taa; b->d.tag1=c->tag1; b->d.tag2=c->tag2; b->d.tf=c->tf; b->d.blend=c->blend; b->d.unit=c->unit; blk_SetParams(b,c->tparams); }
static void MSABI d2c_RestoreState(void*s,void*blk){ D2Ctx*c=s; if(!blk||((D2Hdr*)blk)->cls!=&C_d2block) return; D2Block*b=blk;
    c->aa=b->d.aa; c->taa=b->d.taa; c->tag1=b->d.tag1; c->tag2=b->d.tag2; c->tf=b->d.tf; c->blend=b->d.blend; c->unit=b->d.unit; d2c_SetTextParams(c,b->params); }
// Clear: a GL clear of the clip box on a texture target (after the canvas so far), else the canvas
static void MSABI d2c_Clear(void*s,const D2Col*col){ D2Ctx*c=s; if(!c->target) return; D2Col k=col?*col:(D2Col){0,0,0,0}; c->nops++;
    if(d2_gpu_ok(c)){ D3DObj*rt=c->target->tex; CGRect box=d2_clipbox(c); for(int i=0;i<c->nclip;i++) if(c->clip[i].mask) goto cpu;
        d2_flush(c); if(sub_on()) sub_drain(); GLLOCK();
        if(d3dgl_ensure()){ d3dgl_ensure_rt(rt); gl_bindfbo_any(GLE_FBO,rt->glfbo); d3dgl_reset_raster_ops(); d2gl_scissor(rt,box);
            float a=c->target->alpha==3?1.0f:k.a; G.ClearColor(k.r*a,k.g*a,k.b*a,a); G.Clear(GLE_COLORBIT); gs_cap(0x0C11,0); gl_bindfbo_any(GLE_FBO,0); gs_invalidate();
            if(rt==g_d3d_backbuf) d3dgl_latch_present(rt,(int)rt->w,(int)rt->h,0); }
        GLUNLOCK(); return; }
    cpu:;
    CGContextRef cg=d2_surface(c); if(!cg) return; CGContextSaveGState(cg); CGContextSetBlendMode(cg,kCGBlendModeCopy); CGContextSetRGBFillColor(cg,k.r,k.g,k.b,k.a);
    CGRect box=d2_clipbox(c); CGContextConcatCTM(cg,CGAffineTransformInvert(CGContextGetCTM(cg))); CGContextConcatCTM(cg,CGAffineTransformMake(1,0,0,-1,0,c->canvas?c->ch:(c->target?c->target->hh:0)));
    CGContextFillRect(cg,box); CGContextRestoreGState(cg);
    if(c->canvas){ if(!c->dirty){ c->dx0=box.origin.x; c->dy0=box.origin.y; c->dx1=CGRectGetMaxX(box); c->dy1=CGRectGetMaxY(box); c->dirty=1; }
        else { if(box.origin.x<c->dx0) c->dx0=box.origin.x; if(box.origin.y<c->dy0) c->dy0=box.origin.y; if(CGRectGetMaxX(box)>c->dx1) c->dx1=CGRectGetMaxX(box); if(CGRectGetMaxY(box)>c->dy1) c->dy1=CGRectGetMaxY(box); } }
    else if(c->target) c->target->ver++; }
static void MSABI d2c_BeginDraw(void*s){ D2Ctx*c=s; c->drawing=1; c->nops=0; }
static int MSABI d2c_EndDraw(void*s,uint64_t*t1,uint64_t*t2){ D2Ctx*c=s;
    if(d2_trace()){ static int nl; if(nl++<6){ char m[220]; int k=snprintf(m,sizeof m,"  [d2d] EndDraw: %lu ops, dirty %d box %.0f,%.0f-%.0f,%.0f, clips %d, target %p tex %p canvas %p %ux%u, dpi %.0f unit %u\n",
            c->nops,c->dirty,c->dx0,c->dy0,c->dx1,c->dy1,c->nclip,(void*)c->target,c->target?(void*)c->target->tex:0,(void*)c->canvas,c->cw,c->ch,c->dpix,c->unit); (void)!write(2,m,k); }
      const char*dn=getenv("PELOAD_D2D_DUMP"); static int dumped; if(dn && c->canvas && !dumped && (unsigned long)atol(dn)<=c->nops){ dumped=1; char p[256]; snprintf(p,sizeof p,"%s/d2d_canvas_%d.bmp",getenv("HOME"),(int)getpid());
          uint8_t*rgba=malloc((size_t)c->cw*c->ch*4); for(uint32_t y=0;y<c->ch;y++) for(uint32_t x=0;x<c->cw;x++){ const uint8_t*q=c->canvas+((size_t)(c->ch-1-y)*c->cw+x)*4; uint8_t*d=rgba+((size_t)y*c->cw+x)*4; d[0]=q[2]; d[1]=q[1]; d[2]=q[0]; d[3]=255; }
          bmp_write_rgba(p,(int)c->cw,(int)c->ch,rgba); free(rgba); char m[300]; int k=snprintf(m,sizeof m,"  [d2d] canvas -> %s\n",p); (void)!write(2,m,k); } }
    while(c->nclip) d2_pop(c,1); d2_flush(c); if(t1)*t1=c->tag1; if(t2)*t2=c->tag2;
    if(d2_trace()){ static unsigned long n; if(n++<5 || n%600==0){ char m[120]; int k=snprintf(m,sizeof m,"  [d2d] EndDraw #%lu: %lu operations\n",n,c->nops); (void)!write(2,m,k); } }
    c->drawing=0; return 0; }
static int MSABI d2c_Flush(void*s,uint64_t*t1,uint64_t*t2){ D2Ctx*c=s; d2_flush(c); if(t1)*t1=c->tag1; if(t2)*t2=c->tag2; return 0; }
static D2PixFmt* MSABI d2c_GetPixelFormat(void*s,D2PixFmt*r){ D2Ctx*c=s; r->fmt=c->target?c->target->fmt:87; r->alpha=c->target?c->target->alpha:1; return r; }
static void MSABI d2c_SetDpi(void*s,float x,float y){ D2Ctx*c=s; c->dpix=d2_dpi(x); c->dpiy=d2_dpi(y); }
static void MSABI d2c_GetDpi(void*s,float*x,float*y){ D2Ctx*c=s; if(x)*x=c->dpix; if(y)*y=c->dpiy; }
static D2SizeU d2_ctx_px(D2Ctx*c){ D2SizeU r={0,0}; if(c->target){ r.w=c->target->w; r.h=c->target->hh; } return r; }
static D2SizeF* MSABI d2c_GetSize(void*s,D2SizeF*r){ D2Ctx*c=s; D2SizeU p=d2_ctx_px(c); r->w=p.w*96.0f/c->dpix; r->h=p.h*96.0f/c->dpiy; return r; }
static D2SizeU* MSABI d2c_GetPixelSize(void*s,D2SizeU*r){ *r=d2_ctx_px(s); return r; }
static uint32_t MSABI d2c_GetMaxBitmapSize(void*s){ (void)s; return 16384; }
static int MSABI d2c_IsSupported(void*s,const void*p){ (void)s;(void)p; return 1; }
static int MSABI d2c_IsDxgiFormatSupported(void*s,uint32_t f){ (void)s; return f==87||f==28||f==91||f==29||f==88||f==65; }
static int MSABI d2c_IsBufferPrecisionSupported(void*s,uint32_t p){ (void)s; return p<=1; }
static int MSABI d2c_GetImageLocalBounds(void*s,void*img,D2Rect*o){ (void)s; if(!o) return (int)HR_INVALIDARG; if(!d2_is_bmp(img)) return (int)HR_INVALIDARG; D2Bmp*b=img;
    o->l=o->t=0; o->r=b->w*96.0f/b->dpix; o->b=b->hh*96.0f/b->dpiy; return 0; }
static int MSABI d2c_GetImageWorldBounds(void*s,void*img,D2Rect*o){ D2Ctx*c=s; int hr=d2c_GetImageLocalBounds(s,img,o); if(hr<0) return hr;
    CGRect r=CGRectApplyAffineTransform(CGRectMake(o->l,o->t,o->r-o->l,o->b-o->t),d2_cgm(&c->tf)); d2_rect_out(o,r); return 0; }
static void MSABI d2c_GetDevice(void*s,void**pp){ D2Ctx*c=s; if(pp){ *pp=c->dev; if(c->dev) d2_AddRef(c->dev); } }
static void MSABI d2c_SetTarget(void*s,void*img){ D2Ctx*c=s; if(img && !d2_is_bmp(img)){ d2_unimpl(s,74); return; }
    if(img==c->target) return; while(c->nclip) d2_pop(c,1); d2_flush(c); if(img) d2_AddRef(img); if(c->target) d2_release(c->target); c->target=img; }
    // (the canvas stays: Gravemind sets NULL and the back buffer again every frame; d2_surface remakes it only for another size)
static void MSABI d2c_GetTarget(void*s,void**pp){ D2Ctx*c=s; if(pp){ *pp=c->target; if(c->target) d2_AddRef(c->target); } }
static void MSABI d2c_SetRenderingControls(void*s,const void*p){ (void)s;(void)p; }
static void MSABI d2c_GetRenderingControls(void*s,uint32_t*p){ (void)s; if(p){ p[0]=0; p[1]=4096; p[2]=4096; } }
static void MSABI d2c_SetPrimitiveBlend(void*s,uint32_t v){ ((D2Ctx*)s)->blend=v; }
static uint32_t MSABI d2c_GetPrimitiveBlend(void*s){ return ((D2Ctx*)s)->blend; }
static void MSABI d2c_SetUnitMode(void*s,uint32_t v){ ((D2Ctx*)s)->unit=v; }
static uint32_t MSABI d2c_GetUnitMode(void*s){ return ((D2Ctx*)s)->unit; }
static void d2_setup_ctx(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)d2c_CreateBitmap; v[5]=(void*)d2c_CreateBitmapFromWic; v[6]=(void*)d2c_CreateSharedBitmap;
    v[7]=(void*)d2c_CreateBitmapBrush; v[8]=(void*)d2c_CreateSolidColorBrush; v[9]=(void*)d2c_CreateGradientStops; v[10]=(void*)d2c_CreateLinearGradientBrush;
    v[11]=(void*)d2c_CreateRadialGradientBrush; v[13]=(void*)d2c_CreateLayer; v[15]=(void*)d2c_DrawLine; v[16]=(void*)d2c_DrawRectangle; v[17]=(void*)d2c_FillRectangle;
    v[18]=(void*)d2c_DrawRoundedRectangle; v[19]=(void*)d2c_FillRoundedRectangle; v[20]=(void*)d2c_DrawEllipse; v[21]=(void*)d2c_FillEllipse; v[22]=(void*)d2c_DrawGeometry;
    v[23]=(void*)d2c_FillGeometry; v[26]=(void*)d2c_DrawBitmap; v[27]=(void*)d2c_DrawText; v[28]=(void*)d2c_DrawTextLayout; v[30]=(void*)d2c_SetTransform; v[31]=(void*)d2c_GetTransform;
    v[32]=(void*)d2c_SetAA; v[33]=(void*)d2c_GetAA; v[34]=(void*)d2c_SetTextAA; v[35]=(void*)d2c_GetTextAA; v[36]=(void*)d2c_SetTextParams; v[37]=(void*)d2c_GetTextParams;
    v[38]=(void*)d2c_SetTags; v[39]=(void*)d2c_GetTags; v[40]=(void*)d2c_PushLayer; v[41]=(void*)d2c_PopLayer; v[42]=(void*)d2c_Flush; v[43]=(void*)d2c_SaveState; v[44]=(void*)d2c_RestoreState;
    v[45]=(void*)d2c_PushAxisAlignedClip; v[46]=(void*)d2c_PopAxisAlignedClip; v[47]=(void*)d2c_Clear; v[48]=(void*)d2c_BeginDraw; v[49]=(void*)d2c_EndDraw;
    v[50]=(void*)d2c_GetPixelFormat; v[51]=(void*)d2c_SetDpi; v[52]=(void*)d2c_GetDpi; v[53]=(void*)d2c_GetSize; v[54]=(void*)d2c_GetPixelSize; v[55]=(void*)d2c_GetMaxBitmapSize; v[56]=(void*)d2c_IsSupported;
    v[57]=(void*)d2c_CreateBitmap1; v[58]=(void*)d2c_CreateBitmapFromWic1; v[62]=(void*)d2c_CreateBitmapFromDxgiSurface; v[64]=(void*)d2c_CreateGradientStops1;
    v[66]=(void*)d2c_CreateBitmapBrush; v[68]=(void*)d2c_IsDxgiFormatSupported; v[69]=(void*)d2c_IsBufferPrecisionSupported; v[70]=(void*)d2c_GetImageLocalBounds;
    v[71]=(void*)d2c_GetImageWorldBounds; v[73]=(void*)d2c_GetDevice; v[74]=(void*)d2c_SetTarget; v[75]=(void*)d2c_GetTarget; v[76]=(void*)d2c_SetRenderingControls;
    v[77]=(void*)d2c_GetRenderingControls; v[78]=(void*)d2c_SetPrimitiveBlend; v[79]=(void*)d2c_GetPrimitiveBlend; v[80]=(void*)d2c_SetUnitMode; v[81]=(void*)d2c_GetUnitMode;
    v[83]=(void*)d2c_DrawImage; v[85]=(void*)d2c_DrawBitmap1; v[86]=(void*)d2c_PushLayer; }
// ==== device and factory ======================================================================================================
typedef struct { D2Hdr h; uint64_t maxmem; } D2Dev;
static int MSABI dev_CreateDeviceContext(void*s,uint32_t opts,void**pp){ (void)opts; D2Dev*d=s; if(!pp) return (int)HR_INVALIDARG; *pp=d2_ctx_new(d->h.fac,d);
    if(d2_trace()) (void)!write(2,"  [d2d] CreateDeviceContext\n",28); return 0; }
static void MSABI dev_SetMaxMem(void*s,uint64_t m){ ((D2Dev*)s)->maxmem=m; }
static uint64_t MSABI dev_GetMaxMem(void*s){ return ((D2Dev*)s)->maxmem; }
static void MSABI dev_ClearResources(void*s,uint32_t ms){ (void)s;(void)ms; }
static void d2_setup_dev(void**v){ v[3]=(void*)d2_GetFactory; v[4]=(void*)dev_CreateDeviceContext; v[6]=(void*)dev_SetMaxMem; v[7]=(void*)dev_GetMaxMem; v[8]=(void*)dev_ClearResources; }
static int MSABI fac_Reload(void*s){ (void)s; return 0; }
static void MSABI fac_GetDesktopDpi(void*s,float*x,float*y){ (void)s; if(x)*x=96; if(y)*y=96; }
static int MSABI fac_CreateRectangleGeometry(void*s,const D2Rect*r,void**pp){ if(!r||!pp) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2grect,GK_RECT,s); g->rect=*r; CGPathAddRect(g->path,0,d2_cgrect(r)); *pp=g; return 0; }
static int MSABI fac_CreateRoundedRectangleGeometry(void*s,const D2RRect*r,void**pp){ if(!r||!pp) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2grrect,GK_RRECT,s); g->rr=*r; d2_add_rrect(g->path,r,0); *pp=g; return 0; }
static int MSABI fac_CreateEllipseGeometry(void*s,const D2Ell*e,void**pp){ if(!e||!pp) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2gell,GK_ELL,s); g->ell=*e; d2_add_ell(g->path,e,0); *pp=g; return 0; }
static int MSABI fac_CreateGeometryGroup(void*s,uint32_t fill,void**geos,uint32_t n,void**pp){ if(!pp) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2ggroup,GK_GROUP,s); g->fill=fill;
    g->kids=calloc(n?n:1,sizeof(void*)); for(uint32_t i=0;geos&&i<n;i++) if(d2_is_geom(geos[i])){ D2Geom*k=geos[i]; g->kids[g->nkids++]=k; d2_AddRef(k); CGPathAddPath(g->path,0,k->path); }
    *pp=g; return 0; }
static int MSABI fac_CreateTransformedGeometry(void*s,void*src,const D2Mat*m,void**pp){ if(!pp||!d2_is_geom(src)) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2gtf,GK_TF,s);
    g->src=src; d2_AddRef(src); if(m) g->tf=*m; CGAffineTransform t=d2_cgm(&g->tf); CGPathAddPath(g->path,&t,((D2Geom*)src)->path); *pp=g; return 0; }
static int MSABI fac_CreatePathGeometry(void*s,void**pp){ if(!pp) return (int)HR_INVALIDARG; D2Geom*g=d2_geom_new(&C_d2gpath,GK_PATH,s); g->fill=0; *pp=g; return 0; }
static int MSABI fac_CreateStrokeStyle1(void*s,const D2StrokeProps*p,const float*d,uint32_t n,void**pp){ if(!pp) return (int)HR_INVALIDARG; D2Stroke*k=d2_alloc(&C_d2stroke,sizeof(D2Stroke)); k->h.fac=s;
    if(p) k->p=*p; else k->p.miter=10; if(d&&n){ k->dashes=malloc(sizeof(float)*n); memcpy(k->dashes,d,sizeof(float)*n); k->nd=(int)n; } *pp=k; return 0; }
static int MSABI fac_CreateStrokeStyle(void*s,const D2StrokeProps*p,const float*d,uint32_t n,void**pp){ D2StrokeProps q; memset(&q,0,sizeof q); q.miter=10; if(p) memcpy(&q,p,28); return fac_CreateStrokeStyle1(s,&q,d,n,pp); }
static int MSABI fac_CreateDrawingStateBlock1(void*s,const D2StateDesc*d,void*params,void**pp){ if(!pp) return (int)HR_INVALIDARG; D2Block*b=d2_alloc(&C_d2block,sizeof(D2Block)); b->h.fac=s;
    if(d) b->d=*d; else b->d.tf=d2_ident(); blk_SetParams(b,params); *pp=b; return 0; }
static int MSABI fac_CreateDrawingStateBlock(void*s,const D2StateDesc*d,void*params,void**pp){ D2StateDesc q; memset(&q,0,sizeof q); q.tf=d2_ident(); if(d) memcpy(&q,d,48); return fac_CreateDrawingStateBlock1(s,&q,params,pp); }
static int MSABI fac_CreateDxgiSurfaceRenderTarget(void*s,void*surf,const D2RtProps*p,void**pp){ if(!pp) return (int)HR_INVALIDARG; D2Ctx*c=d2_ctx_new(s,0); void*bm=0;
    D2BmpProps1 bp={{p?p->pf.fmt:0,p&&p->pf.alpha?p->pf.alpha:1},p?p->dpix:96,p?p->dpiy:96,1,0}; int hr=d2c_CreateBitmapFromDxgiSurface(c,surf,&bp,&bm);
    if(hr<0){ d2_Release(c); *pp=0; return hr; } d2c_SetTarget(c,bm); d2_Release(bm); if(p){ c->dpix=d2_dpi(p->dpix); c->dpiy=d2_dpi(p->dpiy); } *pp=c; return 0; }
static int MSABI fac_CreateDevice(void*s,void*dxgidev,void**pp){ (void)dxgidev; if(!pp) return (int)HR_INVALIDARG; D2Dev*d=d2_alloc(&C_d2dev,sizeof(D2Dev)); d->h.fac=s; *pp=d;
    if(d2_trace()) (void)!write(2,"  [d2d] CreateDevice\n",21); return 0; }
static void d2_setup_fac(void**v){ v[3]=(void*)fac_Reload; v[4]=(void*)fac_GetDesktopDpi; v[5]=(void*)fac_CreateRectangleGeometry; v[6]=(void*)fac_CreateRoundedRectangleGeometry;
    v[7]=(void*)fac_CreateEllipseGeometry; v[8]=(void*)fac_CreateGeometryGroup; v[9]=(void*)fac_CreateTransformedGeometry; v[10]=(void*)fac_CreatePathGeometry; v[11]=(void*)fac_CreateStrokeStyle;
    v[12]=(void*)fac_CreateDrawingStateBlock; v[15]=(void*)fac_CreateDxgiSurfaceRenderTarget; v[17]=(void*)fac_CreateDevice; v[18]=(void*)fac_CreateStrokeStyle1;
    v[19]=(void*)fac_CreatePathGeometry; v[20]=(void*)fac_CreateDrawingStateBlock1; }
static D2Cls C_d2fac={"ID2D1Factory1",SLOTS_ID2D1Factory1,NSLOT_ID2D1Factory1,{&IID_IUnknown,&IID_ID2D1Factory,&IID_ID2D1Factory1},0,d2_setup_fac};
static D2Cls C_d2dev={"ID2D1Device",SLOTS_ID2D1Device,NSLOT_ID2D1Device,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Device},0,d2_setup_dev};
static D2Cls C_d2ctx={"ID2D1DeviceContext",SLOTS_ID2D1DeviceContext,NSLOT_ID2D1DeviceContext,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1RenderTarget,&IID_ID2D1DeviceContext},d2_ctx_dtor,d2_setup_ctx};
static D2Cls C_d2bmp={"ID2D1Bitmap1",SLOTS_ID2D1Bitmap1,NSLOT_ID2D1Bitmap1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Image,&IID_ID2D1Bitmap,&IID_ID2D1Bitmap1},d2_bmp_dtor,d2_setup_bmp};
static D2Cls C_d2solid={"ID2D1SolidColorBrush",SLOTS_ID2D1SolidColorBrush,NSLOT_ID2D1SolidColorBrush,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Brush,&IID_ID2D1SolidColorBrush},d2_brush_dtor,d2_setup_solid};
static D2Cls C_d2lin={"ID2D1LinearGradientBrush",SLOTS_ID2D1LinearGradientBrush,NSLOT_ID2D1LinearGradientBrush,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Brush,&IID_ID2D1LinearGradientBrush},d2_brush_dtor,d2_setup_lin};
static D2Cls C_d2rad={"ID2D1RadialGradientBrush",SLOTS_ID2D1RadialGradientBrush,NSLOT_ID2D1RadialGradientBrush,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Brush,&IID_ID2D1RadialGradientBrush},d2_brush_dtor,d2_setup_rad};
static D2Cls C_d2bbrush={"ID2D1BitmapBrush1",SLOTS_ID2D1BitmapBrush1,NSLOT_ID2D1BitmapBrush1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Brush,&IID_ID2D1BitmapBrush,&IID_ID2D1BitmapBrush1},d2_brush_dtor,d2_setup_bbrush};
static D2Cls C_d2stops={"ID2D1GradientStopCollection1",SLOTS_ID2D1GradientStopCollection1,NSLOT_ID2D1GradientStopCollection1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1GradientStopCollection,&IID_ID2D1GradientStopCollection1},d2_stops_dtor,d2_setup_stops};
static D2Cls C_d2stroke={"ID2D1StrokeStyle1",SLOTS_ID2D1StrokeStyle1,NSLOT_ID2D1StrokeStyle1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1StrokeStyle,&IID_ID2D1StrokeStyle1},d2_stroke_dtor,d2_setup_stroke};
static D2Cls C_d2layer={"ID2D1Layer",SLOTS_ID2D1Layer,NSLOT_ID2D1Layer,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Layer},0,d2_setup_layer};
static D2Cls C_d2block={"ID2D1DrawingStateBlock1",SLOTS_ID2D1DrawingStateBlock1,NSLOT_ID2D1DrawingStateBlock1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1DrawingStateBlock,&IID_ID2D1DrawingStateBlock1},d2_block_dtor,d2_setup_block};
static D2Cls C_d2grect={"ID2D1RectangleGeometry",SLOTS_ID2D1RectangleGeometry,NSLOT_ID2D1RectangleGeometry,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1RectangleGeometry},d2_geom_dtor,d2_setup_grect};
static D2Cls C_d2grrect={"ID2D1RoundedRectangleGeometry",SLOTS_ID2D1RoundedRectangleGeometry,NSLOT_ID2D1RoundedRectangleGeometry,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1RoundedRectangleGeometry},d2_geom_dtor,d2_setup_grrect};
static D2Cls C_d2gell={"ID2D1EllipseGeometry",SLOTS_ID2D1EllipseGeometry,NSLOT_ID2D1EllipseGeometry,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1EllipseGeometry},d2_geom_dtor,d2_setup_gell};
static D2Cls C_d2gpath={"ID2D1PathGeometry1",SLOTS_ID2D1PathGeometry1,NSLOT_ID2D1PathGeometry1,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1PathGeometry,&IID_ID2D1PathGeometry1},d2_geom_dtor,d2_setup_gpath};
static D2Cls C_d2gtf={"ID2D1TransformedGeometry",SLOTS_ID2D1TransformedGeometry,NSLOT_ID2D1TransformedGeometry,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1TransformedGeometry},d2_geom_dtor,d2_setup_gtf};
static D2Cls C_d2ggroup={"ID2D1GeometryGroup",SLOTS_ID2D1GeometryGroup,NSLOT_ID2D1GeometryGroup,{&IID_IUnknown,&IID_ID2D1Resource,&IID_ID2D1Geometry,&IID_ID2D1GeometryGroup},d2_geom_dtor,d2_setup_ggroup};
static D2Cls C_d2sink={"ID2D1GeometrySink",SLOTS_ID2D1GeometrySink,NSLOT_ID2D1GeometrySink,{&IID_IUnknown,&IID_ID2D1SimplifiedGeometrySink,&IID_ID2D1GeometrySink},d2_sink_dtor,d2_setup_sink};
// ==== exports ===================================================================================================================
// D2D1CreateFactory(type, iid, options, out): a new factory (single- and multi-threaded alike)
static int MSABI i_D2D1CreateFactory(int type,const GUID_*iid,const void*opt,void**pp){ (void)type;(void)opt; if(!pp) return (int)HR_INVALIDARG;
    if(getenv("PELOAD_NO_D2D")){ *pp=0; return (int)HR_E_NOTIMPL; }
    D2Hdr*f=d2_alloc(&C_d2fac,sizeof(D2Hdr)); f->fac=f; int hr=d2_QI(f,iid,pp); d2_Release(f);
    if(d2_trace()){ char m[100]; int k=snprintf(m,sizeof m,"  [d2d] D2D1CreateFactory -> 0x%08x\n",(unsigned)hr); (void)!write(2,m,k); } return hr; }
static void MSABI i_D2D1MakeRotateMatrix(float a,D2Pt c,D2Mat*m){ double r=a*M_PI/180, cs=cos(r), sn=sin(r); if(!m) return;
    m->m11=(float)cs; m->m12=(float)sn; m->m21=(float)-sn; m->m22=(float)cs; m->dx=(float)(c.x-c.x*cs+c.y*sn); m->dy=(float)(c.y-c.x*sn-c.y*cs); }
static void MSABI i_D2D1MakeSkewMatrix(float ax,float ay,D2Pt c,D2Mat*m){ double tx=tan(ax*M_PI/180), ty=tan(ay*M_PI/180); if(!m) return;
    m->m11=1; m->m12=(float)ty; m->m21=(float)tx; m->m22=1; m->dx=(float)(-c.y*tx); m->dy=(float)(-c.x*ty); }
static int MSABI i_D2D1IsMatrixInvertible(const D2Mat*m){ return m && fabsf(m->m11*m->m22-m->m12*m->m21)>1e-12f; }
static int MSABI i_D2D1InvertMatrix(D2Mat*m){ if(!i_D2D1IsMatrixInvertible(m)) return 0; CGAffineTransform t=CGAffineTransformInvert(d2_cgm(m));
    m->m11=(float)t.a; m->m12=(float)t.b; m->m21=(float)t.c; m->m22=(float)t.d; m->dx=(float)t.tx; m->dy=(float)t.ty; return 1; }
static void MSABI i_D2D1SinCos(float a,float*s,float*c){ if(s)*s=sinf(a); if(c)*c=cosf(a); }
static float MSABI i_D2D1Tan(float a){ return tanf(a); }
static float MSABI i_D2D1Vec3Length(float x,float y,float z){ return sqrtf(x*x+y*y+z*z); }
static int MSABI i_D2D1CreateDevice(void*dxgidev,const void*props,void**pp){ (void)props; void*f=0; int hr=i_D2D1CreateFactory(0,&IID_ID2D1Factory1,0,&f); if(hr<0) return hr;
    hr=fac_CreateDevice(f,dxgidev,pp); d2_Release(f); return hr; }
static int MSABI i_D2D1CreateDeviceContext(void*surf,const void*props,void**pp){ void*dev=0; int hr=i_D2D1CreateDevice(0,props,&dev); if(hr<0) return hr;
    hr=dev_CreateDeviceContext(dev,0,pp); d2_Release(dev); if(hr<0) return hr; void*bm=0; D2BmpProps1 bp={{0,1},96,96,1,0};
    if(d2c_CreateBitmapFromDxgiSurface(*pp,surf,&bp,&bm)>=0){ d2c_SetTarget(*pp,bm); d2_Release(bm); } return 0; }
// ==== PELOAD_D2D_SELFTEST=<out.bmp>: the loader calls its own Direct2D / DirectWrite / WIC through their vtables the way a game does,
// draws a test card into a CPU target bitmap, checks pixels and writes the picture (shim/tools/d2d-test.sh). No game, no window.
#define VT(o,i) ((*(void***)(o))[i])
static int g_d2t_fail;
static void d2t_check(const char*what,D2Bmp*b,int x,int y,int r,int g,int bl,int tol){ const uint8_t*p=b->px+(size_t)y*b->stride+(size_t)x*4;
    int ok=abs(p[2]-r)<=tol && abs(p[1]-g)<=tol && abs(p[0]-bl)<=tol; if(!ok) g_d2t_fail++;
    printf("%s %-34s at %3d,%3d: rgb %3d %3d %3d (want %3d %3d %3d)\n",ok?"PASS":"FAIL",what,x,y,p[2],p[1],p[0],r,g,bl); }
static int d2t_bright(D2Bmp*b,int x0,int y0,int x1,int y1){ int n=0; for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){ const uint8_t*p=b->px+(size_t)y*b->stride+(size_t)x*4; if(p[0]+p[1]+p[2]>500) n++; } return n; }
static const uint16_t* d2t_w(const char*s){ static uint16_t buf[8][256]; static int k; uint16_t*o=buf[k++&7]; int i=0; for(;s[i]&&i<255;i++) o[i]=(uint8_t)s[i]; o[i]=0; return o; }
static int d2d_selftest(const char*out){
    void*f=0,*dev=0,*ctx=0,*bmp=0,*red=0,*white=0,*green=0,*stops=0,*lin=0,*rad=0,*path=0,*sink=0,*ss=0,*layer=0,*dw=0,*fmt=0,*lay=0;
    printf("Direct2D self-test: factory 0x%08x\n",(unsigned)i_D2D1CreateFactory(1,&IID_ID2D1Factory1,0,&f)); if(!f) return 1;
    ((int(MSABI*)(void*,void*,void**))VT(f,17))(f,0,&dev); ((int(MSABI*)(void*,uint32_t,void**))VT(dev,4))(dev,0,&ctx);
    D2BmpProps1 bp={{87,1},96,96,1,0}; D2SizeU sz={640,360}; ((int(MSABI*)(void*,D2SizeU,const void*,uint32_t,const D2BmpProps1*,void**))VT(ctx,57))(ctx,sz,0,0,&bp,&bmp);
    ((void(MSABI*)(void*,void*))VT(ctx,74))(ctx,bmp); ((void(MSABI*)(void*))VT(ctx,48))(ctx);
    D2Col bg={0.05f,0.08f,0.15f,1}, cr={1,0,0,1}, cw={1,1,1,1}, cg={0,1,0,1}; ((void(MSABI*)(void*,const D2Col*))VT(ctx,47))(ctx,&bg);
    typedef int(MSABI*mkbrush)(void*,const D2Col*,const D2BrushProps*,void**); ((mkbrush)VT(ctx,8))(ctx,&cr,0,&red); ((mkbrush)VT(ctx,8))(ctx,&cw,0,&white); ((mkbrush)VT(ctx,8))(ctx,&cg,0,&green);
    D2Rect r1={20,20,120,100}; ((void(MSABI*)(void*,const D2Rect*,void*))VT(ctx,17))(ctx,&r1,red);
    D2Rect r2={140,20,240,100}; ((void(MSABI*)(void*,const D2Rect*,void*,float,void*))VT(ctx,16))(ctx,&r2,red,4.0f,0);
    D2Stop st[2]={{0,{1,1,0,1}},{1,{1,0,1,1}}}; ((int(MSABI*)(void*,const D2Stop*,uint32_t,uint32_t,uint32_t,void**))VT(ctx,9))(ctx,st,2,0,0,&stops);
    D2Pt lp[2]={{260,20},{360,100}}; ((int(MSABI*)(void*,const D2Pt*,const D2BrushProps*,void*,void**))VT(ctx,10))(ctx,lp,0,stops,&lin);
    D2RRect rr={{260,20,360,100},12,12}; ((void(MSABI*)(void*,const D2RRect*,void*))VT(ctx,19))(ctx,&rr,lin);
    float rp[6]={430,60,0,0,50,40}; ((int(MSABI*)(void*,const float*,const D2BrushProps*,void*,void**))VT(ctx,11))(ctx,rp,0,stops,&rad);
    D2Ell el={{430,60},50,40}; ((void(MSABI*)(void*,const D2Ell*,void*))VT(ctx,21))(ctx,&el,rad);
    ((int(MSABI*)(void*,void**))VT(f,10))(f,&path); ((int(MSABI*)(void*,void**))VT(path,17))(path,&sink);
    D2Pt p0={500,100}, p1={540,20}; D2Arc arc={{620,100},{40,40},0,1,0};
    ((void(MSABI*)(void*,D2Pt,uint32_t))VT(sink,5))(sink,p0,0); ((void(MSABI*)(void*,D2Pt))VT(sink,10))(sink,p1); ((void(MSABI*)(void*,const D2Arc*))VT(sink,14))(sink,&arc);
    ((void(MSABI*)(void*,uint32_t))VT(sink,8))(sink,1); ((int(MSABI*)(void*))VT(sink,9))(sink); d2_release(sink);
    ((void(MSABI*)(void*,void*,void*,void*))VT(ctx,23))(ctx,path,green,0);
    D2StrokeProps sp={0,0,0,0,10,1,0,0}; ((int(MSABI*)(void*,const D2StrokeProps*,const float*,uint32_t,void**))VT(f,11))(f,&sp,0,0,&ss);
    D2Pt l0={20,130}, l1={620,130}; ((void(MSABI*)(void*,D2Pt,D2Pt,void*,float,void*))VT(ctx,15))(ctx,l0,l1,white,3.0f,ss);
    D2Rect clip={20,150,100,200}; ((void(MSABI*)(void*,const D2Rect*,uint32_t))VT(ctx,45))(ctx,&clip,0);
    D2Ell big={{60,175},80,80}; ((void(MSABI*)(void*,const D2Ell*,void*))VT(ctx,21))(ctx,&big,green); ((void(MSABI*)(void*))VT(ctx,46))(ctx);
    ((int(MSABI*)(void*,const D2SizeF*,void**))VT(ctx,13))(ctx,0,&layer);
    D2LayerParams lpar={{-FLT_MAX,-FLT_MAX,FLT_MAX,FLT_MAX},0,0,{1,0,0,1,0,0},0.5f,0,0}; ((void(MSABI*)(void*,const D2LayerParams*,void*))VT(ctx,40))(ctx,&lpar,layer);
    D2Rect r3={120,150,220,200}; ((void(MSABI*)(void*,const D2Rect*,void*))VT(ctx,17))(ctx,&r3,white); ((void(MSABI*)(void*))VT(ctx,41))(ctx);
    // DirectWrite: a system-collection format, DrawText, a wrapped centred layout and its metrics
    i_DWriteCreateFactory(0,&IID_IDWriteFactory5,&dw);
    ((int(MSABI*)(void*,const uint16_t*,void*,int,int,int,float,const uint16_t*,void**))VT(dw,15))(dw,d2t_w("Segoe UI"),0,700,0,5,28.0f,d2t_w("en-us"),&fmt);
    const char*t1="Direct2D on CoreGraphics"; D2Rect tr={240,150,620,200};
    ((void(MSABI*)(void*,const uint16_t*,uint32_t,void*,const D2Rect*,void*,uint32_t,uint32_t))VT(ctx,27))(ctx,d2t_w(t1),(uint32_t)strlen(t1),fmt,&tr,white,0,0);
    const char*t2="Centred and wrapped in a 300 px box: DirectWrite on CoreText";
    ((int(MSABI*)(void*,const uint16_t*,uint32_t,void*,float,float,void**))VT(dw,18))(dw,d2t_w(t2),(uint32_t)strlen(t2),fmt,300.0f,120.0f,&lay);
    ((int(MSABI*)(void*,int))VT(lay,3))(lay,2); D2Pt o={20,220}; ((void(MSABI*)(void*,D2Pt,void*,void*,uint32_t))VT(ctx,28))(ctx,o,lay,white,0);
    DwTextMetrics tm; ((int(MSABI*)(void*,DwTextMetrics*))VT(lay,60))(lay,&tm);
    int okm=tm.lines>=2 && tm.width>100 && tm.width<=300.5f && tm.height>40; if(!okm) g_d2t_fail++;
    printf("%s text metrics: %u lines, width %.1f, height %.1f, left %.1f\n",okm?"PASS":"FAIL",tm.lines,tm.width,tm.height,tm.left);
    float hx=0,hy=0; DwHit hm; ((int(MSABI*)(void*,uint32_t,int,float*,float*,DwHit*))VT(lay,65))(lay,0,0,&hx,&hy,&hm); printf("     hit test of position 0: x %.1f y %.1f (line height %.1f)\n",hx,hy,hm.height);
    // an in-memory font (how Gravemind ships its fonts): loader -> font set builder -> collection -> FindFamilyName -> a format on it
    const char*ffile="/System/Library/Fonts/Supplemental/Georgia.ttf"; FILE*ff=fopen(ffile,"rb");
    if(ff){ fseek(ff,0,SEEK_END); long n=ftell(ff); fseek(ff,0,SEEK_SET); uint8_t*fb=malloc((size_t)n); size_t got=fread(fb,1,(size_t)n,ff); fclose(ff);
        void*ldr=0,*bld=0,*file=0,*set=0,*coll=0,*fmt2=0; uint32_t idx=0; int exists=0;
        ((int(MSABI*)(void*,void**))VT(dw,44))(dw,&ldr); ((int(MSABI*)(void*,void*))VT(dw,13))(dw,ldr); ((int(MSABI*)(void*,void**))VT(dw,43))(dw,&bld);
        ((int(MSABI*)(void*,void*,const void*,uint32_t,void*,void**))VT(ldr,4))(ldr,dw,fb,(uint32_t)got,0,&file); ((int(MSABI*)(void*,void*))VT(bld,7))(bld,file);
        ((int(MSABI*)(void*,void**))VT(bld,6))(bld,&set); ((int(MSABI*)(void*,void*,void**))VT(dw,37))(dw,set,&coll);
        ((int(MSABI*)(void*,const uint16_t*,uint32_t*,int*))VT(coll,5))(coll,d2t_w("Georgia"),&idx,&exists); if(!exists) g_d2t_fail++;
        printf("%s in-memory font collection finds 'Georgia' (index %u)\n",exists?"PASS":"FAIL",idx);
        ((int(MSABI*)(void*,const uint16_t*,void*,int,int,int,float,const uint16_t*,void**))VT(dw,15))(dw,d2t_w("Georgia"),coll,400,1,5,24.0f,d2t_w("en-us"),&fmt2);
        D2Rect tr2={340,230,620,270}; const char*t3="Georgia italic, from memory";
        ((void(MSABI*)(void*,const uint16_t*,uint32_t,void*,const D2Rect*,void*,uint32_t,uint32_t))VT(ctx,27))(ctx,d2t_w(t3),(uint32_t)strlen(t3),fmt2,&tr2,green,0,0);
        d2_release(fmt2); d2_release(coll); d2_release(set); d2_release(file); d2_release(bld); d2_release(ldr); free(fb); }
    // WIC: a PNG made here -> stream -> decoder -> frame -> converter (32bppPBGRA) -> CreateBitmapFromWicBitmap -> DrawBitmap
    { uint8_t px[64*64*4]; CGContextRef pc=CGBitmapContextCreate(px,64,64,8,256,d2_srgb(),kCGImageAlphaPremultipliedLast);
        CGContextSetRGBFillColor(pc,0,0.5,1,1); CGContextFillRect(pc,CGRectMake(0,0,64,64)); CGImageRef im=CGBitmapContextCreateImage(pc); CGContextRelease(pc);
        CFMutableDataRef png=CFDataCreateMutable(0,0); CGImageDestinationRef ds=CGImageDestinationCreateWithData(png,CFSTR("public.png"),1,0); CGImageDestinationAddImage(ds,im,0); CGImageDestinationFinalize(ds); CFRelease(ds); CGImageRelease(im);
        GUID_ clsid={0x317d06e8,0x5f24,0x433d,{0xbd,0xf7,0x79,0xce,0x68,0xd8,0xab,0xc2}}; void*wf=0,*stm=0,*dec=0,*frm=0,*cnv=0,*wb=0;
        setenv("PELOAD_WIC","1",0); wic_create(&clsid,&IID_IWICImagingFactory,&wf); ((int(MSABI*)(void*,void**))VT(wf,14))(wf,&stm);
        ((int(MSABI*)(void*,const uint8_t*,uint32_t))VT(stm,16))(stm,CFDataGetBytePtr(png),(uint32_t)CFDataGetLength(png));
        ((int(MSABI*)(void*,void*,const GUID_*,int,void**))VT(wf,4))(wf,stm,0,1,&dec); if(dec) ((int(MSABI*)(void*,uint32_t,void**))VT(dec,13))(dec,0,&frm);
        ((int(MSABI*)(void*,void**))VT(wf,10))(wf,&cnv); int hr=((int(MSABI*)(void*,void*,const GUID_*,int,void*,double,int))VT(cnv,8))(cnv,frm,&GUID_WIC_PBGRA,0,0,0.0,0);
        ((int(MSABI*)(void*,void*,const D2BmpProps*,void**))VT(ctx,5))(ctx,cnv,0,&wb); printf("     WIC: %ld PNG bytes, decoder %p, converter 0x%08x, bitmap %p\n",(long)CFDataGetLength(png),dec,(unsigned)hr,wb);
        D2Rect dr={540,280,604,344}; if(wb) ((void(MSABI*)(void*,void*,const D2Rect*,float,uint32_t,const D2Rect*))VT(ctx,26))(ctx,wb,&dr,1.0f,1,0);
        d2_release(wb); d2_release(cnv); d2_release(frm); d2_release(dec); d2_release(stm); d2_release(wf); CFRelease(png); }
    int hr=((int(MSABI*)(void*,uint64_t*,uint64_t*))VT(ctx,49))(ctx,0,0);
    D2Bmp*b=bmp; printf("EndDraw 0x%08x\n",(unsigned)hr);
    d2t_check("FillRectangle (red)",b,70,60,255,0,0,2); d2t_check("DrawRectangle inside (background)",b,190,60,13,20,38,3); d2t_check("DrawRectangle edge (red)",b,190,21,255,0,0,40);
    d2t_check("radial gradient centre (yellow)",b,430,60,255,255,0,30); d2t_check("linear gradient start corner",b,266,26,255,240,15,40);
    d2t_check("path geometry inside (green)",b,540,80,0,255,0,2); d2t_check("axis-aligned clip inside (green)",b,60,175,0,255,0,2);
    d2t_check("axis-aligned clip outside",b,110,165,13,20,38,3); d2t_check("opacity layer (50% white)",b,170,175,134,137,146,4);
    d2t_check("WIC bitmap (0,128,255)",b,572,312,0,128,255,3);
    int tb=d2t_bright(b,240,150,620,200), lb=d2t_bright(b,20,220,320,340); if(tb<80) g_d2t_fail++; if(lb<80) g_d2t_fail++;
    printf("%s DrawText pixels lit: %d\n%s DrawTextLayout pixels lit: %d\n",tb>=80?"PASS":"FAIL",tb,lb>=80?"PASS":"FAIL",lb);
    if(out && *out){ int W=(int)b->w, H=(int)b->hh; uint8_t*rgba=malloc((size_t)W*H*4);
        for(int y=0;y<H;y++) for(int x=0;x<W;x++){ const uint8_t*s=b->px+(size_t)(H-1-y)*b->stride+(size_t)x*4; uint8_t*d=rgba+((size_t)y*W+x)*4; d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=255; }
        bmp_write_rgba(out,W,H,rgba); free(rgba); printf("picture: %s\n",out); }
    // the texture path (PELOAD_D3DGL=1; the script runs it in-process and through the GL helper, offscreen): a render-target texture
    // as the target -- GL clear, canvas shapes and text composited at EndDraw, a texture drawn as a GPU quad under a translucent canvas
    // rectangle -- read back from GL; the target stands in for the back buffer, so the present latch must pick it
    if(d3dgl_enabled()){ d3d_init(); D3DObj*rt=mktex(640,360,1,28,2), *vid=mktex(64,64,1,87,2); rt->bindf=0x28; vid->bindf=0x8; g_d3d_backbuf=rt;
        for(int i=0;i<64*64;i++){ uint8_t*q=(uint8_t*)vid->data+i*4; int on=((i%64)/8+(i/64)/8)&1; q[0]=on?0:255; q[1]=on?128:64; q[2]=on?255:0; q[3]=255; } vid->gl_dirty=1;
        void*c2=0,*tb=0,*vb=0,*half=0; ((int(MSABI*)(void*,uint32_t,void**))VT(dev,4))(dev,0,&c2);
        void*surf=0; uint32_t sd[5]={0}; ((int(MSABI*)(void*,const GUID_*,void**))VT(rt,0))(rt,&IID_IDXGISurface,&surf);   // (what Gravemind does: the target via IDXGISurface)
        if(surf) ((int(MSABI*)(void*,uint32_t*))VT(surf,8))(surf,sd); int okd=surf && surf!=(void*)rt && sd[0]==640 && sd[1]==360 && sd[2]==28; if(!okd) g_d2t_fail++;
        printf("%s IDXGISurface::GetDesc of the target: %ux%u format %u\n",okd?"PASS":"FAIL",sd[0],sd[1],sd[2]);
        D2BmpProps1 tp={{0,1},96,96,1|2,0}; ((int(MSABI*)(void*,void*,const D2BmpProps1*,void**))VT(c2,62))(c2,surf?surf:(void*)rt,&tp,&tb); if(surf) d2_release(surf);
        D2BmpProps1 vp={{0,3},96,96,0,0}; ((int(MSABI*)(void*,void*,const D2BmpProps1*,void**))VT(c2,62))(c2,vid,&vp,&vb);
        D2Col ch={1,1,1,0.5f}; ((mkbrush)VT(c2,8))(c2,&ch,0,&half);
        ((void(MSABI*)(void*,void*))VT(c2,74))(c2,tb); ((void(MSABI*)(void*))VT(c2,48))(c2); ((void(MSABI*)(void*,const D2Col*))VT(c2,47))(c2,&bg);
        ((void(MSABI*)(void*,const D2Rect*,void*))VT(c2,17))(c2,&r1,red);
        D2Rect vd={200,100,328,228}; ((void(MSABI*)(void*,void*,const D2Rect*,float,uint32_t,const D2Rect*))VT(c2,26))(c2,vb,&vd,1.0f,0,0);
        D2Rect ov={300,120,420,200}; ((void(MSABI*)(void*,const D2Rect*,void*))VT(c2,17))(c2,&ov,half);
        ((void(MSABI*)(void*,const uint16_t*,uint32_t,void*,const D2Rect*,void*,uint32_t,uint32_t))VT(c2,27))(c2,d2t_w(t1),(uint32_t)strlen(t1),fmt,&tr,white,0,0);
        int hr2=((int(MSABI*)(void*,uint64_t*,uint64_t*))VT(c2,49))(c2,0,0);
        uint8_t*px=d2_tex_read(rt); D2Bmp tmp; memset(&tmp,0,sizeof tmp); tmp.px=px; tmp.w=640; tmp.hh=360; tmp.stride=640*4; int f0=g_d2t_fail;
        printf("texture target: EndDraw 0x%08x, present latch %s\n",(unsigned)hr2,g_d3dgl_present_tex==rt->gltex&&rt->gltex?"on it":"NOT on it"); if(!(g_d3dgl_present_tex==rt->gltex&&rt->gltex)) g_d2t_fail++;
        if(px){ d2t_check("GPU: Clear (background)",&tmp,600,340,13,20,38,3); d2t_check("GPU: canvas FillRectangle (red)",&tmp,70,60,255,0,0,2);
            d2t_check("GPU: texture quad, blue cell",&tmp,204,104,0,64,255,3); d2t_check("GPU: texture quad, orange cell",&tmp,220,104,255,128,0,3);
            d2t_check("GPU: canvas over quad (50% white)",&tmp,310,130,255,191,127,4); d2t_check("GPU: canvas over clear (50% white)",&tmp,400,190,134,137,146,4);
            int lit=d2t_bright(&tmp,240,150,620,200); if(lit<80) g_d2t_fail++; printf("%s GPU: DrawText pixels lit: %d\n",lit>=80?"PASS":"FAIL",lit);
            if(out && *out){ char p2[1200]; snprintf(p2,sizeof p2,"%s",out); char*dot=strrchr(p2,'.'); if(dot) *dot=0; strncat(p2,"_gpu.bmp",sizeof p2-strlen(p2)-1);
                uint8_t*rgba=malloc(640*360*4); for(int y=0;y<360;y++) for(int x=0;x<640;x++){ const uint8_t*q=px+((size_t)(359-y)*640+x)*4; uint8_t*d=rgba+((size_t)y*640+x)*4; d[0]=q[2]; d[1]=q[1]; d[2]=q[0]; d[3]=255; }
                bmp_write_rgba(p2,640,360,rgba); free(rgba); printf("picture: %s\n",p2); } }
        else { g_d2t_fail++; printf("FAIL texture read back\n"); }
        printf("texture path: %d failed\n",g_d2t_fail-f0); free(px); d2_release(half); d2_release(vb); d2_release(tb); d2_release(c2); }
    else printf("(texture path skipped: PELOAD_D3DGL not set)\n");
    d2_release(lay); d2_release(fmt); d2_release(layer); d2_release(ss); d2_release(path); d2_release(rad); d2_release(lin); d2_release(stops);
    d2_release(green); d2_release(white); d2_release(red); d2_release(bmp); d2_release(ctx); d2_release(dev); d2_release(f);
    printf("%s (%d failed)\n",g_d2t_fail?"FAILED":"ALL PASSED",g_d2t_fail); fflush(stdout); return g_d2t_fail?1:0; }
