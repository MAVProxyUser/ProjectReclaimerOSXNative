// wic.c -- the Windows Imaging Component (windowscodecs) subset Direct2D programs load pictures with, on ImageIO (2026-10-21).
// Included by d2d.c (it uses d2d.c's COM object base). Project Gravemind loads its pictures (embedded PNG resources, the logo it
// downloads) as: CoCreateInstance(CLSID_WICImagingFactory2) -> CreateStream + InitializeFromMemory -> CreateDecoderFromStream ->
// GetFrame(0) -> CreateFormatConverter + Initialize(32bppPBGRA) -> ID2D1RenderTarget::CreateBitmapFromWicBitmap.
//   Decoders decode the whole first frame at creation (CGImageSource: PNG, JPEG, BMP, GIF, TIFF, ICO ...) into premultiplied BGRA
//   and report 32bppBGRA like WIC's PNG decoder (CopyPixels un-premultiplies); converters and bitmaps hold their own format.
//   Pixel formats: 32bpp BGRA / PBGRA / BGR / RGBA / PRGBA, any to any. Other WIC parts (encoders, metadata, palettes, colour
//   contexts) answer E_NOTIMPL through d2d.c's logging slots.
//   Per game (games.c): on for h2. halo3.dll also asks for CLSID_WICImagingFactory2 and has only ever been told "not registered" --
//   a partial WIC could take it down an untested path, so h3 / reach keep CLASSNOTREG. PELOAD_WIC=1 = on for any game, =0 = off.
#include <ImageIO/ImageIO.h>
#define WIC_FMT(last) {0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,last}}
static const GUID_ GUID_WIC_BGRA=WIC_FMT(0x0f), GUID_WIC_PBGRA=WIC_FMT(0x10), GUID_WIC_BGR32=WIC_FMT(0x0e),
    GUID_WIC_RGBA={0xf5c7ad2d,0x6a8d,0x43dd,{0xa7,0xa8,0xa2,0x99,0x35,0x26,0x1a,0xe9}}, GUID_WIC_PRGBA={0x3cc4a650,0xa527,0x4d37,{0xa9,0x16,0x31,0x42,0xc7,0xeb,0xed,0xba}};
enum { WK_BGRA, WK_PBGRA, WK_BGR32, WK_RGBA, WK_PRGBA };
static int wic_kind(const GUID_*g){ if(!g) return -1; const GUID_*t[5]={&GUID_WIC_BGRA,&GUID_WIC_PBGRA,&GUID_WIC_BGR32,&GUID_WIC_RGBA,&GUID_WIC_PRGBA};
    for(int i=0;i<5;i++) if(!memcmp(g,t[i],16)) return i; return -1; }
static const GUID_* wic_guid(int k){ const GUID_*t[5]={&GUID_WIC_BGRA,&GUID_WIC_PBGRA,&GUID_WIC_BGR32,&GUID_WIC_RGBA,&GUID_WIC_PRGBA}; return (k>=0&&k<5)?t[k]:&GUID_WIC_BGRA; }
#define WINCODEC_ERR_PALETTEUNAVAILABLE 0x88982F45
#define WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT 0x88982F80
#define WINCODEC_ERR_COMPONENTNOTFOUND 0x88982F50
// one pixel between two of the 32-bit formats (pm_src: the source bytes are premultiplied even if its kind says straight)
static void wic_px(const uint8_t*s,int sk,int pm_src,uint8_t*d,int dk){
    uint8_t b,g,r,a; if(sk==WK_RGBA||sk==WK_PRGBA){ r=s[0]; g=s[1]; b=s[2]; } else { b=s[0]; g=s[1]; r=s[2]; } a=(sk==WK_BGR32)?255:s[3];
    int spm=pm_src||sk==WK_PBGRA||sk==WK_PRGBA, dpm=(dk==WK_PBGRA||dk==WK_PRGBA);
    if(spm && !dpm && a && a<255){ r=(uint8_t)((r*255+a/2)/a); g=(uint8_t)((g*255+a/2)/a); b=(uint8_t)((b*255+a/2)/a); }
    else if(!spm && dpm && a<255){ r=(uint8_t)((r*a+127)/255); g=(uint8_t)((g*a+127)/255); b=(uint8_t)((b*a+127)/255); }
    if(dk==WK_BGR32) a=255;
    if(dk==WK_RGBA||dk==WK_PRGBA){ d[0]=r; d[1]=g; d[2]=b; } else { d[0]=b; d[1]=g; d[2]=r; } d[3]=a; }
// a picture in memory: decoder frames, converters, scalers and bitmaps are all this (w x h, 4 bytes a pixel, kind; pm = the bytes
// are premultiplied although the kind is straight: decoded frames)
typedef struct { D2Hdr h; uint32_t w,hh,stride; int kind, pm; uint8_t*px; } WicImg;
typedef struct { D2Hdr h; WicImg*frame; } WicDec;
typedef struct { D2Hdr h; const uint8_t*mem; uint64_t size, pos; uint8_t*own; } WicStream;
typedef struct { D2Hdr h; WicImg*img; } WicLock;
static void wic_img_dtor(void*p){ free(((WicImg*)p)->px); }
static void wic_dec_dtor(void*p){ WicDec*d=p; if(d->frame) d2_release(d->frame); }
static void wic_stream_dtor(void*p){ free(((WicStream*)p)->own); }
static void wic_lock_dtor(void*p){ WicLock*l=p; if(l->img) d2_release(l->img); }
static D2Cls C_wicfac, C_wicframe, C_wicconv, C_wicscaler, C_wicbmp, C_wicdec, C_wicstream, C_wiclock;
static int wic_is_img(void*p){ D2Hdr*h=p; return h && (h->cls==&C_wicframe||h->cls==&C_wicconv||h->cls==&C_wicscaler||h->cls==&C_wicbmp); }
static WicImg* wic_img_new(D2Cls*c,uint32_t w,uint32_t h,int kind){ WicImg*m=d2_alloc(c,sizeof(WicImg)); m->w=w; m->hh=h; m->kind=kind; m->stride=w*4;
    m->px=calloc(1,(size_t)w*h*4+16); return m; }
// the pixels of ANY IWICBitmapSource -- ours directly, a foreign one through its vtable -- as kind `want` (tightly packed, malloc'd)
static uint8_t* wic_pull(void*src,int want,uint32_t*pw,uint32_t*ph){
    if(!src) return 0;
    if(wic_is_img(src)){ WicImg*m=src; uint8_t*o=malloc((size_t)m->w*m->hh*4+16); if(!o) return 0;
        for(uint32_t y=0;y<m->hh;y++) for(uint32_t x=0;x<m->w;x++) wic_px(m->px+y*m->stride+x*4,m->kind,m->pm,o+((size_t)y*m->w+x)*4,want);
        *pw=m->w; *ph=m->hh; return o; }
    void**vt=*(void***)src; uint32_t w=0,h=0; GUID_ g; memset(&g,0,sizeof g);
    if(((int(MSABI*)(void*,uint32_t*,uint32_t*))vt[3])(src,&w,&h)<0 || !w || !h) return 0;
    ((int(MSABI*)(void*,GUID_*))vt[4])(src,&g); int k=wic_kind(&g);
    if(k<0){ char m[160]; int n=snprintf(m,sizeof m,"  [wic] source pixel format %08x-...-%02x not converted\n",g.D1,g.D4[7]); (void)!write(2,m,n); return 0; }
    uint8_t*raw=malloc((size_t)w*h*4+16), *o=malloc((size_t)w*h*4+16); if(!raw||!o){ free(raw); free(o); return 0; }
    if(((int(MSABI*)(void*,const void*,uint32_t,uint32_t,uint8_t*))vt[7])(src,0,w*4,w*h*4,raw)<0){ free(raw); free(o); return 0; }
    for(size_t i=0;i<(size_t)w*h;i++) wic_px(raw+i*4,k,0,o+i*4,want);
    free(raw); *pw=w; *ph=h; return o; }
// ---- IWICBitmapSource (3-7) for every picture
static int MSABI wic_GetSize(void*s,uint32_t*w,uint32_t*h){ WicImg*m=s; if(w)*w=m->w; if(h)*h=m->hh; return 0; }
static int MSABI wic_GetPixelFormat(void*s,GUID_*g){ if(g) *g=*wic_guid(((WicImg*)s)->kind); return 0; }
static int MSABI wic_GetResolution(void*s,double*x,double*y){ (void)s; if(x)*x=96; if(y)*y=96; return 0; }
static int MSABI wic_CopyPalette(void*s,void*p){ (void)s;(void)p; return (int)WINCODEC_ERR_PALETTEUNAVAILABLE; }
static int MSABI wic_CopyPixels(void*s,const int32_t*rc,uint32_t stride,uint32_t size,uint8_t*buf){ WicImg*m=s;
    int32_t x0=0,y0=0,w=(int32_t)m->w,h=(int32_t)m->hh; if(rc){ x0=rc[0]; y0=rc[1]; w=rc[2]; h=rc[3]; }
    if(x0<0||y0<0||w<0||h<0||x0+w>(int32_t)m->w||y0+h>(int32_t)m->hh||!buf) return (int)HR_INVALIDARG;
    if(h && ((uint64_t)stride*(h-1)+(uint64_t)w*4>size || stride<(uint32_t)w*4)) return (int)HR_INVALIDARG;
    for(int32_t y=0;y<h;y++){ const uint8_t*sr=m->px+(size_t)(y0+y)*m->stride+(size_t)x0*4; uint8_t*dr=buf+(size_t)y*stride;
        if(!m->pm) memcpy(dr,sr,(size_t)w*4); else for(int32_t x=0;x<w;x++) wic_px(sr+x*4,m->kind,1,dr+x*4,m->kind); }
    return 0; }
static int MSABI wic_noquery(void*s,void**pp){ (void)s; if(pp)*pp=0; return (int)0x88982F90; }   // WINCODEC_ERR_UNSUPPORTEDOPERATION
// ---- IWICFormatConverter (8 Initialize, 9 CanConvert)
static int MSABI wicconv_Initialize(void*s,void*src,const GUID_*dst,int dither,void*pal,double thr,int ptype){ (void)dither;(void)pal;(void)thr;(void)ptype;
    WicImg*m=s; int k=wic_kind(dst); if(k<0) return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    uint32_t w,h; uint8_t*px=wic_pull(src,k,&w,&h); if(!px) return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    free(m->px); m->px=px; m->w=w; m->hh=h; m->stride=w*4; m->kind=k; m->pm=0; return 0; }
static int MSABI wicconv_CanConvert(void*s,const GUID_*a,const GUID_*b,int*ok){ (void)s; if(ok)*ok=wic_kind(a)>=0&&wic_kind(b)>=0; return 0; }
// ---- IWICBitmapScaler (8 Initialize(source, w, h, mode))
static int MSABI wicscale_Initialize(void*s,void*src,uint32_t w,uint32_t h,int mode){ WicImg*m=s; uint32_t sw,sh; if(!w||!h) return (int)HR_INVALIDARG;
    uint8_t*px=wic_pull(src,WK_PBGRA,&sw,&sh); if(!px) return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    uint8_t*o=calloc(1,(size_t)w*h*4+16); CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef sc=CGBitmapContextCreate(px,sw,sh,8,sw*4,cs,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
    CGContextRef dc=CGBitmapContextCreate(o,w,h,8,w*4,cs,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
    if(sc&&dc){ CGImageRef im=CGBitmapContextCreateImage(sc); CGContextSetInterpolationQuality(dc,mode==0?kCGInterpolationNone:kCGInterpolationHigh);
        CGContextSetBlendMode(dc,kCGBlendModeCopy); CGContextDrawImage(dc,CGRectMake(0,0,w,h),im); CGImageRelease(im); }
    if(sc) CGContextRelease(sc); if(dc) CGContextRelease(dc); CGColorSpaceRelease(cs); free(px);
    free(m->px); m->px=o; m->w=w; m->hh=h; m->stride=w*4; m->kind=WK_PBGRA; m->pm=0; return 0; }   // (always PBGRA: what CG scales in)
// ---- IWICBitmap (8 Lock, 9 SetPalette, 10 SetResolution) + IWICBitmapLock (3 GetSize, 4 GetStride, 5 GetDataPointer, 6 GetPixelFormat)
static int MSABI wicbmp_Lock(void*s,const int32_t*rc,uint32_t fl,void**pp){ (void)rc;(void)fl; WicLock*l=d2_alloc(&C_wiclock,sizeof(WicLock)); l->img=s; d2_addref(s); *pp=l; return 0; }
static int MSABI wicbmp_SetResolution(void*s,double x,double y){ (void)s;(void)x;(void)y; return 0; }
static int MSABI wiclock_GetSize(void*s,uint32_t*w,uint32_t*h){ WicLock*l=s; return wic_GetSize(l->img,w,h); }
static int MSABI wiclock_GetStride(void*s,uint32_t*st){ WicLock*l=s; if(st)*st=l->img->stride; return 0; }
static int MSABI wiclock_GetDataPointer(void*s,uint32_t*sz,uint8_t**p){ WicLock*l=s; if(sz)*sz=l->img->stride*l->img->hh; if(p)*p=l->img->px; return 0; }
static int MSABI wiclock_GetPixelFormat(void*s,GUID_*g){ WicLock*l=s; return wic_GetPixelFormat(l->img,g); }
// ---- IWICBitmapDecoder (3 QueryCapability, 5 GetContainerFormat, 12 GetFrameCount, 13 GetFrame)
static int MSABI wicdec_QueryCapability(void*s,void*st,uint32_t*cap){ (void)s;(void)st; if(cap)*cap=4|8; return 0; }   // CanDecodeAllImages|CanDecodeSomeImages
static int MSABI wicdec_GetFrameCount(void*s,uint32_t*n){ (void)s; if(n)*n=1; return 0; }
static int MSABI wicdec_GetFrame(void*s,uint32_t i,void**pp){ WicDec*d=s; if(i!=0||!d->frame){ if(pp)*pp=0; return (int)HR_INVALIDARG; }
    d2_addref(d->frame); *pp=d->frame; return 0; }
// decode an encoded picture (any ImageIO format) into a frame
static WicDec* wic_decode(const uint8_t*mem,uint64_t size){
    if(!mem||!size) return 0;
    CFDataRef data=CFDataCreate(0,mem,(CFIndex)size); CGImageSourceRef isrc=data?CGImageSourceCreateWithData(data,0):0;
    CGImageRef im=isrc?CGImageSourceCreateImageAtIndex(isrc,0,0):0; WicDec*d=0;
    if(im){ uint32_t w=(uint32_t)CGImageGetWidth(im), h=(uint32_t)CGImageGetHeight(im);
        if(w && h && w<=32768 && h<=32768){ WicImg*f=wic_img_new(&C_wicframe,w,h,WK_BGRA); f->pm=1;
            CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef c=CGBitmapContextCreate(f->px,w,h,8,w*4,cs,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
            if(c){ CGContextSetBlendMode(c,kCGBlendModeCopy); CGContextDrawImage(c,CGRectMake(0,0,w,h),im); CGContextRelease(c); }
            CGColorSpaceRelease(cs); d=d2_alloc(&C_wicdec,sizeof(WicDec)); d->frame=f; }
        CGImageRelease(im); }
    if(isrc) CFRelease(isrc); if(data) CFRelease(data);
    if(getenv("PELOAD_TRACE_D2D")){ char m[120]; int n=snprintf(m,sizeof m,"  [wic] decoded %llu bytes -> %ux%u\n",(unsigned long long)size,d?d->frame->w:0,d?d->frame->hh:0); (void)!write(2,m,n); }
    return d; }
// ---- IStream / IWICStream over memory (3 Read, 4 Write, 5 Seek, 12 Stat, 15 InitializeFromFilename, 16 InitializeFromMemory)
static int MSABI wicst_Read(void*s,void*buf,uint32_t n,uint32_t*got){ WicStream*t=s; uint64_t left=t->pos<t->size?t->size-t->pos:0; uint32_t k=(uint32_t)(n<left?n:left);
    if(k) memcpy(buf,t->mem+t->pos,k); t->pos+=k; if(got)*got=k; return k<n?1:0; }
static int MSABI wicst_Write(void*s,const void*buf,uint32_t n,uint32_t*put){ (void)s;(void)buf;(void)n; if(put)*put=0; return (int)0x80030005; }   // STG_E_ACCESSDENIED
static int MSABI wicst_Seek(void*s,int64_t off,uint32_t org,uint64_t*np){ WicStream*t=s; int64_t b=org==0?0:org==1?(int64_t)t->pos:(int64_t)t->size;
    if(b+off<0) return (int)HR_INVALIDARG; t->pos=(uint64_t)(b+off); if(np)*np=t->pos; return 0; }
static int MSABI wicst_Stat(void*s,uint8_t*st,uint32_t fl){ (void)fl; WicStream*t=s; if(!st) return (int)HR_INVALIDARG; memset(st,0,80);
    *(uint32_t*)(st+8)=2; *(uint64_t*)(st+16)=t->size; return 0; }   // STATSTG: pwcsName@0, type@8 (STGTY_STREAM), cbSize@16
static int MSABI wicst_InitializeFromMemory(void*s,const uint8_t*mem,uint32_t n){ WicStream*t=s; t->mem=mem; t->size=n; t->pos=0; return 0; }
static int MSABI wicst_InitializeFromFilename(void*s,const uint16_t*path,uint32_t acc){ (void)acc; WicStream*t=s; char p[2048]; win2unix(path,p,sizeof p);
    FILE*f=fopen(p,"rb"); if(!f) return (int)0x80070002; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*b=malloc(n>0?(size_t)n:1); if(n>0 && fread(b,1,(size_t)n,f)!=(size_t)n){ free(b); fclose(f); return (int)0x80004005; } fclose(f);
    free(t->own); t->own=b; t->mem=b; t->size=(uint64_t)(n>0?n:0); t->pos=0; return 0; }
// all bytes of any IStream (ours: its memory; a foreign one: Seek(0) + Read through its vtable)
static uint8_t* wic_stream_bytes(void*st,uint64_t*n,int*own){
    D2Hdr*h=st; if(h && h->cls==&C_wicstream){ WicStream*t=st; *n=t->size; *own=0; return (uint8_t*)t->mem; }
    void**vt=*(void***)st; uint64_t np=0; ((int(MSABI*)(void*,int64_t,uint32_t,uint64_t*))vt[5])(st,0,0,&np);
    size_t cap=1<<16, len=0; uint8_t*b=malloc(cap);
    for(;;){ if(len+65536>cap){ cap*=2; b=realloc(b,cap); } uint32_t got=0; int hr=((int(MSABI*)(void*,void*,uint32_t,uint32_t*))vt[3])(st,b+len,65536,&got);
        if(hr<0) break; len+=got; if(!got||hr==1) break; if(len>(256u<<20)) break; }
    *n=len; *own=1; return b; }
// ---- IWICImagingFactory (3 CreateDecoderFromFilename, 4 CreateDecoderFromStream, 10 CreateFormatConverter, 11 CreateBitmapScaler,
//      14 CreateStream, 17 CreateBitmap, 18 CreateBitmapFromSource, 20 CreateBitmapFromMemory)
static int MSABI wicfac_CreateDecoderFromFilename(void*s,const uint16_t*path,const GUID_*vendor,uint32_t acc,int opt,void**pp){ (void)s;(void)vendor;(void)opt;
    WicStream*t=d2_alloc(&C_wicstream,sizeof(WicStream)); int hr=wicst_InitializeFromFilename(t,path,acc);
    WicDec*d=hr>=0?wic_decode(t->mem,t->size):0; d2_release(t); *pp=d; return d?0:hr<0?hr:(int)WINCODEC_ERR_COMPONENTNOTFOUND; }
static int MSABI wicfac_CreateDecoderFromStream(void*s,void*st,const GUID_*vendor,int opt,void**pp){ (void)s;(void)vendor;(void)opt;
    if(!st||!pp) return (int)HR_INVALIDARG; uint64_t n=0; int own=0; uint8_t*b=wic_stream_bytes(st,&n,&own);
    WicDec*d=wic_decode(b,n); if(own) free(b); *pp=d; return d?0:(int)WINCODEC_ERR_COMPONENTNOTFOUND; }
static int MSABI wicfac_CreateFormatConverter(void*s,void**pp){ (void)s; *pp=wic_img_new(&C_wicconv,0,0,WK_PBGRA); return 0; }
static int MSABI wicfac_CreateBitmapScaler(void*s,void**pp){ (void)s; *pp=wic_img_new(&C_wicscaler,0,0,WK_PBGRA); return 0; }
static int MSABI wicfac_CreateStream(void*s,void**pp){ (void)s; *pp=d2_alloc(&C_wicstream,sizeof(WicStream)); return 0; }
static int MSABI wicfac_CreateBitmap(void*s,uint32_t w,uint32_t h,const GUID_*fmt,int opt,void**pp){ (void)s;(void)opt; int k=wic_kind(fmt);
    if(k<0||!w||!h){ *pp=0; return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; } *pp=wic_img_new(&C_wicbmp,w,h,k); return 0; }
static int MSABI wicfac_CreateBitmapFromSource(void*s,void*src,int opt,void**pp){ (void)s;(void)opt; uint32_t w,h; int k=WK_PBGRA;
    if(wic_is_img(src)) k=((WicImg*)src)->kind; uint8_t*px=wic_pull(src,k,&w,&h); if(!px){ *pp=0; return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    WicImg*m=wic_img_new(&C_wicbmp,0,0,k); free(m->px); m->px=px; m->w=w; m->hh=h; m->stride=w*4; *pp=m; return 0; }
static int MSABI wicfac_CreateBitmapFromMemory(void*s,uint32_t w,uint32_t h,const GUID_*fmt,uint32_t stride,uint32_t size,const uint8_t*buf,void**pp){ (void)s;
    int k=wic_kind(fmt); if(k<0||!w||!h||!buf||stride<w*4||(uint64_t)stride*(h-1)+w*4>size){ *pp=0; return (int)WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    WicImg*m=wic_img_new(&C_wicbmp,w,h,k); for(uint32_t y=0;y<h;y++) memcpy(m->px+(size_t)y*m->stride,buf+(size_t)y*stride,(size_t)w*4); *pp=m; return 0; }
static void wic_src_slots(void**v){ v[3]=wic_GetSize; v[4]=wic_GetPixelFormat; v[5]=wic_GetResolution; v[6]=wic_CopyPalette; v[7]=wic_CopyPixels; }
static void wic_setup_frame(void**v){ wic_src_slots(v); v[8]=wic_noquery; v[9]=wic_noquery; v[10]=wic_noquery; }
static void wic_setup_conv(void**v){ wic_src_slots(v); v[8]=wicconv_Initialize; v[9]=wicconv_CanConvert; }
static void wic_setup_scaler(void**v){ wic_src_slots(v); v[8]=wicscale_Initialize; }
static void wic_setup_bmp(void**v){ wic_src_slots(v); v[8]=wicbmp_Lock; v[10]=wicbmp_SetResolution; }
static void wic_setup_lock(void**v){ v[3]=wiclock_GetSize; v[4]=wiclock_GetStride; v[5]=wiclock_GetDataPointer; v[6]=wiclock_GetPixelFormat; }
static void wic_setup_dec(void**v){ v[3]=wicdec_QueryCapability; v[12]=wicdec_GetFrameCount; v[13]=wicdec_GetFrame; }
static void wic_setup_stream(void**v){ v[3]=wicst_Read; v[4]=wicst_Write; v[5]=wicst_Seek; v[12]=wicst_Stat; v[15]=wicst_InitializeFromFilename; v[16]=wicst_InitializeFromMemory; }
static void wic_setup_fac(void**v){ v[3]=wicfac_CreateDecoderFromFilename; v[4]=wicfac_CreateDecoderFromStream; v[10]=wicfac_CreateFormatConverter;
    v[11]=wicfac_CreateBitmapScaler; v[14]=wicfac_CreateStream; v[17]=wicfac_CreateBitmap; v[18]=wicfac_CreateBitmapFromSource; v[20]=wicfac_CreateBitmapFromMemory; }
static D2Cls C_wicfac={"IWICImagingFactory",SLOTS_IWICImagingFactory,NSLOT_IWICImagingFactory,{&IID_IUnknown,&IID_IWICImagingFactory},0,wic_setup_fac};
static D2Cls C_wicframe={"IWICBitmapFrameDecode",SLOTS_IWICBitmapFrameDecode,NSLOT_IWICBitmapFrameDecode,{&IID_IUnknown,&IID_IWICBitmapSource,&IID_IWICBitmapFrameDecode},wic_img_dtor,wic_setup_frame};
static D2Cls C_wicconv={"IWICFormatConverter",SLOTS_IWICFormatConverter,NSLOT_IWICFormatConverter,{&IID_IUnknown,&IID_IWICBitmapSource,&IID_IWICFormatConverter},wic_img_dtor,wic_setup_conv};
static D2Cls C_wicscaler={"IWICBitmapScaler",SLOTS_IWICBitmapScaler,NSLOT_IWICBitmapScaler,{&IID_IUnknown,&IID_IWICBitmapSource,&IID_IWICBitmapScaler},wic_img_dtor,wic_setup_scaler};
static D2Cls C_wicbmp={"IWICBitmap",SLOTS_IWICBitmap,NSLOT_IWICBitmap,{&IID_IUnknown,&IID_IWICBitmapSource,&IID_IWICBitmap},wic_img_dtor,wic_setup_bmp};
static D2Cls C_wiclock={"IWICBitmapLock",SLOTS_IWICBitmapLock,NSLOT_IWICBitmapLock,{&IID_IUnknown,&IID_IWICBitmapLock},wic_lock_dtor,wic_setup_lock};
static D2Cls C_wicdec={"IWICBitmapDecoder",SLOTS_IWICBitmapDecoder,NSLOT_IWICBitmapDecoder,{&IID_IUnknown,&IID_IWICBitmapDecoder},wic_dec_dtor,wic_setup_dec};
static D2Cls C_wicstream={"IWICStream",SLOTS_IWICStream,NSLOT_IWICStream,{&IID_IUnknown,&IID_ISequentialStream,&IID_IStream,&IID_IWICStream},wic_stream_dtor,wic_setup_stream};
// CoCreateInstance: CLSID_WICImagingFactory {cacaf262-9370-4615-a13b-9f5539da4c0a} / CLSID_WICImagingFactory2 {317d06e8-5f24-433d-bdf7-79ce68d8abc2}
static int wic_create(const GUID_*clsid,const GUID_*iid,void**ppv){
    if(!clsid || !game_default_on("PELOAD_WIC",GAME_H2)) return 0;
    if(!(clsid->D1==0xcacaf262u && clsid->D2==0x9370) && !(clsid->D1==0x317d06e8u && clsid->D2==0x5f24)) return 0;
    void*f=d2_alloc(&C_wicfac,sizeof(D2Hdr)); int hr=d2_QI(f,iid,ppv); d2_release(f); return hr<0?hr:1; }
