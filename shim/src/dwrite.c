// dwrite.c -- DirectWrite on CoreText (2026-10-21). Included by d2d.c (it uses d2d.c's COM object base; Direct2D draws the layouts).
//   Factory: IDWriteFactory..IDWriteFactory5. Text formats and layouts (IDWriteTextFormat / IDWriteTextLayout) keep DirectWrite's
//   values; a layout is broken into lines with a CTTypesetter (word / character wrapping, hard line breaks), aligned (leading /
//   trailing / centre, near / far / centre paragraphs), trimmed (character / word granularity, with the ellipsis sign) and measured
//   (GetMetrics, GetLineMetrics, GetClusterMetrics, DetermineMinWidth, the three HitTests). Per-range font family / collection /
//   weight / style / size / underline / strikethrough / drawing effect (a solid brush colours its range); IDWriteTextLayout1's pair
//   kerning and character spacing (as extra advance).
//   Fonts: in-memory font files (IDWriteInMemoryFontFileLoader -> IDWriteFontSetBuilder1 -> CreateFontCollectionFromFontSet: how
//   Project Gravemind ships Michroma, Halo 2 Handel and Halo 2 Conduit) become CGFonts, matched by family name (+ nearest weight /
//   italic) in their collection; the system collection = the font files staged in C:\ReclaimerFonts (matched by their own family
//   names: a genuine bahnschrift.ttf / segoeui.ttf; a variable font's weight axis follows the weight) + macOS's fonts, with the missing
//   Windows families mapped: Bahnschrift -> Avenir Next Condensed, Segoe UI -> Helvetica Neue, Consolas -> Menlo, any other ->
//   Helvetica Neue. PELOAD_DWRITE_FONT=<family> forces one.
//   Not done: custom IDWriteTextRenderer drawing (Draw), glyph-run analysis, font faces / families as objects (E_NOTIMPL, logged).
typedef struct { uint32_t start, len; } DwRng;
typedef struct DwFace { char fam[96], full[96], ps[96]; int weight, italic; CGFontRef cg; struct DwFace*next; } DwFace;
static DwFace* g_dw_faces; static pthread_mutex_t g_dw_mx=PTHREAD_MUTEX_INITIALIZER;
typedef struct { D2Hdr h; int system; DwFace**faces; int n; } DwColl;
typedef struct { D2Hdr h; DwFace*face; } DwFile;
typedef struct { D2Hdr h; DwFace**faces; int n, cap; } DwSet;   // IDWriteFontSet and IDWriteFontSetBuilder1
typedef struct { D2Hdr h; int ellipsis, nfiles; } DwObj;        // rendering params, typography, the ellipsis sign, the in-memory loader
static D2Cls C_dwfac, C_dwcoll, C_dwfile, C_dwset, C_dwbuilder, C_dwmemloader, C_dwfmt, C_dwlayout, C_dwparams, C_dwtypo, C_dwinline;
static int d2_brush_solid_color(void*brush,float rgba[4]);   // d2d.c: a solid colour brush's colour x its opacity (drawing effects)
static char* dw_u8(const uint16_t*w,int n,char*out,int cap){ int k=0; for(int i=0;w&&(n<0?w[i]!=0:i<n)&&k<cap-4;i++){ unsigned c=w[i];
        if(c>=0xD800&&c<0xDC00&&w[i+1]>=0xDC00&&w[i+1]<0xE000){ c=0x10000+((c-0xD800)<<10)+(w[i+1]-0xDC00); i++; }
        if(c<0x80) out[k++]=(char)c; else if(c<0x800){ out[k++]=(char)(0xC0|c>>6); out[k++]=(char)(0x80|(c&63)); }
        else if(c<0x10000){ out[k++]=(char)(0xE0|c>>12); out[k++]=(char)(0x80|((c>>6)&63)); out[k++]=(char)(0x80|(c&63)); }
        else { out[k++]=(char)(0xF0|c>>18); out[k++]=(char)(0x80|((c>>12)&63)); out[k++]=(char)(0x80|((c>>6)&63)); out[k++]=(char)(0x80|(c&63)); } }
    out[k]=0; return out; }
static int dw_wlen(const uint16_t*w){ int n=0; while(w&&w[n]) n++; return n; }
static void dw_wcpy(uint16_t*d,int cap,const uint16_t*s){ int i=0; for(;s&&s[i]&&i<cap-1;i++) d[i]=s[i]; d[i]=0; }
static void dw_cfstr(CFStringRef s,char*out,int cap){ out[0]=0; if(s){ CFStringGetCString(s,out,cap,kCFStringEncodingUTF8); CFRelease(s); } }
// ---- fonts ----------------------------------------------------------------------------------------------------------------
// a font file in memory -> a face (kept for the life of the process: a few per game)
static DwFace* dw_face_from_data(const uint8_t*p,uint32_t n){
    if(!p||!n) return 0; CFDataRef d=CFDataCreate(0,p,n); CGDataProviderRef dp=d?CGDataProviderCreateWithCFData(d):0; if(d) CFRelease(d);
    CGFontRef cg=dp?CGFontCreateWithDataProvider(dp):0; if(dp) CGDataProviderRelease(dp); if(!cg) return 0;
    DwFace*f=calloc(1,sizeof *f); f->cg=cg; CTFontRef ct=CTFontCreateWithGraphicsFont(cg,12,0,0);
    dw_cfstr(CTFontCopyFamilyName(ct),f->fam,sizeof f->fam); dw_cfstr(CTFontCopyFullName(ct),f->full,sizeof f->full); dw_cfstr(CTFontCopyPostScriptName(ct),f->ps,sizeof f->ps);
    CFDictionaryRef tr=CTFontCopyTraits(ct); double wt=0; CTFontSymbolicTraits st=CTFontGetSymbolicTraits(ct);
    if(tr){ CFNumberRef w=CFDictionaryGetValue(tr,kCTFontWeightTrait); if(w) CFNumberGetValue(w,kCFNumberDoubleType,&wt); CFRelease(tr); }
    f->weight=wt<=-0.7?100:wt<=-0.5?200:wt<=-0.3?300:wt<0.15?400:wt<0.27?500:wt<0.35?600:wt<0.5?700:wt<0.6?800:900; f->italic=(st&kCTFontItalicTrait)!=0;
    CFRelease(ct);
    pthread_mutex_lock(&g_dw_mx); f->next=g_dw_faces; g_dw_faces=f; pthread_mutex_unlock(&g_dw_mx);
    if(getenv("PELOAD_TRACE_D2D")){ char m[400]; int k=snprintf(m,sizeof m,"  [dwrite] font file %u bytes: family '%s' (%s / %s) weight %d%s\n",n,f->fam,f->full,f->ps,f->weight,f->italic?" italic":""); (void)!write(2,m,k); }
    return f; }
static int dw_face_named(const DwFace*f,const char*fam){ return !strcasecmp(f->fam,fam)||!strcasecmp(f->full,fam)||!strcasecmp(f->ps,fam); }
// the collection's face of family `fam` closest to weight / italic (DirectWrite's font matching, simplified)
static DwFace* dw_coll_face(DwColl*c,const char*fam,int weight,int italic){
    DwFace*best=0; int bs=1<<30;
    for(int i=0;c&&i<c->n;i++){ DwFace*f=c->faces[i]; if(!dw_face_named(f,fam)) continue;
        int s=abs(f->weight-weight)+(f->italic!=italic?1000:0); if(s<bs){ bs=s; best=f; } }
    return best; }
static double dw_ctweight(int w){ return w<=150?-0.8:w<=250?-0.6:w<=325?-0.4:w<=375?-0.2:w<=450?0:w<=550?0.23:w<=650?0.3:w<=750?0.4:w<=850?0.56:0.62; }
static CTFontRef dw_ct_from_cg(CGFontRef cg,float size,int weight,int italic,int had_italic){
    CTFontDescriptorRef desc=0; CFArrayRef axes=CGFontCopyVariationAxes(cg);
    if(axes){ for(CFIndex i=0;i<CFArrayGetCount(axes);i++){ CFDictionaryRef ax=CFArrayGetValueAtIndex(axes,i); CFStringRef nm=CFDictionaryGetValue(ax,kCGFontVariationAxisName);
            if(nm && CFStringCompare(nm,CFSTR("Weight"),kCFCompareCaseInsensitive)==0){   // a variable font: its weight axis takes the DirectWrite weight
                CFNumberRef mn=CFDictionaryGetValue(ax,kCGFontVariationAxisMinValue), mx=CFDictionaryGetValue(ax,kCGFontVariationAxisMaxValue); double lo=100,hi=900,v=weight;
                if(mn) CFNumberGetValue(mn,kCFNumberDoubleType,&lo); if(mx) CFNumberGetValue(mx,kCFNumberDoubleType,&hi); if(v<lo)v=lo; if(v>hi)v=hi;
                CFNumberRef val=CFNumberCreate(0,kCFNumberDoubleType,&v); uint32_t tag='wght'; CFNumberRef key=CFNumberCreate(0,kCFNumberSInt32Type,&tag);
                CFDictionaryRef var=CFDictionaryCreate(0,(const void**)&key,(const void**)&val,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
                CFStringRef ak=kCTFontVariationAttribute; CFDictionaryRef da=CFDictionaryCreate(0,(const void**)&ak,(const void**)&var,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
                desc=CTFontDescriptorCreateWithAttributes(da); CFRelease(da); CFRelease(var); CFRelease(val); CFRelease(key); break; } }
        CFRelease(axes); }
    CGAffineTransform sk=CGAffineTransformMake(1,0,(italic&&!had_italic)?0.2:0,1,0,0);   // no italic face: slant it like DirectWrite's simulation
    CTFontRef f=CTFontCreateWithGraphicsFont(cg,size,&sk,desc); if(desc) CFRelease(desc); return f; }
static int dw_family_is(CTFontRef f,const char*want){ char b[128]; if(!f) return 0; dw_cfstr(CTFontCopyFamilyName(f),b,sizeof b); return !strcasecmp(b,want); }
static CTFontRef dw_ct_system(const char*fam,float size,int weight,int italic){
    CFStringRef nm=CFStringCreateWithCString(0,fam,kCFStringEncodingUTF8); if(!nm) return 0;
    double wv=dw_ctweight(weight); CFNumberRef wn=CFNumberCreate(0,kCFNumberDoubleType,&wv); CTFontSymbolicTraits sym=italic?kCTFontItalicTrait:0; CFNumberRef sn=CFNumberCreate(0,kCFNumberSInt32Type,&sym);
    const void*tk[2]={kCTFontWeightTrait,kCTFontSymbolicTrait}; const void*tv[2]={wn,sn};
    CFDictionaryRef traits=CFDictionaryCreate(0,tk,tv,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    const void*ak[2]={kCTFontFamilyNameAttribute,kCTFontTraitsAttribute}; const void*av[2]={nm,traits};
    CFDictionaryRef at=CFDictionaryCreate(0,ak,av,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef d=CTFontDescriptorCreateWithAttributes(at); CTFontRef f=d?CTFontCreateWithFontDescriptor(d,size,0):0;
    if(d) CFRelease(d); CFRelease(at); CFRelease(traits); CFRelease(wn); CFRelease(sn); CFRelease(nm);
    if(f && !dw_family_is(f,fam)){ CFRelease(f); f=0; } return f; }
// the font files staged in C:\ReclaimerFonts (genuine Windows fonts the user copied there: bahnschrift.ttf, segoeui.ttf ...) are part of
// the system collection under their own family names (a placeholder file named like another font never matches that font's family)
static DwFace* g_dw_staged; static int g_dw_staged_done;
static void dw_scan_staged(void){ if(g_dw_staged_done) return; g_dw_staged_done=1; if(!g_drivec[0]) return;
    char dir[1200]; snprintf(dir,sizeof dir,"%s/ReclaimerFonts",g_drivec); DIR*d=opendir(dir); if(!d) return; struct dirent*e;
    while((e=readdir(d))){ size_t n=strlen(e->d_name); if(n<5 || (strcasecmp(e->d_name+n-4,".ttf") && strcasecmp(e->d_name+n-4,".otf"))) continue;
        char p[1500]; snprintf(p,sizeof p,"%s/%s",dir,e->d_name); FILE*f=fopen(p,"rb"); if(!f) continue; fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        uint8_t*b=sz>0&&sz<(64<<20)?malloc((size_t)sz):0; if(b && fread(b,1,(size_t)sz,f)==(size_t)sz){
            CFDataRef cd=CFDataCreate(0,b,sz); CGDataProviderRef dp=CGDataProviderCreateWithCFData(cd); CFRelease(cd); CGFontRef cg=CGFontCreateWithDataProvider(dp); CGDataProviderRelease(dp);
            if(cg){ DwFace*fc=calloc(1,sizeof *fc); fc->cg=cg; CTFontRef ct=CTFontCreateWithGraphicsFont(cg,12,0,0); dw_cfstr(CTFontCopyFamilyName(ct),fc->fam,sizeof fc->fam);
                dw_cfstr(CTFontCopyFullName(ct),fc->full,sizeof fc->full); dw_cfstr(CTFontCopyPostScriptName(ct),fc->ps,sizeof fc->ps); fc->italic=(CTFontGetSymbolicTraits(ct)&kCTFontItalicTrait)!=0;
                CFDictionaryRef tr=CTFontCopyTraits(ct); double wt=0; if(tr){ CFNumberRef w=CFDictionaryGetValue(tr,kCTFontWeightTrait); if(w) CFNumberGetValue(w,kCFNumberDoubleType,&wt); CFRelease(tr); }
                fc->weight=wt<-0.3?300:wt<0.15?400:wt<0.27?500:wt<0.35?600:wt<0.5?700:900; CFRelease(ct); fc->next=g_dw_staged; g_dw_staged=fc;
                if(getenv("PELOAD_TRACE_D2D")){ char m[300]; int k=snprintf(m,sizeof m,"  [dwrite] staged font %s: family '%s'\n",e->d_name,fc->fam); (void)!write(2,m,k); } } }
        free(b); fclose(f); }
    closedir(d); }
// family + collection + weight / style / size -> a CTFont (cached: layouts ask for the same few fonts every frame)
static CTFontRef dw_font(DwColl*coll,const uint16_t*wfam,int weight,int style,float size){
    char fam[128]; dw_u8(wfam,-1,fam,sizeof fam); int italic=style!=0; if(size<=0) size=1; if(size>2000) size=2000;
    static struct { void*coll; char fam[64]; int w,it; float sz; CTFontRef f; } cache[256]; static int ncache, rr;
    pthread_mutex_lock(&g_dw_mx);
    for(int i=0;i<ncache;i++) if(cache[i].coll==coll && cache[i].w==weight && cache[i].it==italic && cache[i].sz==size && !strncmp(cache[i].fam,fam,63)){ CTFontRef f=cache[i].f; pthread_mutex_unlock(&g_dw_mx); return f; }
    pthread_mutex_unlock(&g_dw_mx);
    CTFontRef f=0; const char*force=getenv("PELOAD_DWRITE_FONT");
    if(force&&*force) f=dw_ct_system(force,size,weight,italic);
    if(!f && coll && !coll->system){ DwFace*fc=dw_coll_face(coll,fam,weight,italic); if(fc) f=dw_ct_from_cg(fc->cg,size,weight,italic,fc->italic); }
    if(!f){ pthread_mutex_lock(&g_dw_mx); DwFace*fc=0; for(DwFace*x=g_dw_faces;x;x=x->next) if(dw_face_named(x,fam) && (!fc||abs(x->weight-weight)<abs(fc->weight-weight))) fc=x; pthread_mutex_unlock(&g_dw_mx);
        if(fc && coll && coll->system) fc=0;   // (the system collection holds installed fonts only)
        if(fc) f=dw_ct_from_cg(fc->cg,size,weight,italic,fc->italic); }
    if(!f){ pthread_mutex_lock(&g_dw_mx); dw_scan_staged(); DwFace*fc=0; for(DwFace*x=g_dw_staged;x;x=x->next) if(!strcasecmp(x->fam,fam) && (!fc||abs(x->weight-weight)+(x->italic!=italic)*1000<abs(fc->weight-weight)+(fc->italic!=italic)*1000)) fc=x;
        pthread_mutex_unlock(&g_dw_mx); if(fc) f=dw_ct_from_cg(fc->cg,size,weight,italic,fc->italic); }
    if(!f && !strncasecmp(fam,"Bahnschrift",11)){ CGFontRef cg=ct_load_cgfont(); if(cg) f=dw_ct_from_cg(cg,size,weight,italic,0); }
    if(!f) f=dw_ct_system(fam,size,weight,italic);
    if(!f){ const char*sub=!strncasecmp(fam,"Bahnschrift",11)?"Avenir Next Condensed":!strncasecmp(fam,"Consolas",8)||!strncasecmp(fam,"Courier",7)?"Menlo":"Helvetica Neue";
        f=dw_ct_system(sub,size,weight,italic); if(!f) f=CTFontCreateWithName(CFSTR("Helvetica Neue"),size,0);
        static int nlog; if(nlog++<16){ char m[200]; int k=snprintf(m,sizeof m,"  [dwrite] font family '%s' -> %s\n",fam,sub); (void)!write(2,m,k); } }
    pthread_mutex_lock(&g_dw_mx); int slot=ncache<256?ncache++:(rr++&255); if(cache[slot].f) CFRelease(cache[slot].f);
    cache[slot].coll=coll; snprintf(cache[slot].fam,64,"%s",fam); cache[slot].w=weight; cache[slot].it=italic; cache[slot].sz=size; cache[slot].f=f; pthread_mutex_unlock(&g_dw_mx);
    return f; }
// ---- text formats and layouts -----------------------------------------------------------------------------------------------
typedef struct { uint32_t start, len; int kind; int i; float f, f2, f3; void*p; uint16_t*w; } DwRun;   // a per-range property of a layout
enum { DR_COLL=1, DR_FAM, DR_WEIGHT, DR_STYLE, DR_STRETCH, DR_SIZE, DR_UNDER, DR_STRIKE, DR_EFFECT, DR_INLINE, DR_TYPO, DR_LOCALE, DR_KERN, DR_SPACING };
typedef struct { CTLineRef line; CFIndex start, len, nl; double asc, desc, lead, h, w, wtrail, x, top, base; int trimmed; } DwLine;
typedef struct DwFmt { D2Hdr h; uint16_t fam[100]; DwColl*coll; int weight, style, stretch; float size; uint16_t locale[86];
    int talign, palign, wrap, rdir, fdir; float tab; uint32_t trim[3]; void*trimsign; int lsm; float lsp, lsb;
    // layouts only
    uint16_t*text; uint32_t len; float maxw, maxh; DwRun*runs; int nruns, cruns; DwLine*lines; int nlines, built; } DwFmt;
typedef DwFmt DwLayout;
static int dw_is_layout(void*p){ return p && ((D2Hdr*)p)->cls==&C_dwlayout; }
static int dw_is_fmt(void*p){ return p && (((D2Hdr*)p)->cls==&C_dwlayout || ((D2Hdr*)p)->cls==&C_dwfmt); }
static void dw_free_lines(DwFmt*L){ for(int i=0;i<L->nlines;i++) if(L->lines[i].line) CFRelease(L->lines[i].line); free(L->lines); L->lines=0; L->nlines=0; L->built=0; }
static void dw_fmt_dtor(void*p){ DwFmt*f=p; if(f->coll) d2_release(f->coll); if(f->trimsign) d2_release(f->trimsign); dw_free_lines(f); free(f->text);
    for(int i=0;i<f->nruns;i++){ free(f->runs[i].w); if((f->runs[i].kind==DR_EFFECT||f->runs[i].kind==DR_COLL||f->runs[i].kind==DR_INLINE||f->runs[i].kind==DR_TYPO) && f->runs[i].p) d2_release(f->runs[i].p); }
    free(f->runs); }
static void dw_dirty(void*s){ if(dw_is_layout(s)) dw_free_lines(s); }
// IDWriteTextFormat 3..27 (also a layout's)
static int MSABI dwf_SetTextAlignment(void*s,int a){ ((DwFmt*)s)->talign=a; dw_dirty(s); return 0; }
static int MSABI dwf_SetParagraphAlignment(void*s,int a){ ((DwFmt*)s)->palign=a; dw_dirty(s); return 0; }
static int MSABI dwf_SetWordWrapping(void*s,int a){ ((DwFmt*)s)->wrap=a; dw_dirty(s); return 0; }
static int MSABI dwf_SetReadingDirection(void*s,int a){ ((DwFmt*)s)->rdir=a; return 0; }
static int MSABI dwf_SetFlowDirection(void*s,int a){ ((DwFmt*)s)->fdir=a; return 0; }
static int MSABI dwf_SetIncrementalTabStop(void*s,float t){ ((DwFmt*)s)->tab=t; return 0; }
static int MSABI dwf_SetTrimming(void*s,const uint32_t*t,void*sign){ DwFmt*f=s; if(t) memcpy(f->trim,t,12); else memset(f->trim,0,12);
    if(sign) d2_addref(sign); if(f->trimsign) d2_release(f->trimsign); f->trimsign=sign; dw_dirty(s); return 0; }
static int MSABI dwf_SetLineSpacing(void*s,int m,float sp,float base){ DwFmt*f=s; f->lsm=m; f->lsp=sp; f->lsb=base; dw_dirty(s); return 0; }
static int MSABI dwf_GetTextAlignment(void*s){ return ((DwFmt*)s)->talign; }
static int MSABI dwf_GetParagraphAlignment(void*s){ return ((DwFmt*)s)->palign; }
static int MSABI dwf_GetWordWrapping(void*s){ return ((DwFmt*)s)->wrap; }
static int MSABI dwf_GetReadingDirection(void*s){ return ((DwFmt*)s)->rdir; }
static int MSABI dwf_GetFlowDirection(void*s){ return ((DwFmt*)s)->fdir; }
static float MSABI dwf_GetIncrementalTabStop(void*s){ return ((DwFmt*)s)->tab; }
static int MSABI dwf_GetTrimming(void*s,uint32_t*t,void**sign){ DwFmt*f=s; if(t) memcpy(t,f->trim,12); if(sign){ *sign=f->trimsign; if(f->trimsign) d2_addref(f->trimsign); } return 0; }
static int MSABI dwf_GetLineSpacing(void*s,int*m,float*sp,float*base){ DwFmt*f=s; if(m)*m=f->lsm; if(sp)*sp=f->lsp; if(base)*base=f->lsb; return 0; }
static int MSABI dwf_GetFontCollection(void*s,void**pp){ DwFmt*f=s; *pp=f->coll; if(f->coll) d2_addref(f->coll); return 0; }
static uint32_t MSABI dwf_GetFontFamilyNameLength(void*s){ return (uint32_t)dw_wlen(((DwFmt*)s)->fam); }
static int MSABI dwf_GetFontFamilyName(void*s,uint16_t*b,uint32_t n){ DwFmt*f=s; if(!b||n<=(uint32_t)dw_wlen(f->fam)) return (int)0x8007007A; dw_wcpy(b,(int)n,f->fam); return 0; }
static int MSABI dwf_GetFontWeight(void*s){ return ((DwFmt*)s)->weight; }
static int MSABI dwf_GetFontStyle(void*s){ return ((DwFmt*)s)->style; }
static int MSABI dwf_GetFontStretch(void*s){ return ((DwFmt*)s)->stretch; }
static float MSABI dwf_GetFontSize(void*s){ return ((DwFmt*)s)->size; }
static uint32_t MSABI dwf_GetLocaleNameLength(void*s){ return (uint32_t)dw_wlen(((DwFmt*)s)->locale); }
static int MSABI dwf_GetLocaleName(void*s,uint16_t*b,uint32_t n){ DwFmt*f=s; if(!b||n<=(uint32_t)dw_wlen(f->locale)) return (int)0x8007007A; dw_wcpy(b,(int)n,f->locale); return 0; }
// ---- layout properties by range
static DwRun* dw_addrun(DwLayout*L,int kind,DwRng r){ if(r.len==0) return 0; if(L->nruns==L->cruns){ L->cruns=L->cruns?L->cruns*2:8; L->runs=realloc(L->runs,sizeof(DwRun)*L->cruns); }
    DwRun*x=&L->runs[L->nruns++]; memset(x,0,sizeof *x); x->start=r.start; x->len=r.len; x->kind=kind; dw_free_lines(L); return x; }
static int MSABI dwl_SetMaxWidth(void*s,float w){ DwLayout*L=s; if(w<0) return (int)HR_INVALIDARG; L->maxw=w; dw_free_lines(L); return 0; }
static int MSABI dwl_SetMaxHeight(void*s,float h){ DwLayout*L=s; if(h<0) return (int)HR_INVALIDARG; L->maxh=h; dw_free_lines(L); return 0; }
static int MSABI dwl_SetFontCollection(void*s,void*c,DwRng r){ DwRun*x=dw_addrun(s,DR_COLL,r); if(x){ x->p=c; if(c) d2_addref(c); } return 0; }
static int MSABI dwl_SetFontFamilyName(void*s,const uint16_t*n,DwRng r){ DwRun*x=dw_addrun(s,DR_FAM,r); if(x){ int l=dw_wlen(n); x->w=calloc(l+1,2); memcpy(x->w,n,(size_t)l*2); } return 0; }
static int MSABI dwl_SetFontWeight(void*s,int v,DwRng r){ DwRun*x=dw_addrun(s,DR_WEIGHT,r); if(x) x->i=v; return 0; }
static int MSABI dwl_SetFontStyle(void*s,int v,DwRng r){ DwRun*x=dw_addrun(s,DR_STYLE,r); if(x) x->i=v; return 0; }
static int MSABI dwl_SetFontStretch(void*s,int v,DwRng r){ DwRun*x=dw_addrun(s,DR_STRETCH,r); if(x) x->i=v; return 0; }
static int MSABI dwl_SetFontSize(void*s,float v,DwRng r){ DwRun*x=dw_addrun(s,DR_SIZE,r); if(x) x->f=v; return 0; }
static int MSABI dwl_SetUnderline(void*s,int v,DwRng r){ DwRun*x=dw_addrun(s,DR_UNDER,r); if(x) x->i=v; return 0; }
static int MSABI dwl_SetStrikethrough(void*s,int v,DwRng r){ DwRun*x=dw_addrun(s,DR_STRIKE,r); if(x) x->i=v; return 0; }
static int MSABI dwl_SetDrawingEffect(void*s,void*e,DwRng r){ DwRun*x=dw_addrun(s,DR_EFFECT,r); if(x){ x->p=e; if(e) ((uint32_t(MSABI*)(void*))(*(void***)e)[1])(e); } return 0; }
static int MSABI dwl_SetInlineObject(void*s,void*o,DwRng r){ (void)s;(void)o;(void)r; return 0; }   // (inline objects are not drawn)
static int MSABI dwl_SetTypography(void*s,void*t,DwRng r){ (void)s;(void)t;(void)r; return 0; }
static int MSABI dwl_SetLocaleName(void*s,const uint16_t*n,DwRng r){ (void)s;(void)n;(void)r; return 0; }
static float MSABI dwl_GetMaxWidth(void*s){ return ((DwLayout*)s)->maxw; }
static float MSABI dwl_GetMaxHeight(void*s){ return ((DwLayout*)s)->maxh; }
// the value of property `kind` at text position pos (the format's when no range sets it) + the range it holds over
typedef struct { DwColl*coll; const uint16_t*fam; int weight, style, stretch, under, strike, nokern; float size, lead, trail, minadv; void*effect; } DwProps;
static void dw_props_at(DwLayout*L,uint32_t pos,DwProps*p,uint32_t*rs,uint32_t*re){
    p->coll=L->coll; p->fam=L->fam; p->weight=L->weight; p->style=L->style; p->stretch=L->stretch; p->size=L->size; p->under=p->strike=0; p->effect=0;
    p->nokern=0; p->lead=p->trail=p->minadv=0;
    uint32_t a=0, b=L->len>0?L->len:1; if(b<=pos) b=pos+1;
    for(int i=0;i<L->nruns;i++){ DwRun*x=&L->runs[i]; uint32_t xs=x->start, xe=x->start+x->len; if(xe<xs) xe=0xFFFFFFFFu;
        if(pos>=xs && pos<xe){ if(xs>a) a=xs; if(xe<b) b=xe;
            switch(x->kind){ case DR_COLL: p->coll=x->p; break; case DR_FAM: p->fam=x->w; break; case DR_WEIGHT: p->weight=x->i; break; case DR_STYLE: p->style=x->i; break;
                case DR_STRETCH: p->stretch=x->i; break; case DR_SIZE: p->size=x->f; break; case DR_UNDER: p->under=x->i; break; case DR_STRIKE: p->strike=x->i; break;
                case DR_EFFECT: p->effect=x->p; break; case DR_KERN: p->nokern=!x->i; break;
                case DR_SPACING: p->lead=x->f; p->trail=x->f2; p->minadv=x->f3; break; } }
        else if(xs>pos && xs<b) b=xs; else if(xe<=pos && xe>a) a=xe; }
    if(rs)*rs=a; if(re)*re=b; }
static void dw_rng_out(DwRng*out,uint32_t a,uint32_t b){ if(out){ out->start=a; out->len=b-a; } }
static int MSABI dwl_GetFontCollectionAt(void*s,uint32_t pos,void**pp,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); *pp=p.coll; if(p.coll) d2_addref(p.coll); return 0; }
static int MSABI dwl_GetFontFamilyNameLengthAt(void*s,uint32_t pos,uint32_t*n,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(n)*n=(uint32_t)dw_wlen(p.fam); return 0; }
static int MSABI dwl_GetFontFamilyNameAt(void*s,uint32_t pos,uint16_t*buf,uint32_t n,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b);
    if(!buf||n<=(uint32_t)dw_wlen(p.fam)) return (int)0x8007007A; dw_wcpy(buf,(int)n,p.fam); return 0; }
static int MSABI dwl_GetFontWeightAt(void*s,uint32_t pos,int*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.weight; return 0; }
static int MSABI dwl_GetFontStyleAt(void*s,uint32_t pos,int*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.style; return 0; }
static int MSABI dwl_GetFontStretchAt(void*s,uint32_t pos,int*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.stretch; return 0; }
static int MSABI dwl_GetFontSizeAt(void*s,uint32_t pos,float*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.size; return 0; }
static int MSABI dwl_GetUnderlineAt(void*s,uint32_t pos,int*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.under; return 0; }
static int MSABI dwl_GetStrikethroughAt(void*s,uint32_t pos,int*v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(v)*v=p.strike; return 0; }
static int MSABI dwl_GetDrawingEffectAt(void*s,uint32_t pos,void**v,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b);
    *v=p.effect; if(p.effect) ((uint32_t(MSABI*)(void*))(*(void***)p.effect)[1])(p.effect); return 0; }
// IDWriteTextLayout1 67 SetPairKerning, 68 GetPairKerning, 69 SetCharacterSpacing, 70 GetCharacterSpacing
static int MSABI dwl_SetPairKerning(void*s,int on,DwRng r){ DwRun*x=dw_addrun(s,DR_KERN,r); if(x) x->i=on; return 0; }
static int MSABI dwl_GetPairKerning(void*s,uint32_t pos,int*on,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b); if(on)*on=!p.nokern; return 0; }
static int MSABI dwl_SetCharacterSpacing(void*s,float lead,float trail,float minadv,DwRng r){ if(minadv<0) return (int)HR_INVALIDARG;
    DwRun*x=dw_addrun(s,DR_SPACING,r); if(x){ x->f=lead; x->f2=trail; x->f3=minadv; } return 0; }
static int MSABI dwl_GetCharacterSpacing(void*s,uint32_t pos,float*lead,float*trail,float*minadv,DwRng*r){ DwProps p; uint32_t a,b; dw_props_at(s,pos,&p,&a,&b); dw_rng_out(r,a,b);
    if(lead)*lead=p.lead; if(trail)*trail=p.trail; if(minadv)*minadv=p.minadv; return 0; }
static int MSABI dwl_GetNullAt(void*s,uint32_t pos,void**v,DwRng*r){ (void)pos; DwLayout*L=s; if(v)*v=0; dw_rng_out(r,0,L->len); return 0; }
static int MSABI dwl_GetLocaleNameLengthAt(void*s,uint32_t pos,uint32_t*n,DwRng*r){ (void)pos; DwLayout*L=s; if(n)*n=(uint32_t)dw_wlen(L->locale); dw_rng_out(r,0,L->len); return 0; }
static int MSABI dwl_GetLocaleNameAt(void*s,uint32_t pos,uint16_t*buf,uint32_t n,DwRng*r){ (void)pos; DwLayout*L=s; dw_rng_out(r,0,L->len);
    if(!buf||n<=(uint32_t)dw_wlen(L->locale)) return (int)0x8007007A; dw_wcpy(buf,(int)n,L->locale); return 0; }
// ---- line breaking ---------------------------------------------------------------------------------------------------------
static CTLineRef dw_ellipsis(CTFontRef font){ UniChar e=0x2026; CFStringRef s=CFStringCreateWithCharacters(0,&e,1);
    const void*k[2]={kCTFontAttributeName,kCTForegroundColorFromContextAttributeName}; const void*v[2]={font,kCFBooleanTrue};
    CFDictionaryRef at=CFDictionaryCreate(0,k,v,2,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as=CFAttributedStringCreate(0,s,at); CTLineRef l=CTLineCreateWithAttributedString(as); CFRelease(as); CFRelease(at); CFRelease(s); return l; }
static int dw_isnl(uint16_t c){ return c=='\n'||c=='\r'||c==0x2028||c==0x2029||c==0x85||c==0x0b||c==0x0c; }
static CFAttributedStringRef dw_attr(DwLayout*L,CTFontRef*first){
    CFStringRef str=CFStringCreateWithCharacters(0,L->text?L->text:(const UniChar*)u"",(CFIndex)L->len);
    CFMutableAttributedStringRef as=CFAttributedStringCreateMutable(0,0); CFAttributedStringReplaceString(as,CFRangeMake(0,0),str); CFRelease(str);
    CFAttributedStringBeginEditing(as); *first=0;
    for(uint32_t pos=0;pos<L->len||(pos==0&&!*first);){ DwProps p; uint32_t a,b; dw_props_at(L,pos,&p,&a,&b); if(b>L->len) b=L->len; if(b<=pos) b=pos+1;
        CTFontRef f=dw_font(p.coll,p.fam,p.weight,p.style,p.size); if(!*first) *first=f;
        if(L->len){ CFRange r=CFRangeMake(pos,(CFIndex)(b>L->len?L->len:b)-pos); CFAttributedStringSetAttribute(as,r,kCTFontAttributeName,f);
            float c[4]; if(p.effect && d2_brush_solid_color(p.effect,c)){ CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB); CGFloat cc[4]={c[0],c[1],c[2],c[3]};
                CGColorRef col=CGColorCreate(cs,cc); CFAttributedStringSetAttribute(as,r,kCTForegroundColorAttributeName,col); CGColorRelease(col); CGColorSpaceRelease(cs); }
            else CFAttributedStringSetAttribute(as,r,kCTForegroundColorFromContextAttributeName,kCFBooleanTrue);
            // IDWriteTextLayout1: pair kerning off = no kerning; character spacing = extra advance after each character (leading +
            // trailing; the minimum advance width is not applied)
            if(p.nokern || p.lead!=0 || p.trail!=0){ double kv=p.lead+p.trail; CFNumberRef kn=CFNumberCreate(0,kCFNumberDoubleType,&kv);
                CFAttributedStringSetAttribute(as,r,kCTKernAttributeName,kn); CFRelease(kn); } }
        pos=b; }
    CFAttributedStringEndEditing(as); return as; }
static void dw_line_metrics(DwLine*ln,CTFontRef f){ CGFloat a=0,d=0,l=0; ln->w=ln->line?CTLineGetTypographicBounds(ln->line,&a,&d,&l):0;
    if(!ln->line || ln->len-ln->nl<=0){ a=CTFontGetAscent(f); d=CTFontGetDescent(f); l=CTFontGetLeading(f); ln->w=0; }
    ln->asc=a; ln->desc=d; ln->lead=l; ln->wtrail=ln->line?CTLineGetTrailingWhitespaceWidth(ln->line):0; if(ln->wtrail>ln->w) ln->wtrail=ln->w; }
static void dw_build(DwLayout*L){
    if(L->built) return; dw_free_lines(L);
    CTFontRef first=0; CFAttributedStringRef as=dw_attr(L,&first);
    CTTypesetterRef ts=CTTypesetterCreateWithAttributedString(as);
    int cap=8; L->lines=calloc(cap,sizeof(DwLine)); double wmax=(L->wrap==1||L->maxw<=0)?1e7:L->maxw;
    CFIndex start=0, n=(CFIndex)L->len;
    while(start<n){ CFIndex cnt=(L->wrap==4)?CTTypesetterSuggestClusterBreak(ts,start,wmax):CTTypesetterSuggestLineBreak(ts,start,wmax); if(cnt<=0) cnt=1; if(start+cnt>n) cnt=n-start;
        if(L->nlines==cap){ cap*=2; L->lines=realloc(L->lines,sizeof(DwLine)*cap); }
        DwLine*ln=&L->lines[L->nlines++]; memset(ln,0,sizeof *ln); ln->start=start; ln->len=cnt;
        CFIndex e=start+cnt; if(e>start && dw_isnl(L->text[e-1])){ e--; ln->nl=1; if(L->text[e]=='\n' && e>start && L->text[e-1]=='\r'){ e--; ln->nl=2; } }   // (CR LF = one break)
        ln->line=CTTypesetterCreateLine(ts,CFRangeMake(start,e-start>0?e-start:0)); dw_line_metrics(ln,first); start+=cnt; }
    if(L->nlines==0 || (n>0 && dw_isnl(L->text[n-1]))){   // an empty text / a text ending in a line break: one more (empty) line
        if(L->nlines==cap){ cap*=2; L->lines=realloc(L->lines,sizeof(DwLine)*cap); }
        DwLine*ln=&L->lines[L->nlines++]; memset(ln,0,sizeof *ln); ln->start=n; CTFontRef lf=first; if(n>0){ DwProps p; dw_props_at(L,(uint32_t)n-1,&p,0,0); lf=dw_font(p.coll,p.fam,p.weight,p.style,p.size); }
        dw_line_metrics(ln,lf); }
    // trimming: lines past the box are dropped and the last one shown ends in the sign; a no-wrap line wider than the box is cut
    int trim=L->trim[0]!=0; CTLineRef sign=(trim && L->trimsign && ((DwObj*)L->trimsign)->ellipsis)?dw_ellipsis(first):0;
    double y=0;
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i];
        double nat=ln->asc+ln->desc+ln->lead; ln->h=nat; double base=ln->asc+ln->lead*0;
        if(L->lsm==1 && L->lsp>0){ ln->h=L->lsp; base=L->lsb; } else if(L->lsm==2 && L->lsp>0){ ln->h=nat*L->lsp; base=ln->asc*(L->lsb>0?L->lsb:1); }
        if(trim && L->maxh>0 && i>0 && y+ln->h>L->maxh+0.5){   // does not fit: the previous line becomes the last
            DwLine*pl=&L->lines[i-1]; CTLineRef full=CTTypesetterCreateLine(ts,CFRangeMake(pl->start,n-pl->start));
            CTLineRef tl=CTLineCreateTruncatedLine(full,L->maxw>0?L->maxw:1e7,kCTLineTruncationEnd,sign); CFRelease(full);
            if(tl){ CFRelease(pl->line); pl->line=tl; pl->trimmed=1; dw_line_metrics(pl,first); pl->len=n-pl->start; }
            for(int j=i;j<L->nlines;j++) if(L->lines[j].line) CFRelease(L->lines[j].line); L->nlines=i; break; }
        if(trim && L->maxw>0 && ln->line && ln->w-ln->wtrail>L->maxw+0.5){ CTLineRef tl=CTLineCreateTruncatedLine(ln->line,L->maxw,kCTLineTruncationEnd,sign);
            if(tl){ CFRelease(ln->line); ln->line=tl; ln->trimmed=1; dw_line_metrics(ln,first); } }
        ln->top=y; ln->base=y+base; y+=ln->h; }
    if(sign) CFRelease(sign);
    double H=y, top=L->palign==1?L->maxh-H:L->palign==2?(L->maxh-H)/2:0;
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; double w=ln->w-ln->wtrail;
        ln->x=L->talign==1?L->maxw-w:L->talign==2?(L->maxw-w)/2:0; if(L->rdir==1) ln->x=L->maxw-w-ln->x; ln->top+=top; ln->base+=top; }
    CFRelease(ts); CFRelease(as); L->built=1; }
// ---- drawing: every line at origin (ox, oy) of a y-down CGContext; fill mode uses the context's fill colour (ranges with a
// solid-brush effect their own), clip mode adds the glyph outlines to the clip (d2d.c then paints a gradient / bitmap brush)
static void dw_draw(CGContextRef cg,DwLayout*L,double ox,double oy,int clipmode){
    dw_build(L); CGContextSetTextMatrix(cg,CGAffineTransformMakeScale(1,-1)); CGContextSetTextDrawingMode(cg,clipmode?kCGTextClip:kCGTextFill);
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; if(!ln->line) continue;
        CGContextSetTextPosition(cg,ox+ln->x,oy+ln->base); CTLineDraw(ln->line,cg); }
    if(clipmode){ CGContextSetTextDrawingMode(cg,kCGTextFill); return; }
    // underline / strikethrough ranges: CoreText's metrics of each run's font
    for(int r=0;r<L->nruns;r++){ DwRun*x=&L->runs[r]; if((x->kind!=DR_UNDER&&x->kind!=DR_STRIKE)||!x->i) continue;
        for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; if(!ln->line) continue; CFIndex a=(CFIndex)x->start>ln->start?(CFIndex)x->start:ln->start, b=(CFIndex)(x->start+x->len);
            CFIndex le=ln->start+ln->len-ln->nl; if(b>le) b=le; if(b<=a) continue;
            DwProps p; dw_props_at(L,(uint32_t)a,&p,0,0); CTFontRef f=dw_font(p.coll,p.fam,p.weight,p.style,p.size);
            double x0=CTLineGetOffsetForStringIndex(ln->line,a,0), x1=CTLineGetOffsetForStringIndex(ln->line,b,0), th=CTFontGetUnderlineThickness(f); if(th<1) th=1;
            double yy=x->kind==DR_UNDER?ln->base-CTFontGetUnderlinePosition(f):ln->base-CTFontGetXHeight(f)*0.5;
            CGContextFillRect(cg,CGRectMake(ox+ln->x+x0,oy+yy-th/2,x1-x0,th)); } } }
// ---- metrics -------------------------------------------------------------------------------------------------------------------
typedef struct { float left, top, width, wtrail, height, lw, lh; uint32_t depth, lines; } DwTextMetrics;
static int MSABI dwl_GetMetrics(void*s,DwTextMetrics*m){ DwLayout*L=s; dw_build(L); if(!m) return (int)HR_INVALIDARG; memset(m,0,sizeof *m);
    double l=1e30,w=0,wt=0,top=L->nlines?L->lines[0].top:0,H=0;
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; if(ln->x<l) l=ln->x; if(ln->w-ln->wtrail>w) w=ln->w-ln->wtrail; if(ln->w>wt) wt=ln->w; H+=ln->h; }
    m->left=(float)(L->nlines?l:0); m->top=(float)top; m->width=(float)w; m->wtrail=(float)wt; m->height=(float)H; m->lw=L->maxw; m->lh=L->maxh; m->depth=1; m->lines=(uint32_t)L->nlines; return 0; }
typedef struct { uint32_t length, trail, nl; float height, baseline; int trimmed; } DwLineMetrics;
static int MSABI dwl_GetLineMetrics(void*s,DwLineMetrics*m,uint32_t max,uint32_t*got){ DwLayout*L=s; dw_build(L); if(got)*got=(uint32_t)L->nlines;
    if(!m || max<(uint32_t)L->nlines) return (int)0x8007007A;   // E_NOT_SUFFICIENT_BUFFER (the count is out)
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; uint32_t trail=0; for(CFIndex k=ln->start+ln->len-ln->nl-1;k>=ln->start&&(L->text[k]==' '||L->text[k]=='\t');k--) trail++;
        m[i].length=(uint32_t)ln->len; m[i].trail=trail+(uint32_t)ln->nl; m[i].nl=(uint32_t)ln->nl; m[i].height=(float)ln->h; m[i].baseline=(float)(ln->base-ln->top); m[i].trimmed=ln->trimmed; }
    return 0; }
typedef struct { float left, top, right, bottom; } DwOverhang;
static int MSABI dwl_GetOverhangMetrics(void*s,DwOverhang*o){ (void)s; if(o) memset(o,0,sizeof *o); return 0; }
typedef struct { float width; uint16_t length, flags; } DwCluster;
static int dw_line_of(DwLayout*L,CFIndex pos){ for(int i=0;i<L->nlines;i++) if(pos<L->lines[i].start+L->lines[i].len) return i; return L->nlines-1; }
static double dw_xof(DwLayout*L,int li,CFIndex pos){ DwLine*ln=&L->lines[li]; CFIndex e=ln->start+ln->len-ln->nl; if(pos>e) pos=e; return ln->x+(ln->line?CTLineGetOffsetForStringIndex(ln->line,pos,0):0); }
static int MSABI dwl_GetClusterMetrics(void*s,DwCluster*m,uint32_t max,uint32_t*got){ DwLayout*L=s; dw_build(L); uint32_t n=0;
    for(uint32_t i=0;i<L->len;){ uint32_t k=(L->text[i]>=0xD800&&L->text[i]<0xDC00&&i+1<L->len)?2:1; if(m && n<max){ int li=dw_line_of(L,i);
            double w=dw_xof(L,li,i+k)-dw_xof(L,li,i); uint16_t c=L->text[i]; m[n].width=(float)(w<0?0:w); m[n].length=(uint16_t)k;
            m[n].flags=(uint16_t)(((c==' '||c=='\t'||dw_isnl(c)||c=='-')?1:0)|((c==' '||c=='\t')?2:0)|(dw_isnl(c)?4:0)|(c==0xAD?8:0)); }
        n++; i+=k; }
    if(got)*got=n; return n>max?(int)0x8007007A:0; }
static int MSABI dwl_DetermineMinWidth(void*s,float*mw){ DwLayout*L=s; double best=0; CTFontRef first=0; CFAttributedStringRef as=dw_attr(L,&first);
    for(uint32_t i=0;i<L->len;){ while(i<L->len && (L->text[i]==' '||dw_isnl(L->text[i]))) i++; uint32_t j=i; while(j<L->len && L->text[j]!=' ' && !dw_isnl(L->text[j])) j++;
        if(j>i){ CFAttributedStringRef w=CFAttributedStringCreateWithSubstring(0,as,CFRangeMake(i,j-i)); CTLineRef l=CTLineCreateWithAttributedString(w);
            double v=CTLineGetTypographicBounds(l,0,0,0); if(v>best) best=v; CFRelease(l); CFRelease(w); } i=j; }
    CFRelease(as); if(mw)*mw=(float)best; return 0; }
typedef struct { uint32_t pos, len; float left, top, width, height; uint32_t bidi; int isText, isTrimmed; } DwHit;
static void dw_hit_fill(DwLayout*L,int li,CFIndex pos,DwHit*m){ DwLine*ln=&L->lines[li]; CFIndex k=(pos<(CFIndex)L->len && L->text[pos]>=0xD800&&L->text[pos]<0xDC00)?2:1;
    double x0=dw_xof(L,li,pos), x1=dw_xof(L,li,pos+k); if(m){ m->pos=(uint32_t)pos; m->len=(uint32_t)k; m->left=(float)x0; m->top=(float)ln->top; m->width=(float)(x1-x0); m->height=(float)ln->h;
        m->bidi=0; m->isText=pos<(CFIndex)L->len; m->isTrimmed=ln->trimmed; } }
static int MSABI dwl_HitTestPoint(void*s,float x,float y,int*trailing,int*inside,DwHit*m){ DwLayout*L=s; dw_build(L);
    int li=0; for(int i=0;i<L->nlines;i++){ li=i; if(y<L->lines[i].top+L->lines[i].h) break; }
    DwLine*ln=&L->lines[li]; CFIndex e=ln->start+ln->len-ln->nl, pos=ln->line?CTLineGetStringIndexForPosition(ln->line,CGPointMake(x-ln->x,0)):ln->start;
    if(pos<ln->start) pos=ln->start; int tr=0; if(pos>=e && e>ln->start){ pos=e-1; tr=1; } else if(pos>ln->start){ double mid=(dw_xof(L,li,pos-1)+dw_xof(L,li,pos))/2; if(x<mid){ pos--; tr=1; } }
    if(pos>=(CFIndex)L->len && L->len) { pos=L->len-1; tr=1; }
    dw_hit_fill(L,li,pos,m); if(trailing)*trailing=tr; if(inside){ double t=L->nlines?L->lines[0].top:0, b=ln->top+ln->h; *inside=y>=t&&y<b&&x>=ln->x&&x<ln->x+ln->w; } return 0; }
static int MSABI dwl_HitTestTextPosition(void*s,uint32_t pos,int trailing,float*px,float*py,DwHit*m){ DwLayout*L=s; dw_build(L); if(pos>L->len) pos=L->len;
    int li=dw_line_of(L,pos); DwHit h; dw_hit_fill(L,li,pos,&h); if(m)*m=h; if(px)*px=trailing?h.left+h.width:h.left; if(py)*py=h.top; return 0; }
static int MSABI dwl_HitTestTextRange(void*s,uint32_t pos,uint32_t len,float ox,float oy,DwHit*m,uint32_t max,uint32_t*got){ DwLayout*L=s; dw_build(L); uint32_t n=0;
    for(int i=0;i<L->nlines;i++){ DwLine*ln=&L->lines[i]; CFIndex a=(CFIndex)pos>ln->start?(CFIndex)pos:ln->start, b=(CFIndex)(pos+len), le=ln->start+ln->len; if(b>le) b=le; if(b<=a) continue;
        if(m && n<max){ double x0=dw_xof(L,i,a), x1=dw_xof(L,i,b); m[n].pos=(uint32_t)a; m[n].len=(uint32_t)(b-a); m[n].left=(float)(x0+ox); m[n].top=(float)(ln->top+oy);
            m[n].width=(float)(x1-x0); m[n].height=(float)ln->h; m[n].bidi=0; m[n].isText=1; m[n].isTrimmed=ln->trimmed; } n++; }
    if(got)*got=n; return n>max?(int)0x8007007A:0; }
// ---- factory: CreateTextFormat / CreateTextLayout --------------------------------------------------------------------------
static DwColl* g_dw_system;
static DwColl* dw_system(void){ if(!g_dw_system){ DwColl*c=d2_alloc(&C_dwcoll,sizeof(DwColl)); c->system=1; c->h.refs=1<<30; g_dw_system=c; } return g_dw_system; }
static int MSABI dwfac_CreateTextFormat(void*s,const uint16_t*fam,void*coll,int weight,int style,int stretch,float size,const uint16_t*loc,void**pp){ (void)s;
    if(!pp||!fam||size<=0){ if(pp)*pp=0; return (int)HR_INVALIDARG; }
    DwFmt*f=d2_alloc(&C_dwfmt,sizeof(DwFmt)); dw_wcpy(f->fam,100,fam); f->coll=coll?coll:dw_system(); d2_addref(f->coll);
    f->weight=weight; f->style=style; f->stretch=stretch; f->size=size; dw_wcpy(f->locale,86,loc); f->tab=size*4;
    if(getenv("PELOAD_TRACE_D2D")){ char a[128],m[260]; int k=snprintf(m,sizeof m,"  [dwrite] text format '%s' %s weight %d style %d size %.1f\n",dw_u8(fam,-1,a,sizeof a),coll?"(own collection)":"(system)",weight,style,size); (void)!write(2,m,k); }
    *pp=f; return 0; }
static int MSABI dwfac_CreateTextLayout(void*s,const uint16_t*str,uint32_t len,void*fmt,float maxw,float maxh,void**pp){ (void)s;
    if(!pp||!dw_is_fmt(fmt)||(len&&!str)){ if(pp)*pp=0; return (int)HR_INVALIDARG; }
    DwFmt*src=fmt; DwLayout*L=d2_alloc(&C_dwlayout,sizeof(DwLayout)); void*keep=L->h.vtbl; D2Cls*kc=L->h.cls; int kr=L->h.refs;
    memcpy(L,src,offsetof(DwFmt,text)); L->h.vtbl=keep; L->h.cls=kc; L->h.refs=kr;   // the format's values
    if(L->coll) d2_addref(L->coll); if(L->trimsign) d2_addref(L->trimsign);
    L->text=calloc(len+1,2); if(len) memcpy(L->text,str,(size_t)len*2); L->len=len; L->maxw=maxw<0?0:maxw; L->maxh=maxh<0?0:maxh; *pp=L; return 0; }
static int MSABI dwfac_CreateGdiCompatibleTextLayout(void*s,const uint16_t*str,uint32_t len,void*fmt,float maxw,float maxh,float ppd,const void*tf,int nat,void**pp){ (void)ppd;(void)tf;(void)nat;
    return dwfac_CreateTextLayout(s,str,len,fmt,maxw,maxh,pp); }
static int MSABI dwfac_GetSystemFontCollection(void*s,void**pp,int check){ (void)s;(void)check; *pp=dw_system(); return 0; }
static int MSABI dwfac_GetSystemFontCollection3(void*s,int dl,void**pp,int check){ (void)s;(void)dl;(void)check; *pp=dw_system(); return 0; }
static int MSABI dwfac_ok(void*s,void*a){ (void)s;(void)a; return 0; }
static int MSABI dwfac_CreateRenderingParams(void*s,void**pp){ (void)s; *pp=d2_alloc(&C_dwparams,sizeof(DwObj)); return 0; }
static int MSABI dwfac_CreateMonitorRenderingParams(void*s,void*mon,void**pp){ (void)mon; return dwfac_CreateRenderingParams(s,pp); }
static int MSABI dwfac_CreateCustomRenderingParams(void*s,float g,float c,float l,int geo,int mode,void**pp){ (void)g;(void)c;(void)l;(void)geo;(void)mode; return dwfac_CreateRenderingParams(s,pp); }
static int MSABI dwfac_CreateTypography(void*s,void**pp){ (void)s; *pp=d2_alloc(&C_dwtypo,sizeof(DwObj)); return 0; }
static int MSABI dwfac_CreateEllipsisTrimmingSign(void*s,void*fmt,void**pp){ (void)s;(void)fmt; DwObj*o=d2_alloc(&C_dwinline,sizeof(DwObj)); o->ellipsis=1; *pp=o; return 0; }
static int MSABI dwfac_CreateFontSetBuilder(void*s,void**pp){ (void)s; *pp=d2_alloc(&C_dwbuilder,sizeof(DwSet)); return 0; }
static int MSABI dwfac_CreateInMemoryFontFileLoader(void*s,void**pp){ (void)s; *pp=d2_alloc(&C_dwmemloader,sizeof(DwObj)); return 0; }
static int MSABI dwfac_CreateFontCollectionFromFontSet(void*s,void*set,void**pp){ (void)s; D2Hdr*h=set;
    if(!h||(h->cls!=&C_dwset&&h->cls!=&C_dwbuilder)){ *pp=0; return (int)HR_INVALIDARG; } DwSet*fs=set; DwColl*c=d2_alloc(&C_dwcoll,sizeof(DwColl));
    c->faces=calloc(fs->n+1,sizeof(DwFace*)); memcpy(c->faces,fs->faces,sizeof(DwFace*)*fs->n); c->n=fs->n; *pp=c; return 0; }
// a font file on disk (CreateFontFileReference) -- the same as one in memory
static int MSABI dwfac_CreateFontFileReference(void*s,const uint16_t*path,const void*ft,void**pp){ (void)s;(void)ft; char p[2048]; win2unix(path,p,sizeof p);
    FILE*f=fopen(p,"rb"); if(!f){ *pp=0; return (int)0x80070002; } fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); uint8_t*b=malloc(n>0?(size_t)n:1);
    size_t got=n>0?fread(b,1,(size_t)n,f):0; fclose(f); DwFile*fl=d2_alloc(&C_dwfile,sizeof(DwFile)); fl->face=dw_face_from_data(b,(uint32_t)got); free(b); *pp=fl; return 0; }
// IDWriteInMemoryFontFileLoader 4 CreateInMemoryFontFileReference(factory, data, size, owner, file**), 5 GetFileCount
static int MSABI dwml_CreateRef(void*s,void*fac,const uint8_t*data,uint32_t size,void*owner,void**pp){ (void)fac;(void)owner; DwObj*o=s; o->nfiles++;
    DwFile*fl=d2_alloc(&C_dwfile,sizeof(DwFile)); fl->face=dw_face_from_data(data,size); *pp=fl; return 0; }
static uint32_t MSABI dwml_GetFileCount(void*s){ return (uint32_t)((DwObj*)s)->nfiles; }
// IDWriteFontFile 5 Analyze(supported*, fileType*, faceType*, faces*)
static int MSABI dwfile_Analyze(void*s,int*ok,int*ft,int*fc,uint32_t*n){ DwFile*f=s; if(ok)*ok=f->face!=0; if(ft)*ft=f->face?2:0; if(fc)*fc=f->face?1:0; if(n)*n=f->face?1:0; return 0; }
static int MSABI dwfile_GetLoader(void*s,void**pp){ (void)s; *pp=0; return (int)HR_E_NOTIMPL; }
// IDWriteFontSetBuilder(1) 6 CreateFontSet, 7 AddFontFile; IDWriteFontSet 3 GetFontCount
static void dw_set_add(DwSet*b,DwFace*f){ if(!f) return; for(int i=0;i<b->n;i++) if(b->faces[i]==f) return; if(b->n==b->cap){ b->cap=b->cap?b->cap*2:8; b->faces=realloc(b->faces,sizeof(DwFace*)*b->cap); } b->faces[b->n++]=f; }
static void dw_set_dtor(void*p){ free(((DwSet*)p)->faces); }
static int MSABI dwb_AddFontFile(void*s,void*file){ D2Hdr*h=file; if(!h||h->cls!=&C_dwfile) return (int)HR_INVALIDARG; DwFile*f=file;
    if(!f->face) return (int)0x88985004;   // DWRITE_E_FILEFORMAT
    dw_set_add(s,f->face); return 0; }
static int MSABI dwb_AddFontSet(void*s,void*set){ D2Hdr*h=set; if(!h||(h->cls!=&C_dwset&&h->cls!=&C_dwbuilder)) return (int)HR_INVALIDARG; DwSet*o=set; for(int i=0;i<o->n;i++) dw_set_add(s,o->faces[i]); return 0; }
static int MSABI dwb_CreateFontSet(void*s,void**pp){ DwSet*b=s, *o=d2_alloc(&C_dwset,sizeof(DwSet)); for(int i=0;i<b->n;i++) dw_set_add(o,b->faces[i]); *pp=o; return 0; }
static uint32_t MSABI dwset_GetFontCount(void*s){ return (uint32_t)((DwSet*)s)->n; }
// IDWriteFontCollection 3 GetFontFamilyCount, 5 FindFamilyName; (1) 7 GetFontSet
static int dw_coll_families(DwColl*c,const char**out,int cap){ int n=0; for(int i=0;i<c->n;i++){ int dup=0; for(int j=0;j<n;j++) if(!strcasecmp(out[j],c->faces[i]->fam)) dup=1; if(!dup&&n<cap) out[n++]=c->faces[i]->fam; } return n; }
static uint32_t MSABI dwcoll_GetFontFamilyCount(void*s){ DwColl*c=s; if(c->system) return 1; const char*f[64]; return (uint32_t)dw_coll_families(c,f,64); }
static int MSABI dwcoll_FindFamilyName(void*s,const uint16_t*name,uint32_t*idx,int*exists){ DwColl*c=s; char nm[128]; dw_u8(name,-1,nm,sizeof nm);
    if(c->system){ if(idx)*idx=0; if(exists)*exists=1; return 0; }   // (any family: dw_font maps missing ones)
    const char*f[64]; int n=dw_coll_families(c,f,64); for(int i=0;i<n;i++) if(!strcasecmp(f[i],nm)){ if(idx)*idx=(uint32_t)i; if(exists)*exists=1; return 0; }
    for(int i=0;i<c->n;i++) if(dw_face_named(c->faces[i],nm)){ if(idx)*idx=0; if(exists)*exists=1; return 0; }
    if(idx)*idx=0xFFFFFFFFu; if(exists)*exists=0; return 0; }
static int MSABI dwcoll_GetFontSet(void*s,void**pp){ DwColl*c=s; DwSet*o=d2_alloc(&C_dwset,sizeof(DwSet)); for(int i=0;i<c->n;i++) dw_set_add(o,c->faces[i]); *pp=o; return 0; }
static void dw_coll_dtor(void*p){ free(((DwColl*)p)->faces); }
// IDWriteRenderingParams 3 GetGamma, 4 GetEnhancedContrast, 5 GetClearTypeLevel, 6 GetPixelGeometry, 7 GetRenderingMode
static float MSABI dwp_gamma(void*s){ (void)s; return 1.8f; }
static float MSABI dwp_contrast(void*s){ (void)s; return 0.5f; }
static float MSABI dwp_ctlevel(void*s){ (void)s; return 0.0f; }
static int MSABI dwp_zero(void*s){ (void)s; return 0; }
// IDWriteTypography 3 AddFontFeature, 4 GetFontFeatureCount; IDWriteInlineObject 4 GetMetrics, 5 GetOverhangMetrics, 6 GetBreakConditions
static int MSABI dwt_AddFontFeature(void*s,uint64_t f){ (void)s;(void)f; return 0; }
static int MSABI dwi_GetMetrics(void*s,float*m){ (void)s; if(m) memset(m,0,16); return 0; }
static int MSABI dwi_GetOverhang(void*s,float*m){ (void)s; if(m) memset(m,0,16); return 0; }
static int MSABI dwi_GetBreakConditions(void*s,int*a,int*b){ (void)s; if(a)*a=0; if(b)*b=0; return 0; }
static void dw_setup_fmt(void**v){ v[3]=dwf_SetTextAlignment; v[4]=dwf_SetParagraphAlignment; v[5]=dwf_SetWordWrapping; v[6]=dwf_SetReadingDirection; v[7]=dwf_SetFlowDirection;
    v[8]=dwf_SetIncrementalTabStop; v[9]=dwf_SetTrimming; v[10]=dwf_SetLineSpacing; v[11]=dwf_GetTextAlignment; v[12]=dwf_GetParagraphAlignment; v[13]=dwf_GetWordWrapping;
    v[14]=dwf_GetReadingDirection; v[15]=dwf_GetFlowDirection; v[16]=dwf_GetIncrementalTabStop; v[17]=dwf_GetTrimming; v[18]=dwf_GetLineSpacing; v[19]=dwf_GetFontCollection;
    v[20]=dwf_GetFontFamilyNameLength; v[21]=dwf_GetFontFamilyName; v[22]=dwf_GetFontWeight; v[23]=dwf_GetFontStyle; v[24]=dwf_GetFontStretch; v[25]=dwf_GetFontSize;
    v[26]=dwf_GetLocaleNameLength; v[27]=dwf_GetLocaleName; }
static void dw_setup_layout(void**v){ dw_setup_fmt(v); v[28]=dwl_SetMaxWidth; v[29]=dwl_SetMaxHeight; v[30]=dwl_SetFontCollection; v[31]=dwl_SetFontFamilyName;
    v[32]=dwl_SetFontWeight; v[33]=dwl_SetFontStyle; v[34]=dwl_SetFontStretch; v[35]=dwl_SetFontSize; v[36]=dwl_SetUnderline; v[37]=dwl_SetStrikethrough; v[38]=dwl_SetDrawingEffect;
    v[39]=dwl_SetInlineObject; v[40]=dwl_SetTypography; v[41]=dwl_SetLocaleName; v[42]=dwl_GetMaxWidth; v[43]=dwl_GetMaxHeight; v[44]=dwl_GetFontCollectionAt;
    v[45]=dwl_GetFontFamilyNameLengthAt; v[46]=dwl_GetFontFamilyNameAt; v[47]=dwl_GetFontWeightAt; v[48]=dwl_GetFontStyleAt; v[49]=dwl_GetFontStretchAt; v[50]=dwl_GetFontSizeAt;
    v[51]=dwl_GetUnderlineAt; v[52]=dwl_GetStrikethroughAt; v[53]=dwl_GetDrawingEffectAt; v[54]=dwl_GetNullAt; v[55]=dwl_GetNullAt; v[56]=dwl_GetLocaleNameLengthAt;
    v[57]=dwl_GetLocaleNameAt; v[59]=dwl_GetLineMetrics; v[60]=dwl_GetMetrics; v[61]=dwl_GetOverhangMetrics; v[62]=dwl_GetClusterMetrics; v[63]=dwl_DetermineMinWidth;
    v[64]=dwl_HitTestPoint; v[65]=dwl_HitTestTextPosition; v[66]=dwl_HitTestTextRange;
    v[67]=dwl_SetPairKerning; v[68]=dwl_GetPairKerning; v[69]=dwl_SetCharacterSpacing; v[70]=dwl_GetCharacterSpacing; }
static void dw_setup_fac(void**v){ v[3]=dwfac_GetSystemFontCollection; v[5]=dwfac_ok; v[6]=dwfac_ok; v[7]=dwfac_CreateFontFileReference; v[10]=dwfac_CreateRenderingParams;
    v[11]=dwfac_CreateMonitorRenderingParams; v[12]=dwfac_CreateCustomRenderingParams; v[13]=dwfac_ok; v[14]=dwfac_ok; v[15]=dwfac_CreateTextFormat; v[16]=dwfac_CreateTypography;
    v[18]=dwfac_CreateTextLayout; v[19]=dwfac_CreateGdiCompatibleTextLayout; v[20]=dwfac_CreateEllipsisTrimmingSign; v[36]=dwfac_CreateFontSetBuilder;
    v[37]=dwfac_CreateFontCollectionFromFontSet; v[38]=dwfac_GetSystemFontCollection3; v[43]=dwfac_CreateFontSetBuilder; v[44]=dwfac_CreateInMemoryFontFileLoader; }
static void dw_setup_coll(void**v){ v[3]=dwcoll_GetFontFamilyCount; v[5]=dwcoll_FindFamilyName; v[7]=dwcoll_GetFontSet; }
static void dw_setup_file(void**v){ v[4]=dwfile_GetLoader; v[5]=dwfile_Analyze; }
static void dw_setup_set(void**v){ v[3]=dwset_GetFontCount; }
static void dw_setup_builder(void**v){ v[5]=dwb_AddFontSet; v[6]=dwb_CreateFontSet; v[7]=dwb_AddFontFile; }
static void dw_setup_memloader(void**v){ v[4]=dwml_CreateRef; v[5]=dwml_GetFileCount; }
static void dw_setup_params(void**v){ v[3]=dwp_gamma; v[4]=dwp_contrast; v[5]=dwp_ctlevel; v[6]=dwp_zero; v[7]=dwp_zero; }
static void dw_setup_typo(void**v){ v[3]=dwt_AddFontFeature; v[4]=dwp_zero; }
static void dw_setup_inline(void**v){ v[4]=dwi_GetMetrics; v[5]=dwi_GetOverhang; v[6]=dwi_GetBreakConditions; }
static D2Cls C_dwfac={"IDWriteFactory5",SLOTS_IDWriteFactory5,NSLOT_IDWriteFactory5,{&IID_IUnknown,&IID_IDWriteFactory,&IID_IDWriteFactory1,&IID_IDWriteFactory2,&IID_IDWriteFactory3,&IID_IDWriteFactory4,&IID_IDWriteFactory5},0,dw_setup_fac};
static D2Cls C_dwcoll={"IDWriteFontCollection1",SLOTS_IDWriteFontCollection1,NSLOT_IDWriteFontCollection1,{&IID_IUnknown,&IID_IDWriteFontCollection,&IID_IDWriteFontCollection1},dw_coll_dtor,dw_setup_coll};
static D2Cls C_dwfile={"IDWriteFontFile",SLOTS_IDWriteFontFile,NSLOT_IDWriteFontFile,{&IID_IUnknown,&IID_IDWriteFontFile},0,dw_setup_file};
static D2Cls C_dwset={"IDWriteFontSet",SLOTS_IDWriteFontSet,NSLOT_IDWriteFontSet,{&IID_IUnknown,&IID_IDWriteFontSet},dw_set_dtor,dw_setup_set};
static D2Cls C_dwbuilder={"IDWriteFontSetBuilder1",SLOTS_IDWriteFontSetBuilder1,NSLOT_IDWriteFontSetBuilder1,{&IID_IUnknown,&IID_IDWriteFontSetBuilder,&IID_IDWriteFontSetBuilder1},dw_set_dtor,dw_setup_builder};
static D2Cls C_dwmemloader={"IDWriteInMemoryFontFileLoader",SLOTS_IDWriteInMemoryFontFileLoader,NSLOT_IDWriteInMemoryFontFileLoader,{&IID_IUnknown,&IID_IDWriteFontFileLoader,&IID_IDWriteInMemoryFontFileLoader},0,dw_setup_memloader};
static D2Cls C_dwfmt={"IDWriteTextFormat",SLOTS_IDWriteTextFormat,NSLOT_IDWriteTextFormat,{&IID_IUnknown,&IID_IDWriteTextFormat},dw_fmt_dtor,dw_setup_fmt};
static D2Cls C_dwlayout={"IDWriteTextLayout1",SLOTS_IDWriteTextLayout1,NSLOT_IDWriteTextLayout1,{&IID_IUnknown,&IID_IDWriteTextFormat,&IID_IDWriteTextLayout,&IID_IDWriteTextLayout1},dw_fmt_dtor,dw_setup_layout};
static D2Cls C_dwparams={"IDWriteRenderingParams",SLOTS_IDWriteRenderingParams,NSLOT_IDWriteRenderingParams,{&IID_IUnknown,&IID_IDWriteRenderingParams},0,dw_setup_params};
static D2Cls C_dwtypo={"IDWriteTypography",SLOTS_IDWriteTypography,NSLOT_IDWriteTypography,{&IID_IUnknown,&IID_IDWriteTypography},0,dw_setup_typo};
static D2Cls C_dwinline={"IDWriteInlineObject",SLOTS_IDWriteInlineObject,NSLOT_IDWriteInlineObject,{&IID_IUnknown,&IID_IDWriteInlineObject},0,dw_setup_inline};
// DWriteCreateFactory(type, iid, out): one shared factory (DWRITE_FACTORY_TYPE_SHARED or ISOLATED -- the same here)
static int MSABI i_DWriteCreateFactory(int type,const GUID_*iid,void**pp){ (void)type; if(!pp) return (int)HR_INVALIDARG;
    if(getenv("PELOAD_NO_D2D")){ *pp=0; return (int)HR_E_NOTIMPL; }
    static DwObj*fac; if(!fac){ fac=d2_alloc(&C_dwfac,sizeof(DwObj)); fac->h.refs=1<<30; }
    return d2_QI(fac,iid,pp); }
