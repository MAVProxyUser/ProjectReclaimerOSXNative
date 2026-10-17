// d3dsubmit.c -- D3D11 immediate-context submission thread (PELOAD_D3DGL_SUBMIT=1).   Included by peload.c.
//
// Without it, the game's render thread runs the whole D3D11->GL translation and Apple's GL driver (both translated
// under Rosetta) inside every Draw call. With it, the game thread only RECORDS: the context calls that do work become
// commands in a single-producer/single-consumer queue, and a GL thread that owns the GL context executes them with the
// unchanged ctxi_* / d3dgl_* implementations -- so the game's own work overlaps the GL work.
//
// Ordering and data:
//   * Bound state: setters write g_rst on the game thread; each draw sends g_rst along when it changed since the last
//     one (a 2.2 KB snapshot), the GL thread copies it into g_d3dst. Compute state likewise (g_rcs -> g_cs at Dispatch).
//   * Map(WRITE_DISCARD) hands the game a fresh block (recycled per resource, never freed) and Unmap queues a COMMIT
//     that swaps it in on the GL thread, so draws queued earlier still read the old contents. NO_OVERWRITE maps return
//     the newest block and queue a "dirty" at Unmap. READ / WRITE / READ_WRITE maps wait for the queue to drain.
//   * UpdateSubresource copies the source bytes into the queue.
//   * Queries: Begin/End are queued; GetData answers occlusion results the GL thread harvested (S_FALSE until then).
//   * Present is queued; the game may run at most PELOAD_D3DGL_SUBMIT_FRAMES (default 1) frames ahead (more = staler
//     occlusion results, so more draws).
// Without PELOAD_D3DGL_SUBMIT everything runs inline exactly as before (state is copied g_rst -> g_d3dst per draw).

// The thread can be switched at run time (at a Present, once nothing is mapped): the stats page's submit control or
// PELOAD_D3DGL_SUBMIT_AB=<secs>, which alternates on/off every <secs> and logs each phase's fps -- an A/B in one game.
static int g_sub_cur=-1; static volatile int g_sub_want=-1; static uint32_t g_sub_gen=1; static int g_sub_nmapped;
static unsigned long g_sub_ndraw;
static inline int sub_on(void){ if(__builtin_expect(g_sub_cur<0,0)){ const char*e=getenv("PELOAD_D3DGL_SUBMIT"); g_sub_cur=e&&*e&&strcmp(e,"0")?1:0;
        if(getenv("PELOAD_D3DGL_SUBMIT_AB") || (getenv("PELOAD_AB") && !strncmp(getenv("PELOAD_AB"),"submit",6))) g_sub_cur=0; } return g_sub_cur; }

// ---- the queue: variable-size commands in a ring --------------------------------------------------------------------
enum { SQ_STATE=1, SQ_CSSTATE, SQ_DRAW, SQ_CLEAR_RTV, SQ_CLEAR_DSV, SQ_UPDSUB, SQ_COPYRES, SQ_COPYSUB, SQ_COMMIT, SQ_DIRTY,
       SQ_DISPATCH, SQ_CLEAR_UAV, SQ_BEGIN, SQ_END, SQ_PRESENT, SQ_FENCE, SQ_WRAP };
typedef struct { uint32_t type, size; } SqHdr;   // size = header + payload, 16-byte aligned
enum { SQ_BYTES=64<<20 };
static uint8_t* g_sq;
static volatile uint64_t g_sq_head, g_sq_tail;       // byte positions (monotonic): producer writes head, consumer tail
static volatile uint64_t g_sq_fence_req, g_sq_fence_done, g_sq_pres_req, g_sq_pres_done;
static pthread_mutex_t g_sq_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sq_cv=PTHREAD_COND_INITIALIZER, g_sq_pcv=PTHREAD_COND_INITIALIZER;
static volatile int g_sq_csleep, g_sq_psleep;        // consumer / producer is waiting on its condvar
static unsigned long g_sq_ncmd, g_sq_nstate, g_sq_waitfull, g_sq_waitframe, g_sq_drains; static double g_sq_waitms; double g_sq_idle_s;

static void* sub_thread(void*a);
static void sub_wait(pthread_cond_t*cv){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_nsec+=2000000; if(ts.tv_nsec>=1000000000){ ts.tv_sec++; ts.tv_nsec-=1000000000; }
    pthread_cond_timedwait(cv,&g_sq_mx,&ts); }   // (g_sq_mx held) at most 2 ms, then the caller re-checks
static void sub_start(void){ static int started; if(started) return; started=1;
    g_sq=mmap(0,SQ_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at,QOS_CLASS_USER_INTERACTIVE,0);
    pthread_attr_setstacksize(&at,8<<20); pthread_create(&th,&at,sub_thread,0);
    (void)!write(2,"  [submit] GL submission thread started (PELOAD_D3DGL_SUBMIT)\n",63); }
static void sub_kick(void){ __atomic_thread_fence(__ATOMIC_SEQ_CST); if(__atomic_load_n(&g_sq_csleep,__ATOMIC_SEQ_CST)){ pthread_mutex_lock(&g_sq_mx); pthread_cond_signal(&g_sq_cv); pthread_mutex_unlock(&g_sq_mx); } }
// reserve n payload bytes; returns the payload pointer (the command is published by sub_push)
static uint8_t* sub_alloc(uint32_t type,uint32_t n){
    uint32_t sz=(uint32_t)((sizeof(SqHdr)+n+15)&~15u);
    for(;;){ uint64_t h=g_sq_head, t=__atomic_load_n(&g_sq_tail,__ATOMIC_ACQUIRE); uint64_t off=h%SQ_BYTES;
        if(off+sz>SQ_BYTES){   // no room before the end: a WRAP marker fills the rest
            uint64_t need=(SQ_BYTES-off)+sz; if(h+need-t<=SQ_BYTES){ SqHdr*w=(SqHdr*)(g_sq+off); w->type=SQ_WRAP; w->size=(uint32_t)(SQ_BYTES-off);
                __atomic_store_n(&g_sq_head,h+(SQ_BYTES-off),__ATOMIC_RELEASE); continue; } }
        else if(h+sz-t<=SQ_BYTES){ SqHdr*c=(SqHdr*)(g_sq+off); c->type=type; c->size=sz; return (uint8_t*)(c+1); }
        // full: wait for the GL thread
        double t0=mono_s(); g_sq_waitfull++; sub_kick();
        pthread_mutex_lock(&g_sq_mx); __atomic_store_n(&g_sq_psleep,1,__ATOMIC_SEQ_CST); while(g_sq_head-__atomic_load_n(&g_sq_tail,__ATOMIC_ACQUIRE)+sz>SQ_BYTES) sub_wait(&g_sq_pcv); g_sq_psleep=0; pthread_mutex_unlock(&g_sq_mx);
        g_sq_waitms+=(mono_s()-t0)*1000; } }
static void sub_push(void){ uint64_t h=g_sq_head; SqHdr*c=(SqHdr*)(g_sq+h%SQ_BYTES); __atomic_store_n(&g_sq_head,h+c->size,__ATOMIC_RELEASE); g_sq_ncmd++; sub_kick(); }
// wait until the GL thread has executed everything queued so far
static void sub_drain(void){ if(!g_sq) return; uint64_t want=++g_sq_fence_req; uint64_t*p=(uint64_t*)sub_alloc(SQ_FENCE,8); *p=want; sub_push(); g_sq_drains++;
    double t0=mono_s(); pthread_mutex_lock(&g_sq_mx); __atomic_store_n(&g_sq_psleep,1,__ATOMIC_SEQ_CST); while(__atomic_load_n(&g_sq_fence_done,__ATOMIC_ACQUIRE)<want) sub_wait(&g_sq_pcv); g_sq_psleep=0; pthread_mutex_unlock(&g_sq_mx);
    g_sq_waitms+=(mono_s()-t0)*1000; }

// ---- payloads -------------------------------------------------------------------------------------------------------
typedef struct { uint32_t vcount, instcount, start, sinst; int32_t base; int indexed; } SqDraw;
typedef struct { void*view; float col[4]; int hascol; } SqClearRtv;
typedef struct { void*view; uint32_t flags; float depth; uint8_t stencil; } SqClearDsv;
typedef struct { void*res; uint32_t sub, srp, sdp; int hasbox; uint32_t box[6]; uint8_t*data; } SqUpd;
typedef struct { void*dst, *src; } SqCopy;
typedef struct { void*dst; uint32_t dsub, x, y, z; void*src; uint32_t ssub; int hasbox; uint32_t box[6]; } SqCopySub;
typedef struct { D3DObj*res; uint32_t sub; void*block; } SqCommit;
typedef struct { uint32_t a, b, c; } SqDispatch;
typedef struct { void*view; uint32_t val; } SqClearUav;

// ---- game-thread side: state hand-over and draws --------------------------------------------------------------------
static D3DState g_sq_sent; static int g_sq_sent_ok;   // the last state snapshot queued
static CsState g_sq_cssent; static int g_sq_cssent_ok;
static void sub_draw(uint32_t vcount,int indexed,uint32_t instcount,uint32_t start,int32_t base,uint32_t sinst){
    g_sub_ndraw++;
    if(!sub_on()){ if(memcmp(&g_d3dst,&g_rst,sizeof g_rst)) g_d3dst=g_rst; d3dgl_draw(vcount,indexed,instcount,start,base,sinst); return; }
    sub_start();
    if(!g_sq_sent_ok || memcmp(&g_sq_sent,&g_rst,sizeof g_rst)){ D3DState*p=(D3DState*)sub_alloc(SQ_STATE,sizeof(D3DState)); *p=g_rst; sub_push(); g_sq_sent=g_rst; g_sq_sent_ok=1; g_sq_nstate++; }
    SqDraw*d=(SqDraw*)sub_alloc(SQ_DRAW,sizeof(SqDraw)); d->vcount=vcount; d->indexed=indexed; d->instcount=instcount; d->start=start; d->base=base; d->sinst=sinst; sub_push(); }
static void sub_cs_state(void){ if(!g_sq_cssent_ok || memcmp(&g_sq_cssent,&g_rcs,sizeof g_rcs)){ CsState*p=(CsState*)sub_alloc(SQ_CSSTATE,sizeof(CsState)); *p=g_rcs; sub_push(); g_sq_cssent=g_rcs; g_sq_cssent_ok=1; } }

// ---- Map / Unmap with block renaming ----------------------------------------------------------------------------------
static void* sub_block_get(D3DObj*o){   // a recycled block of o->size bytes, else a new one
    for(;;){ void*b=__atomic_load_n(&o->rfree,__ATOMIC_ACQUIRE); if(!b) break; void*next=*(void**)b;
        if(__atomic_compare_exchange_n(&o->rfree,&b,next,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return b; }
    return malloc((size_t)(o->size<16?16:o->size)); }
static void sub_block_put(D3DObj*o,void*b){ for(;;){ void*h=__atomic_load_n(&o->rfree,__ATOMIC_ACQUIRE); *(void**)b=h;
        if(__atomic_compare_exchange_n(&o->rfree,&h,b,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return; } }
// D3D11_MAPPED_SUBRESOURCE for `sub` of o inside block `base` (the pitches ctxi_Map reports)
static void sub_fill_mapped(D3DObj*o,uint32_t sub,uint8_t*base,void*pM){
    void*d=base; uint32_t rp=o->rowpitch?o->rowpitch:(uint32_t)o->size;
    uint32_t dp=(uint32_t)(o->rowpitch?(uint64_t)o->rowpitch*(o->h?o->h:1):o->size); if(dp>o->size) dp=(uint32_t)o->size;
    if(o->kind>=2 && o->kind<=4 && o->w){ uint64_t sz; uint32_t lrp,rows,mw,mh,md; uint64_t off=tex_sub(o,sub,&sz,&lrp,&rows,&mw,&mh,&md); if(off!=UINT64_MAX){ d=base+off; rp=lrp; dp=lrp*rows; } }
    *(void**)pM=d; *(uint32_t*)((char*)pM+8)=rp; *(uint32_t*)((char*)pM+12)=dp; }
static void sub_note_drain(const char*why,D3DObj*o,uint32_t type,uint32_t fl){   // PELOAD_D3DGL_PROF: what forces drains
    if(!prof_on()) return; static int n; if(n++>=40) return; char m[220];
    int k=snprintf(m,sizeof m,"  [submit] drain #%d: %s, map type %u flags 0x%x, res %p kind %d size %llu %ux%u fmt %u stkind %d\n",n,why,type,fl,(void*)o,o?o->kind:-1,
        (unsigned long long)(o?o->size:0),o?o->w:0,o?o->h:0,o?o->fmt:0,o?o->stkind:-1); (void)!write(2,m,k); }
static int MSABI ctx_Map(void*t,void*res,uint32_t sub,uint32_t type,uint32_t fl,void*pM){
    D3DObj*o=res;
    // READ map with DO_NOT_WAIT (D3D11_MAP_FLAG_DO_NOT_WAIT, a staging readback polled every frame): no drain -- "still
    // drawing" while a queued copy into it hasn't run, else map it now (PELOAD_D3DGL_SUBMIT_DRAINREAD = always drain)
    if(sub_on() && g_sq && o && o->data && o->size && type==1 && (fl&0x100000) && !GENV1("PELOAD_D3DGL_SUBMIT_DRAINREAD")){
        if(__atomic_load_n(&g_sq_tail,__ATOMIC_ACQUIRE)<o->sq_wpend) return (int)0x887A000A;   // DXGI_ERROR_WAS_STILL_DRAWING
        return ctxi_Map(t,res,sub,type,fl,pM); }
    if(sub_on() && g_sq && (!o || !o->data || !o->size)) sub_note_drain("no CPU data",o,type,fl);
    if(!sub_on() || !o || !o->data || !o->size) { if(sub_on() && g_sq) sub_drain(); return ctxi_Map(t,res,sub,type,fl,pM); }
    sub_start(); g_fs.maps++;
    if(!o->rdata || o->rgen!=g_sub_gen){ o->rdata=o->data; o->rgen=g_sub_gen; }
    if(type==4){   // WRITE_DISCARD: a fresh block, swapped in on the GL thread at Unmap
        void*b=sub_block_get(o); if(!b){ sub_drain(); return ctxi_Map(t,res,sub,type,fl,pM); }
        o->rdata=b; o->rpend=b; if(!o->rmapped) g_sub_nmapped++; o->rmapped=4; if(pM) sub_fill_mapped(o,sub,b,pM);
        if(o->is_cb && o->size>=32 && o->size<=96){ extern D3DObj* g_last_smallcb; g_last_smallcb=o; }
        return HR_OK; }
    if(type==5){   // NO_OVERWRITE: write into the newest block (only regions no queued draw uses)
        if(!o->rmapped) g_sub_nmapped++; o->rmapped=5; if(pM) sub_fill_mapped(o,sub,o->rdata,pM); return HR_OK; }
    // WRITE (keep the contents): a fresh block holding the newest contents, committed at Unmap like WRITE_DISCARD -- when
    // nothing queued still writes the resource (a copy/update into it, or compute). PELOAD_D3DGL_SUBMIT_DRAINWRITE = drain.
    if(type==2 && o->size<=(8u<<20) && !o->cs_pending && __atomic_load_n(&g_sq_tail,__ATOMIC_ACQUIRE)>=o->sq_wpend && !GENV1("PELOAD_D3DGL_SUBMIT_DRAINWRITE")){
        void*b=sub_block_get(o); if(b){ memcpy(b,o->rdata,(size_t)o->size); o->rdata=b; o->rpend=b; if(!o->rmapped) g_sub_nmapped++; o->rmapped=4; if(pM) sub_fill_mapped(o,sub,b,pM); return HR_OK; } }
    // READ / READ_WRITE (and WRITE otherwise): everything queued must have happened first
    sub_note_drain(type==2?"write map (queued writes pending)":"read map",o,type,fl);
    sub_drain(); o->rdata=o->data; if(o->rmapped) g_sub_nmapped--; o->rmapped=0; return ctxi_Map(t,res,sub,type,fl,pM); }
static void MSABI ctx_Unmap(void*t,void*res,uint32_t sub){ (void)t; D3DObj*o=res; if(!sub_on() || !o || !g_sq) return;
    if(o->rmapped==4 && o->rpend){ SqCommit*c=(SqCommit*)sub_alloc(SQ_COMMIT,sizeof(SqCommit)); c->res=o; c->sub=sub; c->block=o->rpend; sub_push(); o->rpend=0; }
    else if(o->rmapped==5){ SqCommit*c=(SqCommit*)sub_alloc(SQ_DIRTY,sizeof(SqCommit)); c->res=o; c->sub=sub; c->block=0; sub_push(); }
    if(o->rmapped) g_sub_nmapped--; o->rmapped=0; }

// ---- other context calls ------------------------------------------------------------------------------------------------
static uint64_t sub_upd_bytes(D3DObj*o,uint32_t sub,const uint32_t*b,uint32_t srp,uint32_t sdp){   // source bytes UpdateSubresource reads
    if(o->kind==1) return b?(uint64_t)(b[3]-b[0]):o->size;
    uint64_t lsz; uint32_t rp,rows,mw,mh,md; if(tex_sub(o,sub,&lsz,&rp,&rows,&mw,&mh,&md)==UINT64_MAX) return 0;
    int bc=is_bc(o->fmt); uint32_t bpe=bc?bc_block(o->fmt):dxgi_bpp(o->fmt), q=bc?4:1;
    uint32_t l=b?b[0]:0, tp=b?b[1]:0, fr=b?b[2]:0, r=b?b[3]:mw, bt=b?b[4]:mh, bk=b?b[5]:md;
    if(r>mw)r=mw; if(bt>mh)bt=mh; if(bk>md)bk=md; if(r<=l||bt<=tp||bk<=fr) return 0;
    uint32_t c0=l/q, c1=(r+q-1)/q, r0=tp/q, r1=(bt+q-1)/q; uint64_t rowbytes=(uint64_t)(c1-c0)*bpe, sr=srp?srp:rowbytes;
    uint64_t sd=sdp?sdp:sr*(r1-r0); return (uint64_t)(bk-fr-1)*sd+(uint64_t)(r1-r0-1)*sr+rowbytes; }
static void MSABI ctx_UpdateSubresource(void*t,void*res,uint32_t sub,const void*box,const void*src,uint32_t srp,uint32_t sdp){
    D3DObj*o=res; if(!sub_on() || !o || !o->data || !src){ ctxi_UpdateSubresource(t,res,sub,box,src,srp,sdp); return; }
    sub_start(); uint64_t n=sub_upd_bytes(o,sub,box,srp,sdp); if(!n || n>(256u<<20)) return;
    uint8_t*copy=malloc((size_t)n); if(!copy){ sub_drain(); ctxi_UpdateSubresource(t,res,sub,box,src,srp,sdp); return; } memcpy(copy,src,(size_t)n);
    SqUpd*u=(SqUpd*)sub_alloc(SQ_UPDSUB,sizeof(SqUpd)); u->res=res; u->sub=sub; u->srp=srp; u->sdp=sdp; u->hasbox=box!=0; if(box) memcpy(u->box,box,24); u->data=copy; sub_push(); o->sq_wpend=g_sq_head; }
static void MSABI ctx_CopyResource(void*t,void*dst,void*src){ if(!sub_on()){ ctxi_CopyResource(t,dst,src); return; } sub_start();
    SqCopy*c=(SqCopy*)sub_alloc(SQ_COPYRES,sizeof(SqCopy)); c->dst=dst; c->src=src; sub_push(); if(dst) ((D3DObj*)dst)->sq_wpend=g_sq_head; }
static void MSABI ctx_CopySubresourceRegion(void*t,void*dstR,uint32_t dstSub,uint32_t dstX,uint32_t dstY,uint32_t dstZ,void*srcR,uint32_t srcSub,const void*srcBox){
    if(!sub_on()){ ctxi_CopySubresourceRegion(t,dstR,dstSub,dstX,dstY,dstZ,srcR,srcSub,srcBox); return; } sub_start();
    SqCopySub*c=(SqCopySub*)sub_alloc(SQ_COPYSUB,sizeof(SqCopySub)); c->dst=dstR; c->dsub=dstSub; c->x=dstX; c->y=dstY; c->z=dstZ; c->src=srcR; c->ssub=srcSub; c->hasbox=srcBox!=0; if(srcBox) memcpy(c->box,srcBox,24); sub_push(); if(dstR) ((D3DObj*)dstR)->sq_wpend=g_sq_head; }
static void MSABI ctx_ClearRTV(void*t,void*rtv,const float*col){ if(!sub_on()){ ctxi_ClearRTV(t,rtv,col); return; } sub_start();
    SqClearRtv*c=(SqClearRtv*)sub_alloc(SQ_CLEAR_RTV,sizeof(SqClearRtv)); c->view=rtv; c->hascol=col!=0; if(col) memcpy(c->col,col,16); sub_push(); }
static void MSABI ctx_ClearDSV(void*t,void*dsv,uint32_t fl,float d,uint8_t s){ if(!sub_on()){ ctxi_ClearDSV(t,dsv,fl,d,s); return; } sub_start();
    SqClearDsv*c=(SqClearDsv*)sub_alloc(SQ_CLEAR_DSV,sizeof(SqClearDsv)); c->view=dsv; c->flags=fl; c->depth=d; c->stencil=s; sub_push(); }
static void MSABI ctx_Dispatch(void*t,uint32_t a,uint32_t b,uint32_t c){
    if(!sub_on()){ g_cs=g_rcs; ctxi_Dispatch(t,a,b,c); return; } sub_start(); sub_cs_state();
    SqDispatch*d=(SqDispatch*)sub_alloc(SQ_DISPATCH,sizeof(SqDispatch)); d->a=a; d->b=b; d->c=c; sub_push(); }
static void MSABI ctx_ClearUAVUint(void*t,void*uav,const uint32_t*vals){ if(!sub_on()){ ctxi_ClearUAVUint(t,uav,vals); return; } sub_start();
    SqClearUav*c=(SqClearUav*)sub_alloc(SQ_CLEAR_UAV,sizeof(SqClearUav)); c->view=uav; c->val=vals?vals[0]:0; sub_push(); }
static void MSABI ctx_ClearUAVFloat(void*t,void*uav,const float*vals){ if(!sub_on()){ ctxi_ClearUAVFloat(t,uav,vals); return; } sub_start();
    SqClearUav*c=(SqClearUav*)sub_alloc(SQ_CLEAR_UAV,sizeof(SqClearUav)); c->view=uav; c->val=0; if(vals) memcpy(&c->val,vals,4); sub_push(); }
// queries: occlusion results come back through q_res/q_ready, filled by the GL thread
static void MSABI ctx_Begin(void*t,void*a){ D3DObj*o=a; if(!sub_on() || !o || o->stkind!=5){ ctxi_Begin(t,a); return; } sub_start();
    o->qstate=1; o->rvalid=0; void**p=(void**)sub_alloc(SQ_BEGIN,8); *p=a; sub_push(); }
static void MSABI ctx_End(void*t,void*a){ D3DObj*o=a; if(!sub_on() || !o || o->stkind!=5){ ctxi_End(t,a); return; } sub_start();
    if(o->sd[0]==2) o->qval=(uint64_t)(mono_s()*1e9);   // TIMESTAMP: CPU time, as ctxi_End
    o->qstate=2; void**p=(void**)sub_alloc(SQ_END,8); *p=a; sub_push(); }
static int MSABI ctx_GetData(void*t,void*a,void*data,uint32_t size,uint32_t flags){ D3DObj*o=a;
    if(!sub_on() || !o || o->stkind!=5 || !(o->sd[0]==1||o->sd[0]==5)) return ctxi_GetData(t,a,data,size,flags);
    if(!__atomic_load_n(&o->rvalid,__ATOMIC_ACQUIRE)) return 1;   // S_FALSE: the GL thread hasn't got the result yet
    uint64_t r=o->qval; if(data&&size){ if(o->sd[0]==1){ memcpy(data,&r,size<8?size:8); } else { uint32_t b=r?1:0; memcpy(data,&b,size<4?size:4); } }
    return 0; }
// Present: queued; the game may run at most N frames ahead of the GL thread
static void sub_switch(void){   // game thread, at a Present
    static double ab_s=-1, t0; static unsigned long f0, d0; static double sum[2]; static unsigned long fr[2], dr[2]; static int nph[2];
    // PELOAD_AB=<feature>:<secs> (or PELOAD_D3DGL_SUBMIT_AB=<secs> = submit): flip a feature every <secs> in one game and
    // log each phase's fps and draws/frame. Features: submit (this thread), memcpy (big guest copies on the host).
    static int feat; extern uint64_t g_memcpy_min; static uint64_t mc_on;
    if(ab_s<0){ const char*e=getenv("PELOAD_D3DGL_SUBMIT_AB"); ab_s=e?atof(e):0; feat=0;
        const char*a=getenv("PELOAD_AB"); if(a){ const char*c=strchr(a,':'); ab_s=c?atof(c+1):20; feat=!strncmp(a,"memcpy",6)?1:!strncmp(a,"glcache",7)?2:!strncmp(a,"cbbatch",7)?3:0; if(!ab_s) ab_s=20; }
        mc_on=g_memcpy_min; t0=mono_s(); f0=g_ct_frame; d0=g_sub_ndraw; }
    if(ab_s>0){ double now=mono_s(); if(now-t0>=ab_s && g_sub_want<0){
            extern volatile int g_glc_on; extern volatile unsigned g_glc_gen; extern volatile int g_cbbatch_on;
            int ph=feat==1?(g_memcpy_min!=~0ull):feat==2?g_glc_on:feat==3?(g_cbbatch_on!=0):sub_on(); unsigned long fn=g_ct_frame-f0, dn=g_sub_ndraw-d0; double dt=now-t0;
            sum[ph]+=dt; fr[ph]+=fn; dr[ph]+=dn; nph[ph]++;
            char m[300]; int k=snprintf(m,sizeof m,"  [ab] %s %s: %.1f fps, %lu draws/frame (%.0f s) | totals: on %.1f fps (%d), off %.1f fps (%d)\n",
                feat==1?"memcpy":feat==2?"glcache":feat==3?"cbbatch":"submit",ph?"on ":"off",fn/dt,fn?dn/fn:0,dt,sum[1]>0?fr[1]/sum[1]:0,nph[1],sum[0]>0?fr[0]/sum[0]:0,nph[0]); (void)!write(2,m,k);
            if(feat==1) g_memcpy_min=ph?~0ull:mc_on; else if(feat==2){ if(!ph) g_glc_gen++; g_glc_on=!ph; } else if(feat==3) g_cbbatch_on=!ph; else g_sub_want=!ph;
            t0=now; f0=g_ct_frame; d0=g_sub_ndraw; } }
    int w=g_sub_want; if(w<0) return; if(w==sub_on()){ g_sub_want=-1; return; } if(g_sub_nmapped>0) return;
    if(sub_on()) sub_drain();                         // off: everything queued happens first; the GL thread then idles
    else { g_sq_sent_ok=0; g_sq_cssent_ok=0; }        // on: send full state with the first draw / dispatch
    g_sub_gen++; g_sub_cur=w; g_sub_want=-1;
    if(!getenv("PELOAD_D3DGL_SUBMIT_AB") && !getenv("PELOAD_AB")){ char m[80]; int k=snprintf(m,sizeof m,"  [submit] GL submission thread switched %s\n",w?"on":"off"); (void)!write(2,m,k); } }
static void sub_present(void){
    sub_switch();
    if(!sub_on()){ d3dgl_present(); return; } sub_start();
    (void)sub_alloc(SQ_PRESENT,0); sub_push(); uint64_t want=++g_sq_pres_req;
    static int ahead=-1; if(ahead<0){ const char*e=getenv("PELOAD_D3DGL_SUBMIT_FRAMES"); ahead=e?atoi(e):1; if(ahead<1) ahead=1; if(ahead>4) ahead=4; }
    if(want>(uint64_t)ahead && __atomic_load_n(&g_sq_pres_done,__ATOMIC_ACQUIRE)<want-(uint64_t)ahead){ double t0=mono_s(); g_sq_waitframe++;
        pthread_mutex_lock(&g_sq_mx); __atomic_store_n(&g_sq_psleep,1,__ATOMIC_SEQ_CST); while(__atomic_load_n(&g_sq_pres_done,__ATOMIC_ACQUIRE)<want-(uint64_t)ahead) sub_wait(&g_sq_pcv); g_sq_psleep=0; pthread_mutex_unlock(&g_sq_mx);
        g_sq_waitms+=(mono_s()-t0)*1000; }
    if(prof_on()){ static double tl; double now=mono_s(); if(now-tl>=2.0){ tl=now; char m[400];
        extern double g_glwait_s, g_cswait_s; static double lgl, lcs, lidle; extern double g_sq_idle_s; extern uint64_t g_cs_snapbytes; static uint64_t lsnap;
        int k=snprintf(m,sizeof m,"  [submit] %lu commands (%lu state snapshots), game thread waited %.0f ms: queue full %lu, frames ahead %lu, drains %lu | GL thread waited: for work %.0f ms, for compute %.0f ms, for the GL lock %.0f ms | compute inputs copied %.1f MB\n",
            g_sq_ncmd,g_sq_nstate,g_sq_waitms,g_sq_waitfull,g_sq_waitframe,g_sq_drains,(g_sq_idle_s-lidle)*1e3,(g_cswait_s-lcs)*1e3,(g_glwait_s-lgl)*1e3,(g_cs_snapbytes-lsnap)/1e6); (void)!write(2,m,k); lsnap=g_cs_snapbytes;
        lidle=g_sq_idle_s; lcs=g_cswait_s; lgl=g_glwait_s;
        g_sq_ncmd=g_sq_nstate=g_sq_waitfull=g_sq_waitframe=g_sq_drains=0; g_sq_waitms=0; } } }

// ---- the GL thread ------------------------------------------------------------------------------------------------------
static D3DObj* g_sq_qpend[256]; static int g_sq_nqpend;   // ended occlusion queries whose results aren't back yet
static void sub_harvest_queries(void){
    if(!g_sq_nqpend || !G.GetQueryObjectiv) return; GLLOCK(); if(CGLGetCurrentContext()!=g_d3dgl_cgl) CGLSetCurrentContext(g_d3dgl_cgl);
    for(int i=0;i<g_sq_nqpend;){ D3DObj*o=g_sq_qpend[i]; int av=0; if(o->glq) G.GetQueryObjectiv(o->glq,0x8867/*RESULT_AVAILABLE*/,&av); else av=1;
        if(av){ uint64_t r=0; if(o->glq){ if(G.GetQueryObjectui64v) G.GetQueryObjectui64v(o->glq,0x8866,&r); else { int ri=0; G.GetQueryObjectiv(o->glq,0x8866,&ri); r=(uint32_t)ri; } }
            o->qval=r; __atomic_store_n(&o->rvalid,1,__ATOMIC_RELEASE); g_sq_qpend[i]=g_sq_qpend[--g_sq_nqpend]; }
        else i++; }
    GLUNLOCK(); }
static void sub_exec(uint32_t type,uint8_t*p){
    switch(type){
    case SQ_STATE: g_d3dst=*(D3DState*)p; break;
    case SQ_CSSTATE: g_cs=*(CsState*)p; break;
    case SQ_DRAW: { SqDraw*d=(SqDraw*)p; d3dgl_draw(d->vcount,d->indexed,d->instcount,d->start,d->base,d->sinst); break; }
    case SQ_CLEAR_RTV: { SqClearRtv*c=(SqClearRtv*)p; ctxi_ClearRTV(0,c->view,c->hascol?c->col:0); break; }
    case SQ_CLEAR_DSV: { SqClearDsv*c=(SqClearDsv*)p; ctxi_ClearDSV(0,c->view,c->flags,c->depth,c->stencil); break; }
    case SQ_UPDSUB: { SqUpd*u=(SqUpd*)p; ctxi_UpdateSubresource(0,u->res,u->sub,u->hasbox?u->box:0,u->data,u->srp,u->sdp); free(u->data); break; }
    case SQ_COPYRES: { SqCopy*c=(SqCopy*)p; ctxi_CopyResource(0,c->dst,c->src); break; }
    case SQ_COPYSUB: { SqCopySub*c=(SqCopySub*)p; ctxi_CopySubresourceRegion(0,c->dst,c->dsub,c->x,c->y,c->z,c->src,c->ssub,c->hasbox?c->box:0); break; }
    case SQ_COMMIT: { SqCommit*c=(SqCommit*)p; D3DObj*o=c->res; cs_wait_res(o); void*old=o->data; o->data=c->block; if(old && old!=c->block) sub_block_put(o,old);
        d3d_dirty_sub(o,c->sub); o->rvalid=0; o->ver++; if(o->is_cb) o->cb_upd=g_cb_tick; break; }
    case SQ_DIRTY: { SqCommit*c=(SqCommit*)p; D3DObj*o=c->res; d3d_dirty_sub(o,c->sub); o->rvalid=0; o->ver++; if(o->is_cb) o->cb_upd=g_cb_tick; break; }
    case SQ_DISPATCH: { SqDispatch*d=(SqDispatch*)p; ctxi_Dispatch(0,d->a,d->b,d->c); break; }
    case SQ_CLEAR_UAV: { SqClearUav*c=(SqClearUav*)p; cs_clear_uav(c->view,c->val); break; }
    case SQ_BEGIN: { D3DObj*o=*(D3DObj**)p; uint32_t keep=o->qstate; ctxi_Begin(0,o); o->qstate=keep; break; }
    case SQ_END: { D3DObj*o=*(D3DObj**)p; uint32_t keep=o->qstate; uint64_t qv=o->qval; ctxi_End(0,o); o->qstate=keep;
        if(o->sd[0]==1||o->sd[0]==5){ o->qval=qv; if(g_sq_nqpend<256) g_sq_qpend[g_sq_nqpend++]=o; else __atomic_store_n(&o->rvalid,1,__ATOMIC_RELEASE); } break; }
    case SQ_PRESENT: d3dgl_present(); g_hb_taken=0; sub_harvest_queries(); __atomic_add_fetch(&g_sq_pres_done,1,__ATOMIC_ACQ_REL); break;
    case SQ_FENCE: __atomic_store_n(&g_sq_fence_done,*(uint64_t*)p,__ATOMIC_RELEASE); break;
    } }
static void* sub_thread(void*a){ (void)a; pthread_setname_np("shim-gl-submit");
    for(;;){
        uint64_t t=g_sq_tail, h=__atomic_load_n(&g_sq_head,__ATOMIC_ACQUIRE);
        if(t==h){
            // caught up with the game thread: poll for a while before sleeping -- a condvar wake per command costs
            // tens of microseconds and the game thread pushes thousands of commands a frame
            { static int spin_us=-1; if(spin_us<0){ const char*e=getenv("PELOAD_D3DGL_SUBMIT_SPIN"); spin_us=e?atoi(e):500; }
              uint64_t t0=mach_absolute_time(), lim=mt_us((uint64_t)spin_us);
              while(g_sq_tail==__atomic_load_n(&g_sq_head,__ATOMIC_ACQUIRE) && mach_absolute_time()-t0<lim) __asm__ volatile("pause");
              if(g_sq_tail!=__atomic_load_n(&g_sq_head,__ATOMIC_ACQUIRE)) continue; }
            sub_harvest_queries(); double w0=mono_s();
            pthread_mutex_lock(&g_sq_mx); __atomic_store_n(&g_sq_csleep,1,__ATOMIC_SEQ_CST);
            while(g_sq_tail==__atomic_load_n(&g_sq_head,__ATOMIC_ACQUIRE)) sub_wait(&g_sq_cv);
            g_sq_csleep=0; pthread_mutex_unlock(&g_sq_mx); g_sq_idle_s+=mono_s()-w0; continue; }
        SqHdr*c=(SqHdr*)(g_sq+t%SQ_BYTES); uint32_t type=c->type, sz=c->size;
        if(type!=SQ_WRAP) sub_exec(type,(uint8_t*)(c+1));
        // occlusion results as soon as the GPU has them (the game polls GetData; "not ready" makes it draw the object)
        { static unsigned hn; if(g_sq_nqpend && (++hn&127)==0) sub_harvest_queries(); }
        __atomic_store_n(&g_sq_tail,t+sz,__ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if(type==SQ_FENCE || type==SQ_PRESENT || __atomic_load_n(&g_sq_psleep,__ATOMIC_SEQ_CST)){ pthread_mutex_lock(&g_sq_mx); pthread_cond_broadcast(&g_sq_pcv); pthread_mutex_unlock(&g_sq_mx); } }
    return 0; }
