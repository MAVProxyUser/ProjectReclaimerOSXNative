// dxbc_cpu.c — CPU interpreter for DXBC (SM5) compute shaders.
// macOS OpenGL stops at 4.1: no compute shaders, no storage buffers. halo3 simulates its GPU particles
// (muzzle flashes, explosions, sparks) with small compute kernels -- numthreads(64,1,1), one
// RWStructuredBuffer, Dispatch(1,1,1)-sized -- so the shim runs them here, on the CPU copy of the
// buffers, and the particle vertex shaders read the result through GL texture buffers.
// Included by peload.c after dxbc2glsl.c (reuses dg_decode_program / DgIns / DgOp).

typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; } CsV4;

typedef struct {
    DgIns* ins; int nins;
    int ntemps;
    int nx; uint32_t xreg[8], xbase[8], xcount[8]; int xtotal;   // indexable temps x#[n]
    int* jump;        // if->else/endif, else->endif, loop<->endloop, break/continue -> their loop
    uint32_t tg[3];   // numthreads
    uint32_t uavstride[8], srvstride[32];
    int ok; const char* why;
    int usestex;      // has sample*/ld instructions (textures are decoded per dispatch only then)
    uint32_t* icb; int icbn;   // this program's immediate constant buffer (g_icb is shared with the GLSL translator)
} CsProg;

typedef struct {
    const uint8_t* cb[16]; uint32_t cbsz[16];   // constant buffer bytes
    D3DObj* srv[32]; D3DObj* uav[8]; D3DObj* samp[16];
    const float* tpx[32]; int tw[32], th[32];   // SRV textures decoded to RGBA32F once per dispatch (lock-free in threads)
    uint32_t* wlog; int wlogn, wlogcap, wlogovf;   // optional: u0 elements store_structured wrote (GPU particles: upload just those)
    int trace;                                     // debug: print thread 0's every destination register (scalar interpreter)
} CsCtx;
static inline void cs_wlog(CsCtx*c,uint32_t e){ if(!c->wlog) return; if(c->wlogn && c->wlog[c->wlogn-1]==e) return; if(c->wlogn<c->wlogcap) c->wlog[c->wlogn++]=e; else c->wlogovf=1; }

static uint32_t cs_rd_u32(const uint8_t* p, uint64_t size, uint64_t off){ uint32_t v=0; if(off+4<=size) memcpy(&v,p+off,4); return v; }

// ---- program preparation ----
static void cs_free(CsProg* p){ if(!p) return; free(p->ins); free(p->jump); free(p->icb); free(p); }
static CsProg* cs_compile(const uint8_t* d, size_t len){
    CsProg* p=calloc(1,sizeof *p); if(!p) return 0;
    if(len<32||memcmp(d,"DXBC",4)){ p->why="not DXBC"; return p; }
    uint32_t nch=dg_u32(d,len,28); size_t shex=0,sz=0;
    for(uint32_t i=0;i<nch;i++){ size_t o=dg_u32(d,len,32+4*i); if(o+8>len) continue; if(!memcmp(d+o,"SHEX",4)||!memcmp(d+o,"SHDR",4)){ shex=o+8; sz=dg_u32(d,len,o+4); } }
    if(!shex){ p->why="no SHEX";   // keep the blob for a look (~/reclaimer_cs_nocode_<n>.dxbc) + its chunk names in the log
        static int nd; if(nd<4){ const char*h=getenv("HOME"); char pth[300]; snprintf(pth,sizeof pth,"%s/reclaimer_cs_nocode_%d.dxbc",h?h:"/tmp",nd++); FILE*f=fopen(pth,"wb"); if(f){ fwrite(d,1,len,f); fclose(f); }
            char m[300]; int k=snprintf(m,sizeof m,"  [cs-cpu] no code chunk in a %zu-byte DXBC (header size %u, %u chunks:",len,dg_u32(d,len,24),nch);
            for(uint32_t i=0;i<nch && i<12;i++){ size_t o=dg_u32(d,len,32+4*i); if(o+4<=len) k+=snprintf(m+k,sizeof m-k," %.4s",(const char*)d+o); else k+=snprintf(m+k,sizeof m-k," @%zu?",o); }
            k+=snprintf(m+k,sizeof m-k,")\n"); (void)!write(2,m,(size_t)k); }
        return p; }
    p->ins=calloc(8192,sizeof(DgIns)); if(!p->ins){ p->why="oom"; return p; }
    g_icbn=0; dg_decode_program(d,len,shex,sz,p->ins,&p->nins,8192);
    if(g_icbn>0){ p->icb=malloc((size_t)g_icbn*4); if(p->icb){ memcpy(p->icb,g_icb,(size_t)g_icbn*4); p->icbn=g_icbn; } }
    p->jump=calloc(p->nins+1,sizeof(int));
    int st[256], sp=0;           // control-flow stack of instruction indices
    int lst[64], lsp=0;          // loop stack
    p->tg[0]=p->tg[1]=p->tg[2]=1;
    for(int i=0;i<p->nins;i++){ DgIns* in=&p->ins[i]; int op=in->op;
        // register counts come from the bytecode (a map file's): refused before they can go negative or wrap (2026-10-19)
        if(op==104){ if(in->raw[0]>4096){ p->why="too many registers"; return p; } p->ntemps=(int)in->raw[0]; }
        else if(op==105 && p->nx<8){ if(in->raw[1]>65536 || (uint32_t)p->xtotal+in->raw[1]>65536){ p->why="too many registers"; return p; }
            p->xreg[p->nx]=in->raw[0]; p->xbase[p->nx]=(uint32_t)p->xtotal; p->xcount[p->nx]=in->raw[1]; p->xtotal+=(int)in->raw[1]; p->nx++; }
        else if(op==155){ p->tg[0]=in->raw[0]; p->tg[1]=in->raw[1]; p->tg[2]=in->raw[2]; }
        else if(op==158 && in->nop && in->ops[0].nidx){ uint32_t r=in->ops[0].idx[0].val; if(r<8) p->uavstride[r]=in->raw[2]; }
        else if(op==162 && in->nop && in->ops[0].nidx){ uint32_t r=in->ops[0].idx[0].val; if(r<32) p->srvstride[r]=in->raw[2]; }
        else if(op==31/*if*/){ if(sp<256) st[sp++]=i; }
        else if(op==18/*else*/){ if(sp){ p->jump[st[sp-1]]=i; st[sp-1]=i; } }
        else if(op==21/*endif*/){ if(sp){ p->jump[st[--sp]]=i; } }
        else if(op==48/*loop*/){ if(sp<256) st[sp++]=i; if(lsp<64) lst[lsp++]=i; }
        else if(op==22/*endloop*/){ if(sp){ int L=st[--sp]; p->jump[L]=i; p->jump[i]=L; } if(lsp) lsp--; }
        else if(op==2||op==3||op==7||op==8){ p->jump[i]=lsp?lst[lsp-1]:-1; }   // break/breakc/continue/continuec -> loop
        else if(op==76/*switch*/||op==6||op==10||op==23){ p->why="switch unsupported"; return p; }
        if(op==72||op==69||op==74||op==45) p->usestex=1;
    }
    if(p->ntemps>4096||p->xtotal>65536){ p->why="too many registers"; return p; }
    // A 4-component immediate l(a,b,c,d) has no component selection (mask mode, empty mask), which dg_swz reads as .xxxx:
    // its lanes are taken as they are. (Until 2026-10-17 every such immediate was broadcast from lane x -- e.g.
    // "and r2.yz, r2.zzwz, l(0,65535,65535,0)" gave 0 -- and halo3's particle kernels unpacked their packed half-float
    // fields to zero; found by comparing with the GPU form, PELOAD_CS_GPUCHECK. PELOAD_CS_IMMBCAST=1 = old.)
    static int immold=-1; if(immold<0) immold=getenv("PELOAD_CS_IMMBCAST")?1:0;
    for(int i=0;i<p->nins;i++) for(int k=0;k<8;k++){ DgOp* o=&p->ins[i].ops[k]; char sw[5]; dg_swz(o,sw);
        if(o->type==T_IMM32 && !immold){ for(int c=0;c<4;c++) o->csw[c]=(uint8_t)c; continue; }
        for(int c=0;c<4;c++) o->csw[c]=(uint8_t)(sw[c]=='x'?0:sw[c]=='y'?1:sw[c]=='z'?2:3); }
    // the decoder's 8192-entry array (~9 MB) trimmed to the program (2026-10-18: one per compiled kernel was most of the
    // memory a map change left behind -- every map load compiled its fresh copies of the same kernels)
    { DgIns*t=realloc(p->ins,(size_t)(p->nins>0?p->nins:1)*sizeof(DgIns)); if(t) p->ins=t; }
    p->ok=1; return p;
}

// ---- texture fetch (sample_l / ld) on the CPU copy of a texture: mip 0, bilinear ----
typedef struct { D3DObj* tex; uint32_t ver; int w,h; float* px; } CsTexCache;
static CsTexCache g_cstex[16];
static pthread_mutex_t g_cstex_mx=PTHREAD_MUTEX_INITIALIZER;   // parallel compute chunks share the cache
static const float* cs_tex_rgba_l(D3DObj* t,int* w,int* h);
static const float* cs_tex_rgba(D3DObj* t,int* w,int* h){ pthread_mutex_lock(&g_cstex_mx); const float*r=cs_tex_rgba_l(t,w,h); pthread_mutex_unlock(&g_cstex_mx); return r; }
static const float* cs_tex_rgba_l(D3DObj* t,int* w,int* h){
    if(!t||!t->data||!t->w) return 0;
    for(int i=0;i<16;i++) if(g_cstex[i].tex==t && g_cstex[i].ver==t->ver){ *w=g_cstex[i].w; *h=g_cstex[i].h; return g_cstex[i].px; }
    int W=(int)t->w, H=(int)(t->h?t->h:1); if((uint64_t)W*H>4096ull*4096ull) return 0;
    float* px=malloc((size_t)W*H*16); if(!px) return 0;
    uint32_t f=t->fmt; const uint8_t* d=t->data;
    uint8_t* rgba8=0; if(is_bc(f)) rgba8=dg_bc_decode(f,d,(size_t)t->size,W,H);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){ float* o=px+((size_t)y*W+x)*4; o[0]=o[1]=o[2]=0; o[3]=1;
        size_t i=(size_t)y*W+x;
        if(rgba8){ for(int c=0;c<4;c++) o[c]=rgba8[i*4+c]/255.0f; if(f==81||f==84) for(int c=0;c<2;c++) o[c]=((int8_t)rgba8[i*4+c])/127.0f; }
        else switch(f){
            case 27: case 28: case 29: for(int c=0;c<4;c++) o[c]=d[i*4+c]/255.0f; break;
            case 87: case 88: case 90: case 91: o[0]=d[i*4+2]/255.0f; o[1]=d[i*4+1]/255.0f; o[2]=d[i*4]/255.0f; o[3]=(f==88)?1.0f:d[i*4+3]/255.0f; break;
            case 2: memcpy(o,d+i*16,16); break;
            case 16: memcpy(o,d+i*8,8); o[2]=0; break;
            case 41: memcpy(o,d+i*4,4); break;
            case 10: { const uint16_t* h4=(const uint16_t*)(d+i*8); for(int c=0;c<4;c++){ uint32_t hb=h4[c]; uint32_t e=(hb>>10)&31, m=hb&1023; float v= e==0? m*5.9604645e-8f : (e==31? 65504.0f : ldexpf(1.0f+m/1024.0f,(int)e-15)); o[c]=(hb&0x8000)?-v:v; } } break;
            case 34: { const uint16_t* h2=(const uint16_t*)(d+i*4); for(int c=0;c<2;c++){ uint32_t hb=h2[c]; uint32_t e=(hb>>10)&31, m=hb&1023; float v= e==0? m*5.9604645e-8f : ldexpf(1.0f+m/1024.0f,(int)e-15); o[c]=(hb&0x8000)?-v:v; } } break;
            case 61: o[0]=d[i]/255.0f; break;
            case 49: o[0]=d[i*2]/255.0f; o[1]=d[i*2+1]/255.0f; break;
            case 24: { uint32_t v; memcpy(&v,d+i*4,4); o[0]=(v&1023)/1023.0f; o[1]=((v>>10)&1023)/1023.0f; o[2]=((v>>20)&1023)/1023.0f; o[3]=(v>>30)/3.0f; } break;
            default: break; } }
    free(rgba8);
    static int rr; int slot=-1; for(int i=0;i<16;i++) if(g_cstex[i].tex==t){ slot=i; break; }
    if(slot<0) slot=(rr++)&15;
    free(g_cstex[slot].px); g_cstex[slot].tex=t; g_cstex[slot].ver=t->ver; g_cstex[slot].w=W; g_cstex[slot].h=H; g_cstex[slot].px=px;
    *w=W; *h=H; return px;
}
static int cs_addr(int i,int n,uint32_t mode){ if(n<=0) return 0;
    if(mode==1){ i%=n; if(i<0) i+=n; return i; }                       // WRAP
    if(mode==2){ int p=2*n; i%=p; if(i<0) i+=p; return i<n?i:p-1-i; }  // MIRROR
    return i<0?0:(i>=n?n-1:i); }                                       // CLAMP/BORDER/MIRROR_ONCE
static void cs_sample(const float* px,int W,int H,D3DObj* s,float u,float v,float* out){
    out[0]=out[1]=out[2]=0; out[3]=0; if(!px) return;
    uint32_t au=1,av=1, filt=0x15; if(s&&s->stkind==4){ filt=s->sd[0]; au=s->sd[1]; av=s->sd[2]; }
    if(!(filt&0x14)){ int x=cs_addr((int)floorf(u*W),W,au), y=cs_addr((int)floorf(v*H),H,av); memcpy(out,px+((size_t)y*W+x)*4,16); return; }
    float fx=u*W-0.5f, fy=v*H-0.5f; int x0=(int)floorf(fx), y0=(int)floorf(fy); float ax=fx-x0, ay=fy-y0;
    int xa=cs_addr(x0,W,au), xb=cs_addr(x0+1,W,au), ya=cs_addr(y0,H,av), yb=cs_addr(y0+1,H,av);
    const float *p00=px+((size_t)ya*W+xa)*4,*p10=px+((size_t)ya*W+xb)*4,*p01=px+((size_t)yb*W+xa)*4,*p11=px+((size_t)yb*W+xb)*4;
    for(int c=0;c<4;c++) out[c]=(p00[c]*(1-ax)+p10[c]*ax)*(1-ay)+(p01[c]*(1-ax)+p11[c]*ax)*ay;
}

// ---- execution ----
typedef struct { const CsProg* p; CsCtx* c; CsV4* r; CsV4* x; uint32_t tid[3], gid[3], ltid[3]; } CsThread;

static int32_t cs_relidx(CsThread* T,const DgIdx* ix){
    int32_t v=(int32_t)ix->val;
    if(ix->kind==1){ uint32_t comp=(uint32_t)(ix->rcomp&3);
        if(ix->rtype==T_TEMP && ix->rreg<(uint32_t)T->p->ntemps) v+=T->r[ix->rreg].i[comp];
        else if(ix->rtype==T_INDEXABLE_TEMP){ uint32_t xr=ix->rreg&0xffff, xi=ix->rreg>>16;
            for(int k=0;k<T->p->nx;k++) if(T->p->xreg[k]==xr && xi<T->p->xcount[k]) v+=T->x[T->p->xbase[k]+xi].i[comp]; } }
    return v; }
static CsV4* cs_xref(CsThread* T,const DgOp* o){
    uint32_t xr=o->nidx?o->idx[0].val:0; int32_t i=o->nidx>1?cs_relidx(T,&o->idx[1]):0;
    for(int k=0;k<T->p->nx;k++) if(T->p->xreg[k]==xr){ if(i<0||(uint32_t)i>=T->p->xcount[k]) return 0; return &T->x[T->p->xbase[k]+(uint32_t)i]; }
    return 0; }
// read a source operand (raw bits, swizzled; NEG/ABS applied as float or int per isint)
static CsV4 cs_src(CsThread* T,const DgOp* o,int isint){
    CsV4 v; memset(&v,0,sizeof v);
    switch(o->type){
        case T_TEMP: if(o->nidx && o->idx[0].val<(uint32_t)T->p->ntemps) v=T->r[o->idx[0].val]; break;
        case T_IMM32: if(o->immn==1) v.u[0]=v.u[1]=v.u[2]=v.u[3]=o->imm[0]; else for(int k=0;k<4;k++) v.u[k]=o->imm[k]; break;
        case T_CB: { uint32_t b=o->nidx?o->idx[0].val:0; int32_t i=o->nidx>1?cs_relidx(T,&o->idx[1]):0;
            if(b<16 && T->c->cb[b] && i>=0 && ((uint64_t)i+1)*16<=T->c->cbsz[b]) memcpy(&v,T->c->cb[b]+(size_t)i*16,16); } break;
        case T_ICB: { int32_t i=o->nidx?cs_relidx(T,&o->idx[0]):0; if(i>=0 && (int64_t)i*4+4<=(int64_t)T->p->icbn) memcpy(&v,&T->p->icb[i*4],16); } break;
        case T_INDEXABLE_TEMP: { CsV4* p=cs_xref(T,o); if(p) v=*p; } break;
        case 32: v.u[0]=T->tid[0]; v.u[1]=T->tid[1]; v.u[2]=T->tid[2]; break;     // vThreadID
        case 33: v.u[0]=T->gid[0]; v.u[1]=T->gid[1]; v.u[2]=T->gid[2]; break;     // vThreadGroupID
        case 34: v.u[0]=T->ltid[0]; v.u[1]=T->ltid[1]; v.u[2]=T->ltid[2]; break;  // vThreadIDInGroup
        case 35: v.u[0]=T->ltid[0]+T->ltid[1]*T->p->tg[0]+T->ltid[2]*T->p->tg[0]*T->p->tg[1]; v.u[1]=v.u[2]=v.u[3]=v.u[0]; break;
        default: break; }
    CsV4 s=v;
    if(o->numc==2){ const uint8_t* sw=o->csw; s.u[0]=v.u[sw[0]]; s.u[1]=v.u[sw[1]]; s.u[2]=v.u[sw[2]]; s.u[3]=v.u[sw[3]]; }
    else if(o->numc==1){ s.u[1]=s.u[2]=s.u[3]=s.u[0]; }
    if(o->emod&2){ for(int k=0;k<4;k++){ if(isint) s.i[k]=s.i[k]<0?-s.i[k]:s.i[k]; else s.u[k]&=0x7fffffffu; } }
    if(o->emod&1){ for(int k=0;k<4;k++){ if(isint) s.i[k]=-s.i[k]; else s.u[k]^=0x80000000u; } }
    return s; }
static void cs_dst(CsThread* T,const DgOp* o,CsV4 v,int sat){
    if(o->type==T_NULL) return;
    if(sat) for(int k=0;k<4;k++){ float f=v.f[k]; v.f[k]= (f>0.0f)? (f<1.0f?f:1.0f) : 0.0f; }   // NaN -> 0
    CsV4* d=0;
    if(o->type==T_TEMP){ if(o->nidx && o->idx[0].val<(uint32_t)T->p->ntemps) d=&T->r[o->idx[0].val]; }
    else if(o->type==T_INDEXABLE_TEMP) d=cs_xref(T,o);
    if(!d) return;
    int m=o->mask?o->mask:0xf; for(int k=0;k<4;k++) if(m&(1<<k)) d->u[k]=v.u[k]; }
static uint32_t cs_h2f_bits(uint32_t h){ uint32_t s=(h&0x8000)<<16, e=(h>>10)&31, m=h&1023; float v;
    if(e==0) v=m*5.9604645e-8f; else if(e==31) { uint32_t b=s|0x7f800000u|(m<<13); return b; } else v=ldexpf(1.0f+m/1024.0f,(int)e-15);
    uint32_t b; memcpy(&b,&v,4); return b|s; }
static uint32_t cs_f2h(float f){ uint32_t b; memcpy(&b,&f,4); uint32_t s=(b>>16)&0x8000u; int e=(int)((b>>23)&255)-112; uint32_t m=b&0x7fffff;
    if(e<=0) return s; if(e>=31) return s|0x7c00u; return s|((uint32_t)e<<10)|(m>>13); }

// a structured/raw buffer view: resource + its byte stride
static D3DObj* cs_res(D3DObj* view){ return (view&&view->data&&view->vkind)?(D3DObj*)view->data:view; }

static long g_cs_instrs;   // executed-instruction counter (PELOAD_D3DGL_PROF; approximate when parallel)
static int cs_exec_thread_i(CsThread* T,long*budget);
static int cs_exec_thread(CsThread* T){ long budget=4000000; int rc=cs_exec_thread_i(T,&budget); g_cs_instrs+=4000000-budget; return rc; }
static int cs_exec_thread_i(CsThread* T,long*budgetp){
    const CsProg* p=T->p; const DgIns* in=p->ins; int n=p->nins;
    long budget=*budgetp;   // runaway guard
    for(int pc=0; pc<n; pc++){
        if(--budget<0){ *budgetp=budget; return 0; }
        const DgIns* I=&in[pc]; int op=I->op; const DgOp* o=I->ops; int sat=(op<88)&&((I->tok>>13)&1);
        #define F(k) cs_src(T,&o[k],0)
        #define U(k) cs_src(T,&o[k],1)
        CsV4 a,b,c,r; memset(&r,0,sizeof r);
        switch(op){
        // ---- control flow ----
        case 31: a=U(0); if((a.u[0]!=0)!=(I->test!=0)) pc=p->jump[pc]; break;              // if_z/if_nz: false -> else/endif
        case 18: pc=p->jump[pc]; break;                                                      // else: skip to endif
        case 21: break;
        case 48: break;
        case 22: pc=p->jump[pc]; break;                                                      // endloop -> loop
        case 2: { int L=p->jump[pc]; if(L>=0) pc=p->jump[L]; } break;                        // break -> endloop (then +1)
        case 3: a=U(0); if((a.u[0]!=0)==(I->test!=0)){ int L=p->jump[pc]; if(L>=0) pc=p->jump[L]; } break;
        case 7: { int L=p->jump[pc]; if(L>=0) pc=L; } break;                                 // continue -> loop
        case 8: a=U(0); if((a.u[0]!=0)==(I->test!=0)){ int L=p->jump[pc]; if(L>=0) pc=L; } break;
        case 62: *budgetp=budget; return 1;
        case 63: a=U(0); if((a.u[0]!=0)==(I->test!=0)){ *budgetp=budget; return 1; } break;
        case 13: break;   // discard (n/a)
        // ---- float ALU ----
        case 0: a=F(1); b=F(2); for(int k=0;k<4;k++) r.f[k]=a.f[k]+b.f[k]; cs_dst(T,&o[0],r,sat); break;
        case 56: a=F(1); b=F(2); for(int k=0;k<4;k++) r.f[k]=a.f[k]*b.f[k]; cs_dst(T,&o[0],r,sat); break;
        case 50: a=F(1); b=F(2); c=F(3); for(int k=0;k<4;k++) r.f[k]=a.f[k]*b.f[k]+c.f[k]; cs_dst(T,&o[0],r,sat); break;
        case 14: a=F(1); b=F(2); for(int k=0;k<4;k++) r.f[k]=a.f[k]/b.f[k]; cs_dst(T,&o[0],r,sat); break;
        case 51: a=F(1); b=F(2); for(int k=0;k<4;k++) r.f[k]=fminf(a.f[k],b.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 52: a=F(1); b=F(2); for(int k=0;k<4;k++) r.f[k]=fmaxf(a.f[k],b.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 75: a=F(1); for(int k=0;k<4;k++) r.f[k]=sqrtf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 68: a=F(1); for(int k=0;k<4;k++) r.f[k]=1.0f/sqrtf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 129: a=F(1); for(int k=0;k<4;k++) r.f[k]=1.0f/a.f[k]; cs_dst(T,&o[0],r,sat); break;
        case 25: a=F(1); for(int k=0;k<4;k++) r.f[k]=exp2f(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 47: a=F(1); for(int k=0;k<4;k++) r.f[k]=log2f(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 26: a=F(1); for(int k=0;k<4;k++) r.f[k]=a.f[k]-floorf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 64: a=F(1); for(int k=0;k<4;k++) r.f[k]=rintf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 65: a=F(1); for(int k=0;k<4;k++) r.f[k]=floorf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 66: a=F(1); for(int k=0;k<4;k++) r.f[k]=ceilf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 67: a=F(1); for(int k=0;k<4;k++) r.f[k]=truncf(a.f[k]); cs_dst(T,&o[0],r,sat); break;
        case 15: a=F(1); b=F(2); { float d=a.f[0]*b.f[0]+a.f[1]*b.f[1]; for(int k=0;k<4;k++) r.f[k]=d; } cs_dst(T,&o[0],r,sat); break;
        case 16: a=F(1); b=F(2); { float d=a.f[0]*b.f[0]+a.f[1]*b.f[1]+a.f[2]*b.f[2]; for(int k=0;k<4;k++) r.f[k]=d; } cs_dst(T,&o[0],r,sat); break;
        case 17: a=F(1); b=F(2); { float d=a.f[0]*b.f[0]+a.f[1]*b.f[1]+a.f[2]*b.f[2]+a.f[3]*b.f[3]; for(int k=0;k<4;k++) r.f[k]=d; } cs_dst(T,&o[0],r,sat); break;
        case 77: a=F(2); { CsV4 s2,c2; for(int k=0;k<4;k++){ s2.f[k]=sinf(a.f[k]); c2.f[k]=cosf(a.f[k]); } cs_dst(T,&o[0],s2,sat); cs_dst(T,&o[1],c2,sat); } break;
        case 54: a=F(1); cs_dst(T,&o[0],a,sat); break;                                             // mov (bits)
        case 55: a=U(1); b=F(2); c=F(3); for(int k=0;k<4;k++) r.u[k]=a.u[k]?b.u[k]:c.u[k]; cs_dst(T,&o[0],r,sat); break;  // movc
        case 49: a=F(1); b=F(2); for(int k=0;k<4;k++) r.u[k]=a.f[k]<b.f[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 29: a=F(1); b=F(2); for(int k=0;k<4;k++) r.u[k]=a.f[k]>=b.f[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 24: a=F(1); b=F(2); for(int k=0;k<4;k++) r.u[k]=a.f[k]==b.f[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 57: a=F(1); b=F(2); for(int k=0;k<4;k++) r.u[k]=a.f[k]!=b.f[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 11: case 12: case 122: case 123: case 124: case 125: cs_dst(T,&o[0],r,sat); break;   // derivatives: 0
        case 131: a=U(1); for(int k=0;k<4;k++) r.u[k]=cs_h2f_bits(a.u[k]&0xffff); cs_dst(T,&o[0],r,sat); break;   // f16tof32
        case 130: a=F(1); for(int k=0;k<4;k++) r.u[k]=cs_f2h(a.f[k]); cs_dst(T,&o[0],r,0); break;             // f32tof16
        // ---- integer ALU ----
        case 30: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]+b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 38: if(I->nop>3){ a=U(2); b=U(3); CsV4 hi; for(int k=0;k<4;k++){ int64_t m=(int64_t)a.i[k]*b.i[k]; r.i[k]=(int32_t)m; hi.i[k]=(int32_t)(m>>32); } cs_dst(T,&o[0],hi,0); cs_dst(T,&o[1],r,0); } break;
        case 81: if(I->nop>3){ a=U(2); b=U(3); CsV4 hi; for(int k=0;k<4;k++){ uint64_t m=(uint64_t)a.u[k]*b.u[k]; r.u[k]=(uint32_t)m; hi.u[k]=(uint32_t)(m>>32); } cs_dst(T,&o[0],hi,0); cs_dst(T,&o[1],r,0); } break;
        case 78: if(I->nop>3){ a=U(2); b=U(3); CsV4 q; for(int k=0;k<4;k++){ q.u[k]=b.u[k]?a.u[k]/b.u[k]:~0u; r.u[k]=b.u[k]?a.u[k]%b.u[k]:~0u; } cs_dst(T,&o[0],q,0); cs_dst(T,&o[1],r,0); } break;
        case 35: case 82: a=U(1); b=U(2); c=U(3); for(int k=0;k<4;k++) r.u[k]=a.u[k]*b.u[k]+c.u[k]; cs_dst(T,&o[0],r,0); break;   // imad/umad (low 32 bits)
        case 36: a=U(1); b=U(2); for(int k=0;k<4;k++) r.i[k]=a.i[k]>b.i[k]?a.i[k]:b.i[k]; cs_dst(T,&o[0],r,0); break;
        case 37: a=U(1); b=U(2); for(int k=0;k<4;k++) r.i[k]=a.i[k]<b.i[k]?a.i[k]:b.i[k]; cs_dst(T,&o[0],r,0); break;
        case 83: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]>b.u[k]?a.u[k]:b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 84: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]<b.u[k]?a.u[k]:b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 40: a=U(1); for(int k=0;k<4;k++) r.i[k]=-a.i[k]; cs_dst(T,&o[0],r,0); break;
        case 41: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]<<(b.u[k]&31); cs_dst(T,&o[0],r,0); break;
        case 42: a=U(1); b=U(2); for(int k=0;k<4;k++) r.i[k]=a.i[k]>>(b.u[k]&31); cs_dst(T,&o[0],r,0); break;
        case 85: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]>>(b.u[k]&31); cs_dst(T,&o[0],r,0); break;
        case 1: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]&b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 60: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]|b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 87: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]^b.u[k]; cs_dst(T,&o[0],r,0); break;
        case 59: a=U(1); for(int k=0;k<4;k++) r.u[k]=~a.u[k]; cs_dst(T,&o[0],r,0); break;
        case 32: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]==b.u[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 39: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]!=b.u[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 34: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.i[k]<b.i[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 33: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.i[k]>=b.i[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 79: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]<b.u[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 80: a=U(1); b=U(2); for(int k=0;k<4;k++) r.u[k]=a.u[k]>=b.u[k]?~0u:0; cs_dst(T,&o[0],r,0); break;
        case 43: a=U(1); for(int k=0;k<4;k++) r.f[k]=(float)a.i[k]; cs_dst(T,&o[0],r,sat); break;   // itof
        case 86: a=U(1); for(int k=0;k<4;k++) r.f[k]=(float)a.u[k]; cs_dst(T,&o[0],r,sat); break;   // utof
        case 27: a=F(1); for(int k=0;k<4;k++){ float f=a.f[k]; r.i[k]= f!=f?0 : f>=2147483647.0f?0x7fffffff : f<=-2147483648.0f?(int32_t)0x80000000 : (int32_t)f; } cs_dst(T,&o[0],r,0); break;   // ftoi
        case 28: a=F(1); for(int k=0;k<4;k++){ float f=a.f[k]; r.u[k]= (f!=f||f<=0)?0 : f>=4294967295.0f?0xffffffffu : (uint32_t)f; } cs_dst(T,&o[0],r,0); break;  // ftou
        case 138: case 139: { a=U(1); b=U(2); c=U(3); for(int k=0;k<4;k++){ uint32_t w=a.u[k]&31, of=b.u[k]&31;   // ubfe/ibfe width, offset, src
                if(!w){ r.u[k]=0; continue; } if(w+of<32){ if(op==138) r.u[k]=(c.u[k]<<(32-w-of))>>(32-w); else r.i[k]=(int32_t)(c.u[k]<<(32-w-of))>>(32-w); }
                else { if(op==138) r.u[k]=c.u[k]>>of; else r.i[k]=c.i[k]>>of; } } cs_dst(T,&o[0],r,0); } break;
        case 140: { CsV4 w=U(1), of=U(2), ins=U(3), base=U(4); for(int k=0;k<4;k++){ uint32_t W=w.u[k]&31, O=of.u[k]&31; uint32_t mask=(W==32?~0u:(((1u<<W)-1)<<O)); r.u[k]=((ins.u[k]<<O)&mask)|(base.u[k]&~mask); } cs_dst(T,&o[0],r,0); } break;
        case 134: a=U(1); for(int k=0;k<4;k++) r.u[k]=(uint32_t)__builtin_popcount(a.u[k]); cs_dst(T,&o[0],r,0); break;
        // ---- memory ----
        case 167: case 165: {   // ld_structured dst, idx, byteoff, t#/u#  |  ld_raw dst, byteaddr, t#/u#
            int ri=op==167?3:2; const DgOp* ro=&o[ri]; uint32_t reg=ro->nidx?ro->idx[0].val:0;
            D3DObj* res=0; uint32_t stride=4;
            if(ro->type==30/*UAV*/){ if(reg<8){ res=cs_res(T->c->uav[reg]); stride=p->uavstride[reg]; } }
            else if(ro->type==T_RESOURCE){ if(reg<32){ res=cs_res(T->c->srv[reg]); stride=p->srvstride[reg]; } }
            a=U(1); uint64_t addr = op==167 ? (uint64_t)a.u[0]*stride + (b=U(2)).u[0] : a.u[0];
            CsV4 raw4; for(int k=0;k<4;k++) raw4.u[k]= res? cs_rd_u32(res->data,res->size,addr+4u*k) : 0;
            for(int k=0;k<4;k++) r.u[k]=raw4.u[ro->csw[k]];
            cs_dst(T,&o[0],r,0); } break;
        case 168: case 166: {   // store_structured u#.mask, idx, byteoff, src  |  store_raw u#.mask, byteaddr, src
            uint32_t reg=o[0].nidx?o[0].idx[0].val:0; if(reg>=8) break; D3DObj* res=cs_res(T->c->uav[reg]); if(!res||!res->data) break;
            a=U(1); uint64_t addr; CsV4 v;
            if(op==168){ b=U(2); addr=(uint64_t)a.u[0]*p->uavstride[reg]+b.u[0]; v=U(3); if(reg==0) cs_wlog(T->c,a.u[0]); } else { addr=a.u[0]; v=U(2); if(reg==0 && T->c->wlog) T->c->wlogovf=1; }
            int m=o[0].mask?o[0].mask:1; int j=0;
            for(int k=0;k<4;k++) if(m&(1<<k)){ uint64_t off=addr+4u*(uint64_t)j++; if(off+4<=res->size) memcpy((uint8_t*)res->data+off,&v.u[k],4); }
            } break;
        case 72: case 69: case 74: {   // sample_l / sample / sample_b dst, uv, t#, s#, lod  (mip 0, bilinear)
            a=F(1); uint32_t tr=o[2].nidx?o[2].idx[0].val:0, sr=o[3].nidx?o[3].idx[0].val:0;
            D3DObj* s=sr<16?T->c->samp[sr]:0;
            float px[4]; if(tr<32) cs_sample(T->c->tpx[tr],T->c->tw[tr],T->c->th[tr],s,a.f[0],a.f[1],px); else memset(px,0,sizeof px); CsV4 v; memcpy(&v,px,16);
            for(int k=0;k<4;k++) r.u[k]=v.u[o[2].csw[k]];
            cs_dst(T,&o[0],r,sat); } break;
        case 45: {   // ld dst, int addr, t#
            a=U(1); uint32_t tr=o[2].nidx?o[2].idx[0].val:0; int W=tr<32?T->c->tw[tr]:0,H=tr<32?T->c->th[tr]:0; const float* px=tr<32?T->c->tpx[tr]:0;
            CsV4 v; memset(&v,0,sizeof v); if(px && a.i[0]>=0 && a.i[1]>=0 && a.i[0]<W && a.i[1]<H) memcpy(&v,px+((size_t)a.i[1]*W+a.i[0])*4,16);
            for(int k=0;k<4;k++) r.u[k]=v.u[o[2].csw[k]];
            cs_dst(T,&o[0],r,sat); } break;
        default:
            if(op>=88 && op<=106) break;       // declarations
            if(op==53||op==58||op>=143) break; // customdata, nop, SM5 dcls
            *budgetp=budget; return -op;       // unsupported instruction
        }
        if(T->c->trace && T->tid[0]==0 && I->nop && o[0].type==0/*temp*/ && o[0].nidx){ uint32_t rg=o[0].idx[0].val; const CsV4*v=&T->r[rg]; char m[200];
            int k=snprintf(m,sizeof m,"  [cs-trace] %4d op%-3d r%u = %08x %08x %08x %08x  (%g %g %g %g)\n",pc,op,rg,v->u[0],v->u[1],v->u[2],v->u[3],v->f[0],v->f[1],v->f[2],v->f[3]); (void)!write(2,m,k); }
        #undef F
        #undef U
    }
    *budgetp=budget; return 1; }

// ---- SIMT executor: each instruction runs for up to 64 threads before the next one -------------------------------
// The per-thread interpreter above decodes every operand of every instruction for every thread (~21 ns/instruction
// under Rosetta; a 64-thread particle dispatch took ~235 us and the GL thread waited ~25% of its time for them).
// Here an instruction is decoded once per 64 lanes and the lane loops are plain array work. Divergent control flow
// uses execution masks: if/else/endif and loop/break/continue keep a mask stack, ret/retc retire lanes.
// PELOAD_CS_SCALAR=1 = the per-thread interpreter; PELOAD_CS_CHECK=1 runs both on every dispatch and compares the UAVs.
#define CSL 64
typedef struct { const CsProg* p; CsCtx* c; int n; CsV4* r; CsV4* x; uint32_t tid[3][CSL], gid[3][CSL], ltid[3][CSL]; uint64_t mcur; int na; uint8_t act[CSL]; } CsGrp;
// lane loops visit only the active lanes (most particle threads retire within a few instructions; the live ones run long)
#define FORL for(int q_=0,l=0; q_<G->na && ((l=G->act[q_]),1); q_++)
static inline void csg_setmask(CsGrp* G,uint64_t m){ if(m==G->mcur) return; G->mcur=m; int k=0; while(m){ int l=__builtin_ctzll(m); G->act[k++]=(uint8_t)l; m&=m-1; } G->na=k; }
typedef struct { int kind; uint64_t saved, other, brk, cont; int pc; } CsCf;   // kind 0 if, 1 loop
static inline int32_t csg_relidx(const CsGrp* G,const DgIdx* ix,int l){
    int32_t v=(int32_t)ix->val;
    if(ix->kind==1){ uint32_t comp=(uint32_t)(ix->rcomp&3);
        if(ix->rtype==T_TEMP && ix->rreg<(uint32_t)G->p->ntemps) v+=G->r[(size_t)ix->rreg*CSL+l].i[comp];
        else if(ix->rtype==T_INDEXABLE_TEMP){ uint32_t xr=ix->rreg&0xffff, xi=ix->rreg>>16;
            for(int k=0;k<G->p->nx;k++) if(G->p->xreg[k]==xr && xi<G->p->xcount[k]) v+=G->x[(size_t)(G->p->xbase[k]+xi)*CSL+l].i[comp]; } }
    return v; }
static CsV4* csg_xref(const CsGrp* G,const DgOp* o,int l){
    uint32_t xr=o->nidx?o->idx[0].val:0; int32_t i=o->nidx>1?csg_relidx(G,&o->idx[1],l):0;
    for(int k=0;k<G->p->nx;k++) if(G->p->xreg[k]==xr){ if(i<0||(uint32_t)i>=G->p->xcount[k]) return 0; return &G->x[(size_t)(G->p->xbase[k]+(uint32_t)i)*CSL+l]; }
    return 0; }
// source operand for lanes 0..n-1 (raw bits, swizzled, NEG/ABS as float or int per isint)
static void csg_src(const CsGrp* G,const DgOp* o,int isint,CsV4* out){
    int n=G->n;
    switch(o->type){
    case T_TEMP: if(o->nidx && o->idx[0].val<(uint32_t)G->p->ntemps){ const CsV4* src=&G->r[(size_t)o->idx[0].val*CSL]; if(G->na==n) memcpy(out,src,(size_t)n*sizeof(CsV4)); else FORL out[l]=src[l]; } else FORL memset(&out[l],0,16); break;
    case T_IMM32: { CsV4 v; if(o->immn==1) v.u[0]=v.u[1]=v.u[2]=v.u[3]=o->imm[0]; else for(int k=0;k<4;k++) v.u[k]=o->imm[k]; FORL out[l]=v; } break;
    case T_CB: { uint32_t b=o->nidx?o->idx[0].val:0; const uint8_t* cb=b<16?G->c->cb[b]:0; uint32_t sz=b<16?G->c->cbsz[b]:0;
        if(o->nidx>1 && o->idx[1].kind==1){ FORL { int32_t i=csg_relidx(G,&o->idx[1],l);
                if(cb && i>=0 && ((uint64_t)i+1)*16<=sz) memcpy(&out[l],cb+(size_t)i*16,16); else memset(&out[l],0,16); } }
        else { int32_t i=o->nidx>1?(int32_t)o->idx[1].val:0; CsV4 v; memset(&v,0,sizeof v); if(cb && i>=0 && ((uint64_t)i+1)*16<=sz) memcpy(&v,cb+(size_t)i*16,16); FORL out[l]=v; } } break;
    case T_ICB: if(o->nidx && o->idx[0].kind==1){ FORL { int32_t i=csg_relidx(G,&o->idx[0],l); if(i>=0 && (int64_t)i*4+4<=(int64_t)G->p->icbn) memcpy(&out[l],&G->p->icb[i*4],16); else memset(&out[l],0,16); } }
        else { int32_t i=o->nidx?(int32_t)o->idx[0].val:0; CsV4 v; memset(&v,0,sizeof v); if(i>=0 && (int64_t)i*4+4<=(int64_t)G->p->icbn) memcpy(&v,&G->p->icb[i*4],16); FORL out[l]=v; } break;
    case T_INDEXABLE_TEMP: FORL { CsV4* p=csg_xref(G,o,l); if(p) out[l]=*p; else memset(&out[l],0,16); } break;
    case 32: FORL { out[l].u[0]=G->tid[0][l]; out[l].u[1]=G->tid[1][l]; out[l].u[2]=G->tid[2][l]; out[l].u[3]=0; } break;
    case 33: FORL { out[l].u[0]=G->gid[0][l]; out[l].u[1]=G->gid[1][l]; out[l].u[2]=G->gid[2][l]; out[l].u[3]=0; } break;
    case 34: FORL { out[l].u[0]=G->ltid[0][l]; out[l].u[1]=G->ltid[1][l]; out[l].u[2]=G->ltid[2][l]; out[l].u[3]=0; } break;
    case 35: FORL { uint32_t f=G->ltid[0][l]+G->ltid[1][l]*G->p->tg[0]+G->ltid[2][l]*G->p->tg[0]*G->p->tg[1]; out[l].u[0]=out[l].u[1]=out[l].u[2]=out[l].u[3]=f; } break;
    default: FORL memset(&out[l],0,16); break; }
    if(o->numc==2){ const uint8_t* sw=o->csw; if(!(sw[0]==0&&sw[1]==1&&sw[2]==2&&sw[3]==3)) FORL { CsV4 v=out[l]; out[l].u[0]=v.u[sw[0]]; out[l].u[1]=v.u[sw[1]]; out[l].u[2]=v.u[sw[2]]; out[l].u[3]=v.u[sw[3]]; } }
    else if(o->numc==1){ FORL out[l].u[1]=out[l].u[2]=out[l].u[3]=out[l].u[0]; }
    if(o->emod&2){ if(isint){ FORL for(int k=0;k<4;k++) out[l].i[k]=out[l].i[k]<0?-out[l].i[k]:out[l].i[k]; } else { FORL for(int k=0;k<4;k++) out[l].u[k]&=0x7fffffffu; } }
    if(o->emod&1){ if(isint){ FORL for(int k=0;k<4;k++) out[l].i[k]=-out[l].i[k]; } else { FORL for(int k=0;k<4;k++) out[l].u[k]^=0x80000000u; } } }
static void csg_dst(CsGrp* G,const DgOp* o,CsV4* v,int sat,uint64_t m){
    (void)m; if(o->type==T_NULL) return; int n=G->n; int wm=o->mask?o->mask:0xf;
    if(sat) FORL for(int k=0;k<4;k++){ float f=v[l].f[k]; v[l].f[k]=(f>0.0f)?(f<1.0f?f:1.0f):0.0f; }   // NaN -> 0
    if(o->type==T_TEMP){ if(!o->nidx || o->idx[0].val>=(uint32_t)G->p->ntemps) return; CsV4* d=&G->r[(size_t)o->idx[0].val*CSL];
        if(G->na==n && wm==0xf){ memcpy(d,v,(size_t)n*sizeof(CsV4)); return; }
        if(wm==0xf) FORL d[l]=v[l]; else FORL for(int k=0;k<4;k++) if(wm&(1<<k)) d[l].u[k]=v[l].u[k]; }
    else if(o->type==T_INDEXABLE_TEMP){ FORL { CsV4* d=csg_xref(G,o,l); if(d) for(int k=0;k<4;k++) if(wm&(1<<k)) d->u[k]=v[l].u[k]; } } }
static uint64_t csg_lanes_true(const CsGrp* G,const CsV4* a,int test){ uint64_t t=0; FORL if((a[l].u[0]!=0)==(test!=0)) t|=1ull<<l; return t; }
static CsCf* csg_innerloop(CsCf* st,int sp){ for(int i=sp-1;i>=0;i--) if(st[i].kind==1) return &st[i]; return 0; }
static uint64_t csg_loopout(CsCf* st,int sp){ CsCf* L=csg_innerloop(st,sp); return L?(L->brk|L->cont):0; }
static int cs_exec_group(CsGrp* G,long* budgetp){
    const CsProg* p=G->p; const DgIns* in=p->ins; int nins=p->nins, n=G->n;
    uint64_t full=n==64?~0ull:((1ull<<n)-1), m=full, done=0;
    CsCf st[64]; int sp=0; long budget=*budgetp;
    CsV4 A[CSL],B[CSL],C[CSL],D[CSL],R[CSL],R2[CSL];
    #define LOOPL FORL
    #define F1 csg_src(G,&o[1],0,A)
    #define F2 csg_src(G,&o[1],0,A); csg_src(G,&o[2],0,B)
    #define F3 csg_src(G,&o[1],0,A); csg_src(G,&o[2],0,B); csg_src(G,&o[3],0,C)
    #define U1 csg_src(G,&o[1],1,A)
    #define U2 csg_src(G,&o[1],1,A); csg_src(G,&o[2],1,B)
    #define U3 csg_src(G,&o[1],1,A); csg_src(G,&o[2],1,B); csg_src(G,&o[3],1,C)
    #define OUT(s) csg_dst(G,&o[0],R,(s),m)
    for(int pc=0; pc<nins; pc++){
        if(--budget<0){ *budgetp=budget; return 0; }
        const DgIns* I=&in[pc]; int op=I->op; const DgOp* o=I->ops; int sat=(op<88)&&((I->tok>>13)&1);
        csg_setmask(G,m);
        switch(op){   // control flow runs even with no lane active (it keeps the mask stack balanced)
        case 31: { uint64_t t=0; if(m){ csg_src(G,&o[0],1,A); t=csg_lanes_true(G,A,I->test)&m; }
            if(sp<64){ st[sp].kind=0; st[sp].saved=m; st[sp].other=m&~t; sp++; } m=t;
            if(!m) pc=p->jump[pc]-1;   // no lane takes it: straight to its else / endif
            continue; }
        case 18: if(sp && st[sp-1].kind==0){ m=st[sp-1].other&~done&~csg_loopout(st,sp-1); if(!m) pc=p->jump[pc]-1; } continue;   // empty else: to endif
        case 21: if(sp && st[sp-1].kind==0){ sp--; m=st[sp].saved&~done&~csg_loopout(st,sp); } continue;
        case 48: if(!m){ pc=p->jump[pc]; continue; }   // nobody enters: past endloop
            if(sp<64){ st[sp].kind=1; st[sp].saved=m; st[sp].brk=st[sp].cont=0; st[sp].pc=pc; sp++; } continue;
        case 22: if(sp && st[sp-1].kind==1){ CsCf* L=&st[sp-1]; uint64_t again=(m|L->cont)&~done&~L->brk;
                if(again){ m=again; L->cont=0; pc=L->pc; } else { m=L->saved&~done; sp--; } } continue;
        case 2: { CsCf* L=csg_innerloop(st,sp); if(L) L->brk|=m; m=0; } goto cf_left;
        case 3: if(m){ csg_src(G,&o[0],1,A); uint64_t c=csg_lanes_true(G,A,I->test)&m; CsCf* L=csg_innerloop(st,sp); if(L) L->brk|=c; m&=~c; } goto cf_left;
        case 7: { CsCf* L=csg_innerloop(st,sp); if(L) L->cont|=m; m=0; } goto cf_left;
        case 8: if(m){ csg_src(G,&o[0],1,A); uint64_t c=csg_lanes_true(G,A,I->test)&m; CsCf* L=csg_innerloop(st,sp); if(L) L->cont|=c; m&=~c; } goto cf_left;
        cf_left:   // every lane left the loop body and nothing between here and the loop's endloop can bring one back
            if(!m && sp && st[sp-1].kind==1) pc=p->jump[st[sp-1].pc]-1;
            continue;
        case 62: done|=m; m=0; if((done&full)==full){ *budgetp=budget; return 1; } continue;
        case 63: if(m){ csg_src(G,&o[0],1,A); uint64_t c=csg_lanes_true(G,A,I->test)&m; done|=c; m&=~c; if((done&full)==full){ *budgetp=budget; return 1; } } continue;
        case 13: continue;
        default: break; }
        if(!m) continue;
        switch(op){
        // ---- float ALU ----
        case 0: F2; LOOPL for(int k=0;k<4;k++) R[l].f[k]=A[l].f[k]+B[l].f[k]; OUT(sat); break;
        case 56: F2; LOOPL for(int k=0;k<4;k++) R[l].f[k]=A[l].f[k]*B[l].f[k]; OUT(sat); break;
        case 50: F3; LOOPL for(int k=0;k<4;k++) R[l].f[k]=A[l].f[k]*B[l].f[k]+C[l].f[k]; OUT(sat); break;
        case 14: F2; LOOPL for(int k=0;k<4;k++) R[l].f[k]=A[l].f[k]/B[l].f[k]; OUT(sat); break;
        case 51: F2; LOOPL for(int k=0;k<4;k++) R[l].f[k]=fminf(A[l].f[k],B[l].f[k]); OUT(sat); break;
        case 52: F2; LOOPL for(int k=0;k<4;k++) R[l].f[k]=fmaxf(A[l].f[k],B[l].f[k]); OUT(sat); break;
        case 75: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=sqrtf(A[l].f[k]); OUT(sat); break;
        case 68: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=1.0f/sqrtf(A[l].f[k]); OUT(sat); break;
        case 129: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=1.0f/A[l].f[k]; OUT(sat); break;
        case 25: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=exp2f(A[l].f[k]); OUT(sat); break;
        case 47: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=log2f(A[l].f[k]); OUT(sat); break;
        case 26: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=A[l].f[k]-floorf(A[l].f[k]); OUT(sat); break;
        case 64: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=rintf(A[l].f[k]); OUT(sat); break;
        case 65: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=floorf(A[l].f[k]); OUT(sat); break;
        case 66: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=ceilf(A[l].f[k]); OUT(sat); break;
        case 67: F1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=truncf(A[l].f[k]); OUT(sat); break;
        case 15: F2; LOOPL { float d=A[l].f[0]*B[l].f[0]+A[l].f[1]*B[l].f[1]; R[l].f[0]=R[l].f[1]=R[l].f[2]=R[l].f[3]=d; } OUT(sat); break;
        case 16: F2; LOOPL { float d=A[l].f[0]*B[l].f[0]+A[l].f[1]*B[l].f[1]+A[l].f[2]*B[l].f[2]; R[l].f[0]=R[l].f[1]=R[l].f[2]=R[l].f[3]=d; } OUT(sat); break;
        case 17: F2; LOOPL { float d=A[l].f[0]*B[l].f[0]+A[l].f[1]*B[l].f[1]+A[l].f[2]*B[l].f[2]+A[l].f[3]*B[l].f[3]; R[l].f[0]=R[l].f[1]=R[l].f[2]=R[l].f[3]=d; } OUT(sat); break;
        case 77: csg_src(G,&o[2],0,A); LOOPL for(int k=0;k<4;k++){ R[l].f[k]=sinf(A[l].f[k]); R2[l].f[k]=cosf(A[l].f[k]); } csg_dst(G,&o[0],R,sat,m); csg_dst(G,&o[1],R2,sat,m); break;
        case 54: F1; csg_dst(G,&o[0],A,sat,m); break;                                                     // mov (bits)
        case 55: csg_src(G,&o[1],1,A); csg_src(G,&o[2],0,B); csg_src(G,&o[3],0,C); LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]?B[l].u[k]:C[l].u[k]; OUT(sat); break;  // movc
        case 49: F2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].f[k]<B[l].f[k]?~0u:0; OUT(0); break;
        case 29: F2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].f[k]>=B[l].f[k]?~0u:0; OUT(0); break;
        case 24: F2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].f[k]==B[l].f[k]?~0u:0; OUT(0); break;
        case 57: F2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].f[k]!=B[l].f[k]?~0u:0; OUT(0); break;
        case 11: case 12: case 122: case 123: case 124: case 125: LOOPL memset(&R[l],0,16); OUT(sat); break;   // derivatives: 0
        case 131: U1; LOOPL for(int k=0;k<4;k++) R[l].u[k]=cs_h2f_bits(A[l].u[k]&0xffff); OUT(sat); break;   // f16tof32
        case 130: F1; LOOPL for(int k=0;k<4;k++) R[l].u[k]=cs_f2h(A[l].f[k]); OUT(0); break;                 // f32tof16
        // ---- integer ALU ----
        case 30: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]+B[l].u[k]; OUT(0); break;
        case 38: if(I->nop>3){ csg_src(G,&o[2],1,A); csg_src(G,&o[3],1,B); LOOPL for(int k=0;k<4;k++){ int64_t mm=(int64_t)A[l].i[k]*B[l].i[k]; R[l].i[k]=(int32_t)mm; R2[l].i[k]=(int32_t)(mm>>32); } csg_dst(G,&o[0],R2,0,m); csg_dst(G,&o[1],R,0,m); } break;
        case 81: if(I->nop>3){ csg_src(G,&o[2],1,A); csg_src(G,&o[3],1,B); LOOPL for(int k=0;k<4;k++){ uint64_t mm=(uint64_t)A[l].u[k]*B[l].u[k]; R[l].u[k]=(uint32_t)mm; R2[l].u[k]=(uint32_t)(mm>>32); } csg_dst(G,&o[0],R2,0,m); csg_dst(G,&o[1],R,0,m); } break;
        case 78: if(I->nop>3){ csg_src(G,&o[2],1,A); csg_src(G,&o[3],1,B); LOOPL for(int k=0;k<4;k++){ R2[l].u[k]=B[l].u[k]?A[l].u[k]/B[l].u[k]:~0u; R[l].u[k]=B[l].u[k]?A[l].u[k]%B[l].u[k]:~0u; } csg_dst(G,&o[0],R2,0,m); csg_dst(G,&o[1],R,0,m); } break;
        case 35: case 82: U3; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]*B[l].u[k]+C[l].u[k]; OUT(0); break;
        case 36: U2; LOOPL for(int k=0;k<4;k++) R[l].i[k]=A[l].i[k]>B[l].i[k]?A[l].i[k]:B[l].i[k]; OUT(0); break;
        case 37: U2; LOOPL for(int k=0;k<4;k++) R[l].i[k]=A[l].i[k]<B[l].i[k]?A[l].i[k]:B[l].i[k]; OUT(0); break;
        case 83: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]>B[l].u[k]?A[l].u[k]:B[l].u[k]; OUT(0); break;
        case 84: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]<B[l].u[k]?A[l].u[k]:B[l].u[k]; OUT(0); break;
        case 40: U1; LOOPL for(int k=0;k<4;k++) R[l].i[k]=-A[l].i[k]; OUT(0); break;
        case 41: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]<<(B[l].u[k]&31); OUT(0); break;
        case 42: U2; LOOPL for(int k=0;k<4;k++) R[l].i[k]=A[l].i[k]>>(B[l].u[k]&31); OUT(0); break;
        case 85: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]>>(B[l].u[k]&31); OUT(0); break;
        case 1: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]&B[l].u[k]; OUT(0); break;
        case 60: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]|B[l].u[k]; OUT(0); break;
        case 87: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]^B[l].u[k]; OUT(0); break;
        case 59: U1; LOOPL for(int k=0;k<4;k++) R[l].u[k]=~A[l].u[k]; OUT(0); break;
        case 32: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]==B[l].u[k]?~0u:0; OUT(0); break;
        case 39: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]!=B[l].u[k]?~0u:0; OUT(0); break;
        case 34: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].i[k]<B[l].i[k]?~0u:0; OUT(0); break;
        case 33: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].i[k]>=B[l].i[k]?~0u:0; OUT(0); break;
        case 79: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]<B[l].u[k]?~0u:0; OUT(0); break;
        case 80: U2; LOOPL for(int k=0;k<4;k++) R[l].u[k]=A[l].u[k]>=B[l].u[k]?~0u:0; OUT(0); break;
        case 43: U1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=(float)A[l].i[k]; OUT(sat); break;   // itof
        case 86: U1; LOOPL for(int k=0;k<4;k++) R[l].f[k]=(float)A[l].u[k]; OUT(sat); break;   // utof
        case 27: F1; LOOPL for(int k=0;k<4;k++){ float f=A[l].f[k]; R[l].i[k]= f!=f?0 : f>=2147483647.0f?0x7fffffff : f<=-2147483648.0f?(int32_t)0x80000000 : (int32_t)f; } OUT(0); break;   // ftoi
        case 28: F1; LOOPL for(int k=0;k<4;k++){ float f=A[l].f[k]; R[l].u[k]= (f!=f||f<=0)?0 : f>=4294967295.0f?0xffffffffu : (uint32_t)f; } OUT(0); break;  // ftou
        case 138: case 139: U3; LOOPL for(int k=0;k<4;k++){ uint32_t w=A[l].u[k]&31, of=B[l].u[k]&31, cv=C[l].u[k];   // ubfe/ibfe width, offset, src
                if(!w){ R[l].u[k]=0; continue; } if(w+of<32){ if(op==138) R[l].u[k]=(cv<<(32-w-of))>>(32-w); else R[l].i[k]=(int32_t)(cv<<(32-w-of))>>(32-w); }
                else { if(op==138) R[l].u[k]=cv>>of; else R[l].i[k]=(int32_t)cv>>of; } } OUT(0); break;
        case 140: U3; csg_src(G,&o[4],1,D); LOOPL for(int k=0;k<4;k++){ uint32_t W=A[l].u[k]&31, O=B[l].u[k]&31; uint32_t mask=(W==32?~0u:(((1u<<W)-1)<<O)); R[l].u[k]=((C[l].u[k]<<O)&mask)|(D[l].u[k]&~mask); } OUT(0); break;
        case 134: U1; LOOPL for(int k=0;k<4;k++) R[l].u[k]=(uint32_t)__builtin_popcount(A[l].u[k]); OUT(0); break;
        // ---- memory ----
        case 167: case 165: {   // ld_structured dst, idx, byteoff, t#/u#  |  ld_raw dst, byteaddr, t#/u#
            int ri=op==167?3:2; const DgOp* ro=&o[ri]; uint32_t reg=ro->nidx?ro->idx[0].val:0;
            D3DObj* res=0; uint32_t stride=4;
            if(ro->type==30/*UAV*/){ if(reg<8){ res=cs_res(G->c->uav[reg]); stride=p->uavstride[reg]; } }
            else if(ro->type==T_RESOURCE){ if(reg<32){ res=cs_res(G->c->srv[reg]); stride=p->srvstride[reg]; } }
            U1; if(op==167) csg_src(G,&o[2],1,B);
            LOOPL { uint64_t addr= op==167 ? (uint64_t)A[l].u[0]*stride+B[l].u[0] : A[l].u[0]; CsV4 raw4;
                for(int k=0;k<4;k++) raw4.u[k]= res? cs_rd_u32(res->data,res->size,addr+4u*k) : 0;
                for(int k=0;k<4;k++) R[l].u[k]=raw4.u[ro->csw[k]]; }
            OUT(0); } break;
        case 168: case 166: {   // store_structured u#.mask, idx, byteoff, src  |  store_raw u#.mask, byteaddr, src (lanes in order)
            uint32_t reg=o[0].nidx?o[0].idx[0].val:0; if(reg>=8) break; D3DObj* res=cs_res(G->c->uav[reg]); if(!res||!res->data) break;
            U1; if(op==168){ csg_src(G,&o[2],1,B); csg_src(G,&o[3],1,C); } else csg_src(G,&o[2],1,C);
            int wm=o[0].mask?o[0].mask:1;
            if(reg==0 && G->c->wlog){ if(op==168){ FORL cs_wlog(G->c,A[l].u[0]); } else G->c->wlogovf=1; }
            FORL { uint64_t addr= op==168 ? (uint64_t)A[l].u[0]*p->uavstride[reg]+B[l].u[0] : A[l].u[0]; int j=0;
                for(int k=0;k<4;k++) if(wm&(1<<k)){ uint64_t off=addr+4u*(uint64_t)j++; if(off+4<=res->size) memcpy((uint8_t*)res->data+off,&C[l].u[k],4); } }
            } break;
        case 72: case 69: case 74: {   // sample_l / sample / sample_b dst, uv, t#, s#, lod  (mip 0, bilinear)
            F1; uint32_t tr=o[2].nidx?o[2].idx[0].val:0, sr=o[3].nidx?o[3].idx[0].val:0; D3DObj* s=sr<16?G->c->samp[sr]:0;
            LOOPL { float px[4]; if(tr<32) cs_sample(G->c->tpx[tr],G->c->tw[tr],G->c->th[tr],s,A[l].f[0],A[l].f[1],px); else memset(px,0,sizeof px); CsV4 v; memcpy(&v,px,16);
                for(int k=0;k<4;k++) R[l].u[k]=v.u[o[2].csw[k]]; }
            OUT(sat); } break;
        case 45: {   // ld dst, int addr, t#
            U1; uint32_t tr=o[2].nidx?o[2].idx[0].val:0; int W=tr<32?G->c->tw[tr]:0,H=tr<32?G->c->th[tr]:0; const float* px=tr<32?G->c->tpx[tr]:0;
            LOOPL { CsV4 v; memset(&v,0,sizeof v); if(px && A[l].i[0]>=0 && A[l].i[1]>=0 && A[l].i[0]<W && A[l].i[1]<H) memcpy(&v,px+((size_t)A[l].i[1]*W+A[l].i[0])*4,16);
                for(int k=0;k<4;k++) R[l].u[k]=v.u[o[2].csw[k]]; }
            OUT(sat); } break;
        default:
            if(op>=88 && op<=106) break;       // declarations
            if(op==53||op==58||op>=143) break; // customdata, nop, SM5 dcls
            *budgetp=budget; return -op;       // unsupported instruction
        }
    }
    #undef LOOPL
    #undef F1
    #undef F2
    #undef F3
    #undef U1
    #undef U2
    #undef U3
    #undef OUT
    *budgetp=budget; return 1; }

// Run Dispatch(gx,gy,gz). Returns threads run, or <0 = -opcode of an unsupported instruction.
// Threads are independent (no group-shared memory in these kernels), so the thread range is split into
// chunks that can run in parallel with GCD (PELOAD_CS_PARALLEL; measured no gain for 64-thread dispatches,
// so serial is the default). Each chunk has its own registers.
#include <dispatch/dispatch.h>
typedef struct { CsProg* p; CsCtx* c; uint32_t gx,gy,gz; uint64_t total; uint32_t chunks; volatile long ran; volatile long bad; long tmax,tsum,tbusy; int scalar; } CsJob;
static int cs_scalar(void){ static int s=-1; if(s<0) s=getenv("PELOAD_CS_SCALAR")?1:0; return s; }
static void cs_chunk_simt(CsJob* J,uint64_t lo,uint64_t hi){
    CsProg* p=J->p; int nr=p->ntemps>0?p->ntemps:1, nx=p->xtotal>0?p->xtotal:1;
    CsV4* r=calloc((size_t)nr*CSL,sizeof(CsV4)); CsV4* x=calloc((size_t)nx*CSL,sizeof(CsV4)); CsGrp* G=calloc(1,sizeof(CsGrp)); if(!r||!x||!G){ free(r); free(x); free(G); return; }
    G->p=p; G->c=J->c; G->r=r; G->x=x;
    uint32_t tgn=p->tg[0]*p->tg[1]*p->tg[2];
    for(uint64_t base=lo; base<hi; base+=CSL){
        int n=(int)(hi-base<CSL?hi-base:CSL); G->n=n; G->mcur=0; G->na=0;
        for(int l=0;l<n;l++){ uint64_t i=base+(uint64_t)l, g=i/tgn, li=i%tgn;
            uint32_t gi=(uint32_t)(g%J->gx), gj=(uint32_t)((g/J->gx)%J->gy), gk=(uint32_t)(g/((uint64_t)J->gx*J->gy));
            uint32_t a=(uint32_t)(li%p->tg[0]), b=(uint32_t)((li/p->tg[0])%p->tg[1]), c=(uint32_t)(li/((uint64_t)p->tg[0]*p->tg[1]));
            G->gid[0][l]=gi; G->gid[1][l]=gj; G->gid[2][l]=gk; G->ltid[0][l]=a; G->ltid[1][l]=b; G->ltid[2][l]=c;
            G->tid[0][l]=gi*p->tg[0]+a; G->tid[1][l]=gj*p->tg[1]+b; G->tid[2][l]=gk*p->tg[2]+c; }
        memset(r,0,(size_t)nr*CSL*sizeof(CsV4)); memset(x,0,(size_t)nx*CSL*sizeof(CsV4));
        long budget=4000000; int rc=cs_exec_group(G,&budget); g_cs_instrs+=(4000000-budget)*(long)n;
        if(rc<0){ J->bad=rc; break; } __sync_fetch_and_add(&J->ran,n); }
    free(r); free(x); free(G); }
static void cs_chunk(void* ctx,size_t ci){
    CsJob* J=ctx; CsProg* p=J->p; uint64_t per=(J->total+J->chunks-1)/J->chunks, lo=ci*per, hi=lo+per; if(hi>J->total) hi=J->total; if(lo>=hi) return;
    if(!J->scalar){ cs_chunk_simt(J,lo,hi); return; }
    int nr=p->ntemps>0?p->ntemps:1; CsV4* r=calloc((size_t)nr,sizeof(CsV4)); CsV4* x=calloc((size_t)(p->xtotal>0?p->xtotal:1),sizeof(CsV4)); if(!r||!x){ free(r); free(x); return; }
    CsThread T; memset(&T,0,sizeof T); T.p=p; T.c=J->c; T.r=r; T.x=x;
    uint32_t tgn=p->tg[0]*p->tg[1]*p->tg[2];
    for(uint64_t i=lo;i<hi;i++){
        uint64_t g=i/tgn, l=i%tgn;
        uint32_t gi=(uint32_t)(g%J->gx), gj=(uint32_t)((g/J->gx)%J->gy), gk=(uint32_t)(g/((uint64_t)J->gx*J->gy));
        uint32_t li=(uint32_t)(l%p->tg[0]), lj=(uint32_t)((l/p->tg[0])%p->tg[1]), lk=(uint32_t)(l/((uint64_t)p->tg[0]*p->tg[1]));
        T.gid[0]=gi; T.gid[1]=gj; T.gid[2]=gk; T.ltid[0]=li; T.ltid[1]=lj; T.ltid[2]=lk;
        T.tid[0]=gi*p->tg[0]+li; T.tid[1]=gj*p->tg[1]+lj; T.tid[2]=gk*p->tg[2]+lk;
        memset(r,0,(size_t)nr*sizeof(CsV4));
        long before=g_cs_instrs;
        int rc=cs_exec_thread(&T); if(rc<0){ J->bad=rc; break; } __sync_fetch_and_add(&J->ran,1);
        { static int ts=-1; if(ts<0) ts=getenv("PELOAD_CS_THRSTAT")?1:0; if(ts){ long d=g_cs_instrs-before; if(d>J->tmax) J->tmax=d; J->tsum+=d; if(d>8) J->tbusy++; } } }
    free(r); free(x); }
static long cs_dispatch(CsProg*p,CsCtx*c,uint32_t gx,uint32_t gy,uint32_t gz){
    if(!p||!p->ok) return 0;
    CsJob J; memset(&J,0,sizeof J); J.p=p; J.c=c; J.gx=gx?gx:1; J.gy=gy?gy:1; J.gz=gz?gz:1;
    J.total=(uint64_t)J.gx*J.gy*J.gz*p->tg[0]*p->tg[1]*p->tg[2];
    for(int i=0;i<32;i++){ c->tpx[i]=0; c->tw[i]=c->th[i]=0; D3DObj* t=cs_res(c->srv[i]);
        if(t && t->kind>=2 && t->kind<=4 && t->w && p->usestex) c->tpx[i]=cs_tex_rgba(t,&c->tw[i],&c->th[i]); }
    static int serial=-1; if(serial<0) serial=getenv("PELOAD_CS_PARALLEL")?0:1;   // 64-thread dispatches: GCD fan-out costs more than it saves
    J.chunks = serial||J.total<32||c->wlog ? 1 : (uint32_t)(J.total/16 < 8 ? J.total/16 : 8);   // (the write log is single-threaded)
    J.scalar=cs_scalar()||c->trace;
    // PELOAD_CS_CHECK: run the per-thread interpreter first on a copy of every UAV, then the SIMT one, and compare
    static int check=-1; if(check<0) check=getenv("PELOAD_CS_CHECK")?1:0;
    D3DObj* ures[8]={0}; uint8_t* before[8]={0}; uint8_t* sref[8]={0}; int nu=0;
    if(check && !J.scalar){ for(int i=0;i<8;i++){ D3DObj* r=cs_res(c->uav[i]); if(!r||!r->data||!r->size) continue; int dup=0; for(int k=0;k<nu;k++) if(ures[k]==r) dup=1; if(dup) continue;
            ures[nu]=r; before[nu]=malloc(r->size); if(before[nu]) memcpy(before[nu],r->data,r->size); nu++; }
        J.scalar=1; J.chunks=1; cs_chunk(&J,0); long ran_s=J.ran, bad_s=J.bad;
        for(int k=0;k<nu;k++){ sref[k]=malloc(ures[k]->size); if(sref[k]) memcpy(sref[k],ures[k]->data,ures[k]->size); if(before[k]) memcpy(ures[k]->data,before[k],ures[k]->size); }
        J.scalar=0; J.ran=0; J.bad=0;
        cs_chunk(&J,0);
        static long nchk, nbad; nchk++; int mism=0;
        for(int k=0;k<nu;k++) if(sref[k] && memcmp(sref[k],ures[k]->data,ures[k]->size)){ mism=1;
            if(nbad<20){ uint64_t off=0; while(off<ures[k]->size && ((uint8_t*)ures[k]->data)[off]==sref[k][off]) off++;
                char m[220]; int kk=snprintf(m,sizeof m,"  [cs-check] MISMATCH prog %p (%d instrs) uav %d size %llu first diff at byte %llu (ran scalar %ld simt %ld, bad %ld/%ld)\n",
                    (void*)p,p->nins,k,(unsigned long long)ures[k]->size,(unsigned long long)off,ran_s,J.ran,bad_s,J.bad); (void)!write(2,m,kk); } }
        if(mism) nbad++;
        if(nchk%2000==0){ char m[120]; int kk=snprintf(m,sizeof m,"  [cs-check] %ld dispatches compared, %ld differed\n",nchk,nbad); (void)!write(2,m,kk); }
        for(int k=0;k<nu;k++){ free(before[k]); free(sref[k]); } }
    else if(J.chunks<=1){ J.chunks=1; cs_chunk(&J,0); }
    else dispatch_apply_f(J.chunks,dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),&J,cs_chunk);
    { static int n; if(getenv("PELOAD_CS_THRSTAT") && n++<40){ char m[200]; int k=snprintf(m,sizeof m,"  [cs-thr] prog %p nins %d threads %llu: instrs sum %ld max/thread %ld busy threads %ld\n",(void*)p,p->nins,(unsigned long long)J.total,J.tsum,J.tmax,J.tbusy); (void)!write(2,m,k); } }
    if(J.bad) return J.bad;
    return J.ran; }
