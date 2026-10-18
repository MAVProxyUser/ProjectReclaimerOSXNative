// xaudio.c — XAudio2 2.9 + X3DAudio implemented on CoreAudio (AudioQueue). Included by peload.c.
// The engine creates source voices (PCM / IEEE float), submits buffers, and refills them from the
// voice callbacks (OnBufferEnd / OnVoiceProcessingPassStart). A mixer running on a guest-capable thread
// (shim CreateThread -> has a TEB, so it may call the game's MS-ABI callbacks) renders 10 ms blocks:
// source -> (resample by rate*ratio, volume, output matrix) -> submix/master -> stereo ring -> AudioQueue.
// Effects (XAPO reverb etc.) and filters are accepted and ignored. Opt in/out: PELOAD_AUDIO.
#include <AudioToolbox/AudioToolbox.h>

#define XA_RATE 48000
#define XA_BLOCK 480                       // 10 ms mixing block
#define XA_MAXCH 8
typedef struct { uint32_t flags, bytes; const uint8_t* data; uint32_t pbeg, plen, lbeg, llen, lcount; void* ctx; } XABuf;
typedef struct XAVoice XAVoice;
struct XAVoice {
    void** vtbl;                           // COM vtable (first member)
    int kind;                              // 0 source, 1 submix, 2 master
    int fmt;                               // source sample format: 1 = int PCM, 3 = float, 0 = unsupported (silent)
    int ch, rate, bits, align;             // source format / submix+master input
    int stage;                             // submix processing stage
    float vol, chvol[XA_MAXCH];
    XAVoice* dst[4]; float mat[4][XA_MAXCH*XA_MAXCH]; int matset[4]; int ndst; int dstdefault;
    float ratio;
    void** cb;                             // IXAudio2VoiceCallback* (guest)
    XABuf q[64]; int qh, qn;
    uint64_t pos; double frac; uint64_t played; int startcb;
    int started, alive;
    float acc[XA_MAXCH*XA_BLOCK];          // submix/master accumulation
};
static XAVoice* g_xav[1024]; static int g_nxav; static XAVoice* g_xamaster;
static pthread_mutex_t g_xamx=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void** g_xaengcb[8]; static int g_nxaengcb;
static int g_xa_trace=-1; static int xa_trace(void){ if(g_xa_trace<0) g_xa_trace=getenv("PELOAD_TRACE_AUDIO")?1:0; return g_xa_trace; }

// guest callback invocation (MS ABI). Callbacks are QUEUED while the mixer holds g_xamx and invoked after it
// is released: a callback that waits on a game lock while a game thread holding that lock calls into XAudio2
// (Submit/Stop/...) would otherwise deadlock.
typedef void (MSABI *XACb0)(void*); typedef void (MSABI *XACb1)(void*,void*); typedef void (MSABI *XACbU)(void*,uint32_t);
typedef struct { void** cb; int slot; void* arg; } XAPend;
static XAPend g_xapend[8192]; static int g_nxapend;
static void xa_q(XAVoice*v,int slot,void*arg){ if(v->cb && g_nxapend<8192){ g_xapend[g_nxapend].cb=v->cb; g_xapend[g_nxapend].slot=slot; g_xapend[g_nxapend].arg=arg; g_nxapend++; } }
static void xa_cb_bufstart(XAVoice*v,void*ctx){ xa_q(v,3,ctx); }
static void xa_cb_bufend(XAVoice*v,void*ctx){ xa_q(v,4,ctx); }
static void xa_cb_loopend(XAVoice*v,void*ctx){ xa_q(v,5,ctx); }
static void xa_cb_streamend(XAVoice*v){ xa_q(v,2,0); }
static void xa_cb_passstart(XAVoice*v,uint32_t req){ xa_q(v,0,(void*)(uintptr_t)req); }
static void xa_cb_passend(XAVoice*v){ xa_q(v,1,0); }
static void xa_flush_callbacks(XAPend*list,int n){
    for(int i=0;i<n;i++){ void**cb=list[i].cb; void*fn=cb[0]? ((void**)cb[0])[list[i].slot] : 0; if(!fn) continue;
        switch(list[i].slot){ case 0: ((XACbU)fn)(cb,(uint32_t)(uintptr_t)list[i].arg); break;
            case 1: case 2: ((XACb0)fn)(cb); break; default: ((XACb1)fn)(cb,list[i].arg); break; } } }

// ---- default output matrices ----
static void xa_default_matrix(float*m,int sc,int dc){
    memset(m,0,sizeof(float)*XA_MAXCH*XA_MAXCH);
    if(sc==dc){ for(int i=0;i<sc&&i<XA_MAXCH;i++) m[i*sc+i]=1.0f; return; }
    if(sc==1){ for(int d=0;d<dc&&d<2;d++) m[d*sc]=1.0f; if(dc==1) m[0]=1.0f; return; }
    if(dc==1){ for(int s=0;s<sc;s++) m[s]=1.0f/sc; return; }
    if(dc==2){ // FL FR C LFE BL BR SL SR -> stereo
        static const float L[8]={1,0,0.707f,0,0.707f,0,0.707f,0}, R[8]={0,1,0.707f,0,0,0.707f,0,0.707f};
        for(int s=0;s<sc&&s<8;s++){ m[0*sc+s]=L[s]; m[1*sc+s]=R[s]; } return; }
    for(int s=0;s<sc&&s<dc;s++) m[s*sc+s]=1.0f;   // stereo -> 5.1/7.1: front pair
}
static int xa_inch(XAVoice*v){ return v->ch; }
static void xa_set_default_dst(XAVoice*v){
    v->ndst=0; v->dstdefault=1;
    if(v->kind!=2 && g_xamaster){ v->dst[0]=g_xamaster; v->ndst=1; v->matset[0]=0; xa_default_matrix(v->mat[0],xa_inch(v),g_xamaster->ch); } }
static void xa_apply_sends(XAVoice*v,const void*sends){
    if(!sends){ xa_set_default_dst(v); return; }
    // xaudio2.h structures are #pragma pack(1): SENDS = {UINT32 count; DESC* pSends@4}, DESC = {UINT32 flags; voice*@4} (12 B)
    uint32_t n; memcpy(&n,sends,4); const uint8_t* d; memcpy(&d,(const uint8_t*)sends+4,8);
    v->ndst=0; v->dstdefault=0;
    for(uint32_t i=0;i<n&&i<4&&d;i++){ XAVoice* o; memcpy(&o,d+12*i+4,8); if(!o) continue;
        v->dst[v->ndst]=o; v->matset[v->ndst]=0; xa_default_matrix(v->mat[v->ndst],xa_inch(v),o->ch); v->ndst++; } }

// ---- IXAudio2Voice methods ----
static void MSABI xv_GetVoiceDetails(XAVoice*v,uint32_t*d){ if(d){ d[0]=0; d[1]=0; d[2]=(uint32_t)v->ch; d[3]=(uint32_t)v->rate; } }
static int MSABI xv_SetOutputVoices(XAVoice*v,const void*sends){ pthread_mutex_lock(&g_xamx); xa_apply_sends(v,sends); pthread_mutex_unlock(&g_xamx); return 0; }
static int MSABI xv_ret0(void){ return 0; }
static void MSABI xv_GetEffectState(XAVoice*v,uint32_t i,int*en){ (void)v;(void)i; if(en)*en=1; }
static void MSABI xv_GetFilterParameters(XAVoice*v,float*p){ (void)v; if(p){ uint32_t*u=(uint32_t*)p; u[0]=0; p[1]=1.0f; p[2]=1.0f; } }
static void MSABI xv_GetOutputFilterParameters(XAVoice*v,void*dst,float*p){ (void)v;(void)dst; if(p){ uint32_t*u=(uint32_t*)p; u[0]=0; p[1]=1.0f; p[2]=1.0f; } }
static int MSABI xv_SetVolume(XAVoice*v,float vol,uint32_t op){ (void)op; v->vol=vol; return 0; }
static void MSABI xv_GetVolume(XAVoice*v,float*p){ if(p)*p=v->vol; }
static int MSABI xv_SetChannelVolumes(XAVoice*v,uint32_t n,const float*c,uint32_t op){ (void)op; if(c) for(uint32_t i=0;i<n&&i<XA_MAXCH;i++) v->chvol[i]=c[i]; return 0; }
static void MSABI xv_GetChannelVolumes(XAVoice*v,uint32_t n,float*c){ if(c) for(uint32_t i=0;i<n;i++) c[i]=i<XA_MAXCH?v->chvol[i]:0.0f; }
static int MSABI xv_SetOutputMatrix(XAVoice*v,XAVoice*dst,uint32_t sc,uint32_t dc,const float*m,uint32_t op){ (void)op;
    if(!m||sc>XA_MAXCH||dc>XA_MAXCH) return 0;
    pthread_mutex_lock(&g_xamx);
    int k=-1; for(int i=0;i<v->ndst;i++) if(!dst||v->dst[i]==dst){ k=i; break; }
    if(k>=0){ memset(v->mat[k],0,sizeof v->mat[k]); for(uint32_t d=0;d<dc;d++) for(uint32_t s=0;s<sc;s++) v->mat[k][d*sc+s]=m[d*sc+s]; v->matset[k]=1; }
    pthread_mutex_unlock(&g_xamx);
    if(xa_trace()){ static int n; if(n++<20){ char t[160]; int q=snprintf(t,sizeof t,"  [xaudio] SetOutputMatrix v=%p %ux%u [%.2f %.2f ..]\n",(void*)v,sc,dc,m[0],dc>1?m[sc]:0); (void)!write(2,t,q); } }
    return 0; }
static void MSABI xv_GetOutputMatrix(XAVoice*v,XAVoice*dst,uint32_t sc,uint32_t dc,float*m){
    if(!m) return; int k=-1; for(int i=0;i<v->ndst;i++) if(!dst||v->dst[i]==dst){ k=i; break; }
    for(uint32_t d=0;d<dc;d++) for(uint32_t s=0;s<sc;s++) m[d*sc+s]=(k>=0&&d<XA_MAXCH&&s<XA_MAXCH)?v->mat[k][d*sc+s]:0.0f; }
static void MSABI xv_DestroyVoice(XAVoice*v){ pthread_mutex_lock(&g_xamx); v->alive=0; v->started=0; v->qn=0; v->cb=0;
    if(v==g_xamaster) g_xamaster=0;
    for(int i=0;i<g_nxav;i++) if(g_xav[i]==v){ g_xav[i]=g_xav[--g_nxav]; break; }
    for(int i=0;i<g_nxav;i++){ XAVoice*o=g_xav[i]; for(int k=0;k<o->ndst;k++) if(o->dst[k]==v){ o->dst[k]=o->dst[--o->ndst]; k--; } }
    pthread_mutex_unlock(&g_xamx); /* leaked deliberately: the game may race late calls */ }
// ---- IXAudio2SourceVoice ----
static int MSABI xs_Start(XAVoice*v,uint32_t fl,uint32_t op){ (void)fl;(void)op; v->started=1; return 0; }
static int MSABI xs_Stop(XAVoice*v,uint32_t fl,uint32_t op){ (void)fl;(void)op; v->started=0; return 0; }
static int MSABI xs_SubmitSourceBuffer(XAVoice*v,const uint8_t*b,const void*wma){ (void)wma; if(!b) return 0x80070057;
    pthread_mutex_lock(&g_xamx);
    if(v->qn>=64){ pthread_mutex_unlock(&g_xamx); return 0x88960001; }   // XAUDIO2_E_INVALID_CALL
    XABuf*q=&v->q[(v->qh+v->qn)%64]; memset(q,0,sizeof *q);
    memcpy(&q->flags,b,4); memcpy(&q->bytes,b+4,4); memcpy(&q->data,b+8,8);
    q->pbeg=*(const uint32_t*)(b+16); q->plen=*(const uint32_t*)(b+20); q->lbeg=*(const uint32_t*)(b+24); q->llen=*(const uint32_t*)(b+28); q->lcount=*(const uint32_t*)(b+32);
    memcpy(&q->ctx,b+36,8);   // packed XAUDIO2_BUFFER: pContext at 36
    uint32_t frames=v->align?q->bytes/(uint32_t)v->align:0;
    if(!q->plen||q->pbeg+q->plen>frames){ if(q->pbeg>frames) q->pbeg=frames; q->plen=frames-q->pbeg; }
    if(q->lcount && (!q->llen || q->lbeg+q->llen>q->pbeg+q->plen)){ q->lbeg=q->pbeg; q->llen=q->plen; }
    if(v->qn==0){ v->pos=q->pbeg; v->frac=0; v->startcb=1; }
    v->qn++;
    pthread_mutex_unlock(&g_xamx); return 0; }
static int MSABI xs_FlushSourceBuffers(XAVoice*v){ pthread_mutex_lock(&g_xamx);
    // Flushed buffers still get OnBufferEnd, in queue order, from the mixer's next pass (never inside the
    // caller's XAudio2 call). A STARTED voice keeps the buffer it is playing; a STOPPED voice flushes all of
    // them. Keeping the head of a stopped voice meant its OnBufferEnd never came (the mixer only advances
    // started voices), and the engine, which waits for every voice to drain before loading a map, hung there.
    int keep=(v->started && v->qn)?1:0;
    for(int k=keep;k<v->qn;k++) xa_cb_bufend(v,v->q[(v->qh+k)%64].ctx);
    if(keep) v->qn=1; else { v->qn=0; v->pos=0; v->frac=0; v->startcb=0; }
    pthread_mutex_unlock(&g_xamx); return 0; }
static int MSABI xs_ExitLoop(XAVoice*v,uint32_t op){ (void)op; pthread_mutex_lock(&g_xamx); if(v->qn) v->q[v->qh].lcount=0; pthread_mutex_unlock(&g_xamx); return 0; }
static void MSABI xs_GetState(XAVoice*v,uint8_t*st,uint32_t fl){ (void)fl; if(!st) return; pthread_mutex_lock(&g_xamx);
    void*c=v->qn? v->q[v->qh].ctx : 0; uint32_t qn=(uint32_t)v->qn; memcpy(st,&c,8); memcpy(st+8,&qn,4); memcpy(st+12,&v->played,8); pthread_mutex_unlock(&g_xamx); }   // packed VOICE_STATE
static int MSABI xs_SetFrequencyRatio(XAVoice*v,float r,uint32_t op){ (void)op; if(r<1.0f/1024) r=1.0f/1024; if(r>1024) r=1024; v->ratio=r; return 0; }
static void MSABI xs_GetFrequencyRatio(XAVoice*v,float*r){ if(r)*r=v->ratio; }
static int MSABI xs_SetSourceSampleRate(XAVoice*v,uint32_t r){ if(r) v->rate=(int)r; return 0; }
static void MSABI xm_GetChannelMask(XAVoice*v,uint32_t*m){ if(m) *m = v->ch>=8?0x63F : v->ch>=6?0x3F : v->ch==1?0x4:0x3; }

// PELOAD_TRACE_AUDIO=2: log every call of an unimplemented voice/engine slot (which slot, which object)
#define XA_SLOT(n) static int MSABI xa_slot##n(void*self){ if(g_xa_trace>1){ char t[80]; int k=snprintf(t,sizeof t,"  [xaudio] slot %d on %p\n",n,self); (void)!write(2,t,k);} return 0; }
XA_SLOT(0) XA_SLOT(1) XA_SLOT(2) XA_SLOT(3) XA_SLOT(4) XA_SLOT(5) XA_SLOT(6) XA_SLOT(7) XA_SLOT(8) XA_SLOT(9) XA_SLOT(10) XA_SLOT(11) XA_SLOT(12) XA_SLOT(13) XA_SLOT(14) XA_SLOT(15)
XA_SLOT(16) XA_SLOT(17) XA_SLOT(18) XA_SLOT(19) XA_SLOT(20) XA_SLOT(21) XA_SLOT(22) XA_SLOT(23) XA_SLOT(24) XA_SLOT(25) XA_SLOT(26) XA_SLOT(27) XA_SLOT(28) XA_SLOT(29) XA_SLOT(30) XA_SLOT(31)
static void* g_xa_slotfn[32]={(void*)xa_slot0,(void*)xa_slot1,(void*)xa_slot2,(void*)xa_slot3,(void*)xa_slot4,(void*)xa_slot5,(void*)xa_slot6,(void*)xa_slot7,(void*)xa_slot8,(void*)xa_slot9,(void*)xa_slot10,(void*)xa_slot11,(void*)xa_slot12,(void*)xa_slot13,(void*)xa_slot14,(void*)xa_slot15,
    (void*)xa_slot16,(void*)xa_slot17,(void*)xa_slot18,(void*)xa_slot19,(void*)xa_slot20,(void*)xa_slot21,(void*)xa_slot22,(void*)xa_slot23,(void*)xa_slot24,(void*)xa_slot25,(void*)xa_slot26,(void*)xa_slot27,(void*)xa_slot28,(void*)xa_slot29,(void*)xa_slot30,(void*)xa_slot31};
static void* g_xa_src_vtbl[32], *g_xa_sub_vtbl[32], *g_xa_mas_vtbl[32];
static void xa_vtbl_init(void){ static int done; if(done) return; done=1;
    void** t[3]={g_xa_src_vtbl,g_xa_sub_vtbl,g_xa_mas_vtbl};
    for(int k=0;k<3;k++){ void**v=t[k]; for(int i=0;i<32;i++) v[i]=g_xa_slotfn[i];
        v[0]=(void*)xv_GetVoiceDetails; v[1]=(void*)xv_SetOutputVoices; v[5]=(void*)xv_GetEffectState; v[9]=(void*)xv_GetFilterParameters; v[11]=(void*)xv_GetOutputFilterParameters;
        v[12]=(void*)xv_SetVolume; v[13]=(void*)xv_GetVolume; v[14]=(void*)xv_SetChannelVolumes; v[15]=(void*)xv_GetChannelVolumes;
        v[16]=(void*)xv_SetOutputMatrix; v[17]=(void*)xv_GetOutputMatrix; v[18]=(void*)xv_DestroyVoice; }
    g_xa_src_vtbl[19]=(void*)xs_Start; g_xa_src_vtbl[20]=(void*)xs_Stop; g_xa_src_vtbl[21]=(void*)xs_SubmitSourceBuffer; g_xa_src_vtbl[22]=(void*)xs_FlushSourceBuffers;
    g_xa_src_vtbl[24]=(void*)xs_ExitLoop; g_xa_src_vtbl[25]=(void*)xs_GetState; g_xa_src_vtbl[26]=(void*)xs_SetFrequencyRatio; g_xa_src_vtbl[27]=(void*)xs_GetFrequencyRatio; g_xa_src_vtbl[28]=(void*)xs_SetSourceSampleRate;
    g_xa_mas_vtbl[19]=(void*)xm_GetChannelMask; }
static XAVoice* xa_new(int kind,void**vt){ XAVoice*v=calloc(1,sizeof *v); v->vtbl=vt; v->kind=kind; v->vol=1.0f; for(int i=0;i<XA_MAXCH;i++) v->chvol[i]=1.0f; v->ratio=1.0f; v->alive=1; return v; }
static void xa_register(XAVoice*v){ pthread_mutex_lock(&g_xamx); if(g_nxav<1024) g_xav[g_nxav++]=v; pthread_mutex_unlock(&g_xamx); }

// ---- IXAudio2 ----
static void xa_start_output(void);
static int MSABI xa2_RegisterForCallbacks(void*self,void**cb){ (void)self; pthread_mutex_lock(&g_xamx); if(g_nxaengcb<8) g_xaengcb[g_nxaengcb++]=cb; pthread_mutex_unlock(&g_xamx); return 0; }
static void MSABI xa2_UnregisterForCallbacks(void*self,void**cb){ (void)self; pthread_mutex_lock(&g_xamx); for(int i=0;i<g_nxaengcb;i++) if(g_xaengcb[i]==cb){ g_xaengcb[i]=g_xaengcb[--g_nxaengcb]; break; } pthread_mutex_unlock(&g_xamx); }
static int MSABI xa2_CreateSourceVoice(void*self,XAVoice**pp,const uint8_t*wfx,uint32_t fl,float maxr,void**cb,const void*sends,const void*fx){ (void)self;(void)fl;(void)maxr;(void)fx;
    if(g_xa_trace>1){ char t[120]; int k=snprintf(t,sizeof t,"  [xaudio] CreateSourceVoice pp=%p wfx=%p cb=%p sends=%p\n",(void*)pp,(void*)wfx,(void*)cb,sends); (void)!write(2,t,k); }
    xa_vtbl_init(); XAVoice*v=xa_new(0,g_xa_src_vtbl);
    if(wfx){ uint16_t tag=*(const uint16_t*)wfx; v->ch=*(const uint16_t*)(wfx+2); v->rate=(int)*(const uint32_t*)(wfx+4); v->align=*(const uint16_t*)(wfx+12); v->bits=*(const uint16_t*)(wfx+14);
        uint16_t cbs=*(const uint16_t*)(wfx+16);
        if(tag==0xFFFE && cbs>=22){ uint32_t sub=*(const uint32_t*)(wfx+24); tag=(uint16_t)sub; }
        v->fmt = (tag==1)?1 : (tag==3)?3 : 0;
        if(v->fmt==1 && v->bits!=8 && v->bits!=16 && v->bits!=24 && v->bits!=32) v->fmt=0;
        if(xa_trace() || !v->fmt){ static int n; if(n++<40){ char t[180]; int k=snprintf(t,sizeof t,"  [xaudio] CreateSourceVoice tag=0x%x ch=%d rate=%d bits=%d align=%d %s\n",tag,v->ch,v->rate,v->bits,v->align,v->fmt?"":"(UNSUPPORTED -> silent)"); (void)!write(2,t,k); } } }
    if(v->ch<1) v->ch=1; if(v->ch>XA_MAXCH) v->ch=XA_MAXCH; if(v->rate<1) v->rate=XA_RATE; if(v->align<1) v->align=v->ch*2;
    v->cb=cb; pthread_mutex_lock(&g_xamx); xa_apply_sends(v,sends); pthread_mutex_unlock(&g_xamx);
    xa_register(v); if(pp)*pp=v; return 0; }
static int MSABI xa2_CreateSubmixVoice(void*self,XAVoice**pp,uint32_t ich,uint32_t irate,uint32_t fl,uint32_t stage,const void*sends,const void*fx){ (void)self;(void)fl;(void)fx;
    xa_vtbl_init(); XAVoice*v=xa_new(1,g_xa_sub_vtbl); v->ch=ich?(int)ich:2; if(v->ch>XA_MAXCH) v->ch=XA_MAXCH; v->rate=irate?(int)irate:XA_RATE; v->stage=(int)stage;
    pthread_mutex_lock(&g_xamx); xa_apply_sends(v,sends); pthread_mutex_unlock(&g_xamx);
    xa_register(v); if(pp)*pp=v; return 0; }
static int MSABI xa2_CreateMasteringVoice(void*self,XAVoice**pp,uint32_t ich,uint32_t irate,uint32_t fl,const void*dev,const void*fx,uint32_t cat){ (void)self;(void)fl;(void)dev;(void)fx;(void)cat;
    xa_vtbl_init(); XAVoice*v=xa_new(2,g_xa_mas_vtbl); v->ch=ich?(int)ich:2; if(v->ch>XA_MAXCH) v->ch=XA_MAXCH; v->rate=XA_RATE; (void)irate;
    pthread_mutex_lock(&g_xamx); g_xamaster=v; pthread_mutex_unlock(&g_xamx);
    xa_register(v); if(pp)*pp=v; xa_start_output();
    if(xa_trace()){ char t[96]; int k=snprintf(t,sizeof t,"  [xaudio] CreateMasteringVoice ch=%d\n",v->ch); (void)!write(2,t,k); }
    return 0; }
static void MSABI xa2_GetPerformanceData(void*self,void*p){ (void)self; if(p) memset(p,0,64); }
static void* g_xa2_vtbl[16]; static ComObj g_xa2 = { g_xa2_vtbl };
static int MSABI i_XAudio2Create_real(void**pp,uint32_t flags,uint32_t proc){ (void)flags;(void)proc;
    static int init; if(!init){ init=1; xa_trace(); if(getenv("PELOAD_TRACE_AUDIO")&&atoi(getenv("PELOAD_TRACE_AUDIO"))>1) g_xa_trace=2; for(int i=0;i<16;i++) g_xa2_vtbl[i]=g_xa_slotfn[i];
        g_xa2_vtbl[0]=(void*)com_QI; g_xa2_vtbl[1]=(void*)com_AddRef; g_xa2_vtbl[2]=(void*)com_Release;
        g_xa2_vtbl[3]=(void*)xa2_RegisterForCallbacks; g_xa2_vtbl[4]=(void*)xa2_UnregisterForCallbacks;
        g_xa2_vtbl[5]=(void*)xa2_CreateSourceVoice; g_xa2_vtbl[6]=(void*)xa2_CreateSubmixVoice; g_xa2_vtbl[7]=(void*)xa2_CreateMasteringVoice;
        g_xa2_vtbl[11]=(void*)xa2_GetPerformanceData; }
    if(pp)*pp=&g_xa2; return 0; }

// ---- X3DAudio ----
static int MSABI i_X3DAudioInitialize(uint32_t mask,float sos,uint8_t*h){ (void)sos; if(h){ memset(h,0,20); memcpy(h,&mask,4); } return 0; }
static float x3d_curve(const uint8_t*curve,float nd,float def){
    if(!curve) return def; const float*pts=*(const float* const*)curve; uint32_t n=*(const uint32_t*)(curve+8); if(!pts||!n) return def;
    if(nd<=pts[0]) return pts[1];
    for(uint32_t i=1;i<n;i++){ float d0=pts[2*(i-1)],v0=pts[2*(i-1)+1],d1=pts[2*i],v1=pts[2*i+1]; if(nd<=d1){ float t=(d1>d0)?(nd-d0)/(d1-d0):0; return v0+(v1-v0)*t; } }
    return pts[2*(n-1)+1]; }
static void MSABI i_X3DAudioCalculate(const uint8_t*h,const float*L,const uint8_t*E,uint32_t fl,uint8_t*dsp){ (void)h;
    if(!L||!E||!dsp) return;
    const float*F=L, *T=L+3, *LP=L+6; const float*EP=(const float*)(E+32);
    float R[3]={ T[1]*F[2]-T[2]*F[1], T[2]*F[0]-T[0]*F[2], T[0]*F[1]-T[1]*F[0] };   // right = top x front (left-handed)
    float v[3]={EP[0]-LP[0],EP[1]-LP[1],EP[2]-LP[2]}; float d=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    float x=v[0]*R[0]+v[1]*R[1]+v[2]*R[2], z=v[0]*F[0]+v[1]*F[1]+v[2]*F[2];
    float scaler=*(const float*)(E+120); if(scaler<=0) scaler=1.0f;
    const uint8_t*vc=*(const uint8_t* const*)(E+80);
    float gain = vc ? x3d_curve(vc,d/scaler,1.0f) : (d<=scaler?1.0f:scaler/d);
    float pan = d>1e-4f ? x/d : 0.0f; if(pan<-1)pan=-1; if(pan>1)pan=1;
    float gl=cosf((pan+1.0f)*0.78539816f), gr=sinf((pan+1.0f)*0.78539816f);
    uint32_t sc=*(const uint32_t*)(dsp+16), dc=*(const uint32_t*)(dsp+20); float*m=*(float**)dsp;
    if((fl&1) && m){ for(uint32_t i=0;i<sc*dc;i++) m[i]=0;
        for(uint32_t s=0;s<sc;s++){ if(dc==1) m[s]=gain; else { m[0*sc+s]=gain*gl; m[1*sc+s]=gain*gr; } } }
    float*dl=*(float**)(dsp+8); if((fl&2)&&dl) for(uint32_t i=0;i<dc;i++) dl[i]=0;
    *(float*)(dsp+24)=1.0f; *(float*)(dsp+28)=1.0f; *(float*)(dsp+32)=0.0f; *(float*)(dsp+36)=1.0f;
    *(float*)(dsp+40)=atan2f(x,z); *(float*)(dsp+44)=d; *(float*)(dsp+48)=0; *(float*)(dsp+52)=0; }

// ---- mixer ----
static float xa_sample(XAVoice*v,const XABuf*b,uint64_t fr,int c){
    if(fr>=(uint64_t)b->pbeg+b->plen && !(b->lcount && fr<(uint64_t)b->lbeg+b->llen)) return 0.0f;
    uint64_t off=fr*(uint64_t)v->align; if(off+(uint64_t)v->align>b->bytes) return 0.0f;
    const uint8_t*p=b->data+off;
    if(v->fmt==3) return ((const float*)p)[c];
    switch(v->bits){ case 16: return ((const int16_t*)p)[c]*(1.0f/32768.0f);
        case 8: return (p[c]-128)*(1.0f/128.0f);
        case 24: { const uint8_t*s=p+3*c; int32_t x=(int32_t)((uint32_t)s[0]<<8|(uint32_t)s[1]<<16|(uint32_t)s[2]<<24)>>8; return x*(1.0f/8388608.0f); }
        case 32: return ((const int32_t*)p)[c]*(1.0f/2147483648.0f); }
    return 0.0f; }
// render XA_BLOCK output frames of source v into its destinations
static void xa_render_source(XAVoice*v){
    float tmp[XA_MAXCH*XA_BLOCK]; int ch=v->ch; int n=0;
    double step=(double)v->rate*v->ratio/XA_RATE;
    while(n<XA_BLOCK && v->qn){
        XABuf*b=&v->q[v->qh];
        if(v->startcb){ v->startcb=0; xa_cb_bufstart(v,b->ctx); if(!v->qn) break; b=&v->q[v->qh]; }
        uint64_t end = (b->lcount ? (uint64_t)b->lbeg+b->llen : (uint64_t)b->pbeg+b->plen);
        if(v->pos>=end){
            if(b->lcount){ if(b->lcount!=255) b->lcount--; xa_cb_loopend(v,b->ctx); v->pos=b->lbeg; continue; }
            void*ctx=b->ctx; uint32_t f=b->flags; v->qh=(v->qh+1)%64; v->qn--;
            xa_cb_bufend(v,ctx); if(f&0x40) xa_cb_streamend(v);
            if(v->qn){ v->pos=v->q[v->qh].pbeg; v->frac=0; v->startcb=1; }
            continue; }
        if(!v->fmt){ v->pos=end; continue; }     // unsupported format: consume silently (keeps the game's streaming alive)
        for(int c=0;c<ch;c++){ float a=xa_sample(v,b,v->pos,c), bb=(v->pos+1<end)?xa_sample(v,b,v->pos+1,c):a; tmp[n*ch+c]=a+(bb-a)*(float)v->frac; }
        n++; v->played++;
        v->frac+=step; uint64_t adv=(uint64_t)v->frac; v->frac-=(double)adv; v->pos+=adv; }
    if(!n) return;
    for(int k=0;k<v->ndst;k++){ XAVoice*o=v->dst[k]; if(!o||!o->alive) continue; int dc=o->ch; const float*m=v->mat[k];
        for(int i=0;i<n;i++) for(int d=0;d<dc;d++){ float s=0; for(int c=0;c<ch;c++) s+=m[d*ch+c]*tmp[i*ch+c]*v->chvol[c]; o->acc[i*dc+d]+=s*v->vol; } } }
static void xa_render_submix(XAVoice*v){
    int ch=v->ch;
    for(int k=0;k<v->ndst;k++){ XAVoice*o=v->dst[k]; if(!o||!o->alive||o==v) continue; int dc=o->ch; const float*m=v->mat[k];
        for(int i=0;i<XA_BLOCK;i++) for(int d=0;d<dc;d++){ float s=0; for(int c=0;c<ch;c++) s+=m[d*ch+c]*v->acc[i*ch+c]*v->chvol[c]; o->acc[i*dc+d]+=s*v->vol; } } }

// output ring (stereo float) consumed by the AudioQueue callback
#define XA_RING (XA_RATE/2)                // 0.5 s
static float g_xaring[XA_RING*2]; static volatile uint64_t g_xaw, g_xar;
static void xa_aq_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef buf){ (void)ud;
    float*o=buf->mAudioData; uint32_t frames=buf->mAudioDataBytesCapacity/8;
    static int mute=-1; if(mute<0) mute=getenv("PELOAD_AUDIO_MUTE")?1:0;   // test runs: mix (and PELOAD_AUDIO_DUMP) but play silence
    for(uint32_t i=0;i<frames;i++){ if(g_xar<g_xaw){ uint64_t r=g_xar%XA_RING; o[2*i]=g_xaring[2*r]; o[2*i+1]=g_xaring[2*r+1]; g_xar++; } else { o[2*i]=o[2*i+1]=0; } }
    if(g_rec_audio) rec_audio_push(&g_xaring,o,frames,2,XA_RATE,32,8,1);   // clip recording (rec.m): the game's mix, before the mute
    if(mute) memset(o,0,(size_t)frames*8);
    buf->mAudioDataByteSize=frames*8; AudioQueueEnqueueBuffer(q,buf,0,0); }
static uint32_t MSABI xa_mixer_thread(void*arg){ (void)arg;
    double next=mono_s();
    for(;;){
        // keep ~40 ms buffered ahead of the device
        while(g_xaw>g_xar+XA_RATE/25){ struct timespec ts={0,2000000}; nanosleep(&ts,0); }
        pthread_mutex_lock(&g_xamx);
        int ne=g_nxaengcb; void** eng[8]; for(int e=0;e<ne;e++) eng[e]=g_xaengcb[e];
        for(int i=0;i<g_nxav;i++){ XAVoice*v=g_xav[i]; if(v->kind) memset(v->acc,0,sizeof(float)*(size_t)v->ch*XA_BLOCK); }
        for(int i=0;i<g_nxav;i++){ XAVoice*v=g_xav[i]; if(v->kind||!v->alive) continue;
            if(v->cb){ uint32_t queued=0; for(int k=0;k<v->qn;k++){ XABuf*b=&v->q[(v->qh+k)%64]; queued+=b->pbeg+b->plen; } (void)queued;
                xa_cb_passstart(v, v->started && v->qn==0 ? (uint32_t)(XA_BLOCK*(double)v->rate/XA_RATE*v->align) : 0); }
            if(v->started) xa_render_source(v);
            if(v->cb) xa_cb_passend(v); }
        for(int st=0;st<8;st++) for(int i=0;i<g_nxav;i++){ XAVoice*v=g_xav[i]; if(v->kind==1 && v->alive && (v->stage==st || (st==7 && v->stage>7))) xa_render_submix(v); }
        XAVoice*m=g_xamaster;
        uint64_t w=g_xaw;
        for(int i=0;i<XA_BLOCK;i++){ float l=0,r=0;
            if(m&&m->alive){ int mc=m->ch; const float*a=m->acc+i*mc;
                if(mc==1){ l=r=a[0]; } else { l=a[0]; r=a[1]; if(mc>=6){ l+=0.707f*(a[2]+a[4]); r+=0.707f*(a[2]+a[5]); } }
                l*=m->vol; r*=m->vol; }
            if(l>1)l=1; if(l<-1)l=-1; if(r>1)r=1; if(r<-1)r=-1;
            uint64_t k=(w+i)%XA_RING; g_xaring[2*k]=l; g_xaring[2*k+1]=r; }
        static XAPend local[8192]; int nl=g_nxapend; memcpy(local,g_xapend,sizeof(XAPend)*(size_t)nl); g_nxapend=0;
        pthread_mutex_unlock(&g_xamx);
        // PELOAD_AUDIO_DUMP=<file.raw>: append the mixed stereo float32 48 kHz output (first 60 s) for offline checks
        { static FILE*df; static int dfi; static uint64_t dn; if(!dfi){ dfi=1; const char*p=getenv("PELOAD_AUDIO_DUMP"); if(p) df=fopen(p,"wb"); }
          static double dsecs=-1; if(dsecs<0){ const char*e=getenv("PELOAD_AUDIO_DUMP_SECS"); dsecs=e?atof(e):60; }
          if(df && dn<(uint64_t)(XA_RATE*dsecs)){ for(int i=0;i<XA_BLOCK;i++){ uint64_t k=(w+i)%XA_RING; fwrite(&g_xaring[2*k],4,2,df); } dn+=XA_BLOCK; fflush(df); } }
        for(int e=0;e<ne;e++) ((XACb0)((void**)eng[e][0])[0])(eng[e]);   // engine OnProcessingPassStart
        xa_flush_callbacks(local,nl);                                    // voice callbacks (outside the lock)
        for(int e=0;e<ne;e++) ((XACb0)((void**)eng[e][0])[1])(eng[e]);   // engine OnProcessingPassEnd
        g_xaw=w+XA_BLOCK;
        next+=XA_BLOCK/(double)XA_RATE; }
    return 0; }
static void* MSABI i_CreateThread(void*attr,uint64_t stack,WinThreadProc start,void*param,uint32_t fl,uint32_t*tid); // fwd
static void xa_start_output(void){
    static int started; if(started) return; started=1;
    AudioStreamBasicDescription f; memset(&f,0,sizeof f);
    f.mSampleRate=XA_RATE; f.mFormatID=kAudioFormatLinearPCM; f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=2; f.mBitsPerChannel=32; f.mBytesPerFrame=8; f.mBytesPerPacket=8;
    AudioQueueRef q=0; OSStatus st=AudioQueueNewOutput(&f,xa_aq_cb,0,0,0,0,&q);
    if(st||!q){ char t[96]; int k=snprintf(t,sizeof t,"  [xaudio] AudioQueueNewOutput failed %d\n",(int)st); (void)!write(2,t,k); return; }
    for(int i=0;i<3;i++){ AudioQueueBufferRef b=0; if(!AudioQueueAllocateBuffer(q,XA_BLOCK*8,&b)){ memset(b->mAudioData,0,XA_BLOCK*8); b->mAudioDataByteSize=XA_BLOCK*8; AudioQueueEnqueueBuffer(q,b,0,0); } }
    AudioQueueStart(q,0);
    uint32_t tid=0; if(!getenv("PELOAD_AUDIO_NOMIX")) i_CreateThread(0,0,(WinThreadProc)xa_mixer_thread,0,0,&tid);
    char t[96]; int k=snprintf(t,sizeof t,"  [xaudio] CoreAudio output running (48 kHz stereo), mixer thread %u\n",tid); (void)!write(2,t,k); }
