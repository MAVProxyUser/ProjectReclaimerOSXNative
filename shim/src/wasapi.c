// wasapi.c — a WASAPI render endpoint on CoreAudio, for the Reclaimer client's OWN audio. Included by peload.c.
// Halo 3's game audio goes through XAudio2 (xaudio.c); the client itself plays menu music and voice chat through the
// Rust crate cpal 0.15 (WASAPI host): CoCreateInstance(MMDeviceEnumerator) -> GetDefaultAudioEndpoint(eRender) ->
// IMMDevice::Activate(IAudioClient) -> GetMixFormat / IsFormatSupported -> Initialize(shared, EVENTCALLBACK) ->
// SetEventHandle -> GetBufferSize -> GetService(IAudioRenderClient, IAudioClock) -> Start, then a thread that waits on
// the event and fills GetCurrentPadding..GetBuffer..ReleaseBuffer. The old stub reported no devices ("Menu music output
// unavailable: No music playback device", "Voice audio unavailable"). PELOAD_NO_WASAPI=1 brings that stub back.
// Each IAudioClient gets its own AudioQueue in exactly the format the client initialised (no conversion); its callback
// drains the stream's ring buffer every 10 ms and signals the client's event so it refills. Capture (microphone) is
// not provided: eCapture has no endpoints. PELOAD_AUDIO=0 (test players) mutes these queues too.

typedef struct WaStream WaStream;
typedef struct { void**vtbl; WaStream*s; } WaItf;           // one COM interface of a stream (container: WaStream)
struct WaStream {
    WaItf ac, rc, clk;                                       // IAudioClient, IAudioRenderClient, IAudioClock
    volatile int ref; pthread_mutex_t m;
    int inited, started; uint32_t rate, ch, bits, blk, isfloat, bufframes;
    uint8_t*ring, *scratch; uint32_t scratch_cap; uint64_t rd, wr, played;   // ring positions in frames (monotonic)
    void*event; AudioQueueRef q; };
#define WA_E_NOINTERFACE 0x80004002
#define WA_E_POINTER     0x80004003
#define WA_E_NOT_INIT    0x88890001
#define WA_E_ALREADY     0x88890002
#define WA_E_TOO_LARGE   0x88890006
#define WA_E_UNSUPPORTED 0x88890008
static int wa_guid(const GUID_*g,uint32_t d1){ return g && g->D1==d1; }
static void wa_log(const char*s){ if(getenv("PELOAD_TRACE_WASAPI")) (void)!write(2,s,strlen(s)); }

// ---- the stream ---------------------------------------------------------------------------------------------------
static void wa_aq_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef b){
    WaStream*s=ud; uint32_t frames=b->mAudioDataBytesCapacity/s->blk;
    pthread_mutex_lock(&s->m);
    uint64_t avail=s->wr-s->rd; uint32_t n=avail<frames?(uint32_t)avail:frames;
    for(uint32_t i=0;i<n;){ uint32_t pos=(uint32_t)((s->rd+i)%s->bufframes), run=s->bufframes-pos; if(run>n-i) run=n-i;
        memcpy((uint8_t*)b->mAudioData+(size_t)i*s->blk, s->ring+(size_t)pos*s->blk, (size_t)run*s->blk); i+=run; }
    if(n<frames) memset((uint8_t*)b->mAudioData+(size_t)n*s->blk,0,(size_t)(frames-n)*s->blk);   // underrun: silence
    s->rd+=n; s->played+=frames; void*ev=s->event;
    pthread_mutex_unlock(&s->m);
    b->mAudioDataByteSize=frames*s->blk; AudioQueueEnqueueBuffer(q,b,0,0);
    if(ev) i_SetEvent(ev); }                                  // room in the buffer: let the client refill
static void wa_free(WaStream*s){
    if(s->q){ AudioQueueStop(s->q,1); AudioQueueDispose(s->q,1); s->q=0; }
    free(s->ring); free(s->scratch); pthread_mutex_destroy(&s->m); free(s); wa_log("  [wasapi] stream released\n"); }
static unsigned MSABI wa_AddRef(WaItf*self){ return (unsigned)__atomic_add_fetch(&self->s->ref,1,__ATOMIC_ACQ_REL); }
static unsigned MSABI wa_Release(WaItf*self){ WaStream*s=self->s; int r=__atomic_sub_fetch(&s->ref,1,__ATOMIC_ACQ_REL); if(r==0) wa_free(s); return (unsigned)(r<0?0:r); }
static int MSABI wa_QI(WaItf*self,GUID_*riid,void**ppv){ if(!ppv) return WA_E_POINTER; *ppv=self; wa_AddRef(self); (void)riid; return HR_OK; }

// IAudioClient
static int MSABI wac_Initialize(WaItf*self,int mode,uint32_t flags,int64_t dur,int64_t period,const uint8_t*fmt,const GUID_*session){
    (void)period;(void)session; WaStream*s=self->s; if(!fmt) return WA_E_POINTER; if(s->inited) return WA_E_ALREADY;
    uint16_t tag=*(const uint16_t*)fmt, ch=*(const uint16_t*)(fmt+2), blk=*(const uint16_t*)(fmt+12), bits=*(const uint16_t*)(fmt+14);
    uint32_t rate=*(const uint32_t*)(fmt+4); int isfloat = tag==3 || (tag==0xFFFE && *(const uint32_t*)(fmt+24)==3);
    if(!ch || ch>8 || !rate || !blk || !(bits==16||bits==24||bits==32)) return WA_E_UNSUPPORTED;
    s->rate=rate; s->ch=ch; s->bits=bits; s->blk=blk; s->isfloat=isfloat;
    uint64_t want=dur>0?(uint64_t)dur*rate/10000000ull:0; if(want<rate/10) want=rate/10; if(want>rate) want=rate;   // 100 ms .. 1 s
    s->bufframes=(uint32_t)want; s->ring=calloc(s->bufframes,blk); s->scratch_cap=s->bufframes; s->scratch=calloc(s->scratch_cap,blk);
    AudioStreamBasicDescription f={0}; f.mSampleRate=rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=(isfloat?kAudioFormatFlagIsFloat:kAudioFormatFlagIsSignedInteger)|kAudioFormatFlagIsPacked;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=ch; f.mBitsPerChannel=bits; f.mBytesPerFrame=blk; f.mBytesPerPacket=blk;
    OSStatus st=AudioQueueNewOutput(&f,wa_aq_cb,s,0,0,0,&s->q);
    if(st||!s->q){ char t[96]; int k=snprintf(t,sizeof t,"  [wasapi] AudioQueueNewOutput failed %d\n",(int)st); (void)!write(2,t,k); return WA_E_UNSUPPORTED; }
    if(!xa_real()) AudioQueueSetParameter(s->q,kAudioQueueParam_Volume,0.0f);   // PELOAD_AUDIO=0: keep the clock running, play nothing
    uint32_t per=rate/100; for(int i=0;i<3;i++){ AudioQueueBufferRef b=0; if(!AudioQueueAllocateBuffer(s->q,per*blk,&b)){ memset(b->mAudioData,0,per*blk); b->mAudioDataByteSize=per*blk; AudioQueueEnqueueBuffer(s->q,b,0,0); } }
    s->inited=1;
    { char t[200]; int k=snprintf(t,sizeof t,"  [wasapi] stream: %u Hz, %u ch, %u-bit %s, buffer %u frames, mode %d flags 0x%x\n",rate,ch,bits,isfloat?"float":"int",s->bufframes,mode,flags); (void)!write(2,t,k); }
    return HR_OK; }
static int MSABI wac_GetBufferSize(WaItf*self,uint32_t*n){ if(!n) return WA_E_POINTER; if(!self->s->inited) return WA_E_NOT_INIT; *n=self->s->bufframes; return HR_OK; }
static int MSABI wac_GetStreamLatency(WaItf*self,int64_t*l){ (void)self; if(l)*l=100000; return HR_OK; }   // 10 ms
static int MSABI wac_GetCurrentPadding(WaItf*self,uint32_t*n){ WaStream*s=self->s; if(!n) return WA_E_POINTER; if(!s->inited) return WA_E_NOT_INIT;
    pthread_mutex_lock(&s->m); *n=(uint32_t)(s->wr-s->rd); pthread_mutex_unlock(&s->m); return HR_OK; }
static int MSABI wac_IsFormatSupported(WaItf*self,int mode,const uint8_t*fmt,void**closest){ (void)self;(void)mode; if(closest)*closest=0; if(!fmt) return WA_E_POINTER;
    uint16_t tag=*(const uint16_t*)fmt, ch=*(const uint16_t*)(fmt+2), bits=*(const uint16_t*)(fmt+14);
    int ok=(tag==1||tag==3||tag==0xFFFE) && ch>=1 && ch<=8 && (bits==16||bits==24||bits==32); return ok?HR_OK:WA_E_UNSUPPORTED; }
static uint8_t* wa_mixformat(void){   // WAVEFORMATEXTENSIBLE: 48 kHz stereo float, the macOS default
    uint8_t*w=i_CoTaskMemAlloc(40); memset(w,0,40);
    *(uint16_t*)w=0xFFFE; *(uint16_t*)(w+2)=2; *(uint32_t*)(w+4)=48000; *(uint32_t*)(w+8)=48000*8; *(uint16_t*)(w+12)=8; *(uint16_t*)(w+14)=32; *(uint16_t*)(w+16)=22;
    *(uint16_t*)(w+18)=32; *(uint32_t*)(w+20)=3;                                     // valid bits, channel mask FL|FR
    static const uint8_t sub[16]={3,0,0,0,0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71};  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    memcpy(w+24,sub,16); return w; }
static int MSABI wac_GetMixFormat(WaItf*self,uint8_t**pp){ (void)self; if(!pp) return WA_E_POINTER; *pp=wa_mixformat(); return HR_OK; }
static int MSABI wac_GetDevicePeriod(WaItf*self,int64_t*def,int64_t*min){ (void)self; if(def)*def=100000; if(min)*min=30000; return HR_OK; }
static int MSABI wac_Start(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT; if(!s->started){ AudioQueueStart(s->q,0); s->started=1; if(s->event) i_SetEvent(s->event); } return HR_OK; }
static int MSABI wac_Stop(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT; if(s->started){ AudioQueuePause(s->q); s->started=0; } return HR_OK; }
static int MSABI wac_Reset(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT; pthread_mutex_lock(&s->m); s->rd=s->wr; pthread_mutex_unlock(&s->m); return HR_OK; }
static int MSABI wac_SetEventHandle(WaItf*self,void*h){ self->s->event=h; return HR_OK; }
static int MSABI wac_GetService(WaItf*self,GUID_*riid,void**ppv){ WaStream*s=self->s; if(!ppv) return WA_E_POINTER; *ppv=0;
    if(wa_guid(riid,0xF294ACFC)){ *ppv=&s->rc; wa_AddRef(&s->rc); return HR_OK; }    // IAudioRenderClient
    if(wa_guid(riid,0xCD63314F)){ *ppv=&s->clk; wa_AddRef(&s->clk); return HR_OK; }   // IAudioClock
    return WA_E_NOINTERFACE; }
static void* g_wa_ac_vtbl[]={ (void*)wa_QI,(void*)wa_AddRef,(void*)wa_Release,(void*)wac_Initialize,(void*)wac_GetBufferSize,(void*)wac_GetStreamLatency,
    (void*)wac_GetCurrentPadding,(void*)wac_IsFormatSupported,(void*)wac_GetMixFormat,(void*)wac_GetDevicePeriod,(void*)wac_Start,(void*)wac_Stop,(void*)wac_Reset,
    (void*)wac_SetEventHandle,(void*)wac_GetService };
// IAudioRenderClient: the client writes into scratch; ReleaseBuffer copies it into the ring
static int MSABI warc_GetBuffer(WaItf*self,uint32_t n,uint8_t**pp){ WaStream*s=self->s; if(!pp) return WA_E_POINTER;
    pthread_mutex_lock(&s->m); uint32_t pad=(uint32_t)(s->wr-s->rd); pthread_mutex_unlock(&s->m);
    if(n>s->bufframes-pad) return WA_E_TOO_LARGE;
    if(n>s->scratch_cap){ free(s->scratch); s->scratch_cap=n; s->scratch=calloc(n,s->blk); }
    *pp=s->scratch; return HR_OK; }
static int MSABI warc_ReleaseBuffer(WaItf*self,uint32_t n,uint32_t flags){ WaStream*s=self->s;
    pthread_mutex_lock(&s->m);
    for(uint32_t i=0;i<n;){ uint32_t pos=(uint32_t)((s->wr+i)%s->bufframes), run=s->bufframes-pos; if(run>n-i) run=n-i;
        if(flags&2) memset(s->ring+(size_t)pos*s->blk,0,(size_t)run*s->blk);           // AUDCLNT_BUFFERFLAGS_SILENT
        else memcpy(s->ring+(size_t)pos*s->blk,s->scratch+(size_t)i*s->blk,(size_t)run*s->blk); i+=run; }
    s->wr+=n; pthread_mutex_unlock(&s->m); return HR_OK; }
static void* g_wa_rc_vtbl[]={ (void*)wa_QI,(void*)wa_AddRef,(void*)wa_Release,(void*)warc_GetBuffer,(void*)warc_ReleaseBuffer };
// IAudioClock: position in frames, frequency = sample rate
static int MSABI wack_GetFrequency(WaItf*self,uint64_t*f){ if(f)*f=self->s->rate?self->s->rate:48000; return HR_OK; }
static int MSABI wack_GetPosition(WaItf*self,uint64_t*pos,uint64_t*qpc){ WaStream*s=self->s; pthread_mutex_lock(&s->m); uint64_t p=s->played; pthread_mutex_unlock(&s->m);
    if(pos)*pos=p; if(qpc)*qpc=(uint64_t)(mono_s()*1e7); return HR_OK; }
static int MSABI wack_GetCharacteristics(WaItf*self,uint32_t*c){ (void)self; if(c)*c=0; return HR_OK; }
static void* g_wa_clk_vtbl[]={ (void*)wa_QI,(void*)wa_AddRef,(void*)wa_Release,(void*)wack_GetFrequency,(void*)wack_GetPosition,(void*)wack_GetCharacteristics };
static WaStream* wa_stream_new(void){ WaStream*s=calloc(1,sizeof *s); pthread_mutex_init(&s->m,0); s->ref=1;
    s->ac.vtbl=g_wa_ac_vtbl; s->ac.s=s; s->rc.vtbl=g_wa_rc_vtbl; s->rc.s=s; s->clk.vtbl=g_wa_clk_vtbl; s->clk.s=s; return s; }

// ---- the device: IMMDevice + IMMEndpoint + IPropertyStore (static singletons) ---------------------------------------
static ComObj g_wa_dev, g_wa_ep, g_wa_props;
static int MSABI wadev_QI(void*self,GUID_*riid,void**ppv){ (void)self; if(!ppv) return WA_E_POINTER;
    *ppv = wa_guid(riid,0x1BE09788) ? (void*)&g_wa_ep : (void*)&g_wa_dev; return HR_OK; }        // IMMEndpoint, else IMMDevice/IUnknown
static int MSABI wadev_Activate(void*self,GUID_*iid,uint32_t ctx,void*params,void**ppv){ (void)self;(void)ctx;(void)params; if(!ppv) return WA_E_POINTER; *ppv=0;
    if(wa_guid(iid,0x1CB9AD4C) || wa_guid(iid,0x726778CD) || wa_guid(iid,0x7ED4EE07)){ WaStream*s=wa_stream_new(); *ppv=&s->ac; wa_log("  [wasapi] Activate IAudioClient\n"); return HR_OK; }
    return WA_E_NOINTERFACE; }
static int MSABI wadev_OpenPropertyStore(void*self,uint32_t acc,void**pp){ (void)self;(void)acc; if(pp)*pp=&g_wa_props; return HR_OK; }
static uint16_t* wa_wstr(const char*s){ size_t n=strlen(s); uint16_t*w=i_CoTaskMemAlloc((n+1)*2); for(size_t i=0;i<=n;i++) w[i]=(uint8_t)s[i]; return w; }
static int MSABI wadev_GetId(void*self,uint16_t**id){ (void)self; if(id)*id=wa_wstr("{0.0.0.00000000}.{macos-coreaudio-default-render}"); return HR_OK; }
static int MSABI wadev_GetState(void*self,uint32_t*st){ (void)self; if(st)*st=1; return HR_OK; }   // DEVICE_STATE_ACTIVE
static void* g_wa_dev_vtbl[]={ (void*)wadev_QI,(void*)com_AddRef,(void*)com_Release,(void*)wadev_Activate,(void*)wadev_OpenPropertyStore,(void*)wadev_GetId,(void*)wadev_GetState };
static int MSABI waep_GetDataFlow(void*self,int*df){ (void)self; if(df)*df=0; return HR_OK; }      // eRender
static void* g_wa_ep_vtbl[]={ (void*)wadev_QI,(void*)com_AddRef,(void*)com_Release,(void*)waep_GetDataFlow };
// IPropertyStore: every key reads as the device's friendly name (PROPVARIANT: vt @0, value @8; VT_LPWSTR = 31)
static int MSABI waps_GetCount(void*self,uint32_t*n){ (void)self; if(n)*n=1; return HR_OK; }
static int MSABI waps_GetAt(void*self,uint32_t i,void*key){ (void)self;(void)i; if(key) memset(key,0,20); return HR_OK; }
static int MSABI waps_GetValue(void*self,const void*key,uint8_t*pv){ (void)self;(void)key; if(!pv) return WA_E_POINTER; memset(pv,0,24);
    *(uint16_t*)pv=31; *(uint16_t**)(pv+8)=wa_wstr("Mac speakers (CoreAudio)"); return HR_OK; }
static int MSABI waps_SetValue(void*self,const void*key,const void*pv){ (void)self;(void)key;(void)pv; return HR_OK; }
static int MSABI waps_Commit(void*self){ (void)self; return HR_OK; }
static void* g_wa_props_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)waps_GetCount,(void*)waps_GetAt,(void*)waps_GetValue,(void*)waps_SetValue,(void*)waps_Commit };

// ---- the enumerator + collections ------------------------------------------------------------------------------------
static ComObj g_wa_col1, g_wa_col0;
static int MSABI wacol_GetCount(void*self,unsigned*n){ if(n)*n = self==&g_wa_col1 ? 1 : 0; return HR_OK; }
static int MSABI wacol_Item(void*self,unsigned i,void**pp){ if(!pp) return WA_E_POINTER; if(self==&g_wa_col1 && i==0){ *pp=&g_wa_dev; return HR_OK; } *pp=0; return HR_INVALIDARG; }
static void* g_wa_col_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)wacol_GetCount,(void*)wacol_Item };
static int MSABI wae_Enum(void*self,int df,unsigned mask,void**pp){ (void)self;(void)mask; if(pp)*pp = (df==0||df==2) ? &g_wa_col1 : &g_wa_col0; return HR_OK; }   // eRender / eAll
static int MSABI wae_Default(void*self,int df,int role,void**pp){ (void)self;(void)role; if(!pp) return WA_E_POINTER;
    if(df==0){ *pp=&g_wa_dev; wa_log("  [wasapi] default render endpoint\n"); return HR_OK; } *pp=0; return HR_NOTFOUND; }   // no capture device
static int MSABI wae_GetDevice(void*self,const uint16_t*id,void**pp){ (void)self;(void)id; if(pp)*pp=&g_wa_dev; return HR_OK; }
static int MSABI wae_RegCB(void*self,void*c){ (void)self;(void)c; return HR_OK; }
static void* g_wa_enum_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)wae_Enum,(void*)wae_Default,(void*)wae_GetDevice,(void*)wae_RegCB,(void*)wae_RegCB };
static ComObj g_wa_enum;
static void* wasapi_enumerator(void){
    if(!g_wa_enum.vtbl){ g_wa_dev.vtbl=g_wa_dev_vtbl; g_wa_ep.vtbl=g_wa_ep_vtbl; g_wa_props.vtbl=g_wa_props_vtbl;
        g_wa_col1.vtbl=g_wa_col_vtbl; g_wa_col0.vtbl=g_wa_col_vtbl; g_wa_enum.vtbl=g_wa_enum_vtbl; }
    return &g_wa_enum; }
