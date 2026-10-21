// dsound.c -- DirectSound 8 on CoreAudio (2026-10-21). Included by peload.c after d2d.c (it uses that COM object base).
// Why: MCC's Miles Sound System (Halo 2) uses DirectSound when there is one and waveOut otherwise. On waveOut it keeps five 50 ms
// buffers, four of them always queued: every sound reached the speakers ~0.2 s after it was mixed (the "audio a hair behind the
// picture" in Project Gravemind). On DirectSound it mixes a short distance ahead of the play cursor of one looping buffer.
//   dsound.dll (LoadLibrary) is a handle of ours whose GetProcAddress gives DirectSoundCreate(8), DirectSoundEnumerateA/W and
//   no capture devices. IDirectSound8: CreateSoundBuffer (primary + secondary), GetCaps (software mixing), SetCooperativeLevel,
//   Get/SetSpeakerConfig (stereo), Compact, Initialize, VerifyCertification. IDirectSoundBuffer8: a ring in memory (Lock hands
//   out pointers into it, Unlock has nothing to do), Play (looping or once) / Stop / Get-SetCurrentPosition / GetStatus /
//   Get-SetFormat / Volume (an AudioQueue gain) / Pan / Frequency (kept). Each buffer that plays has an AudioQueue pulling 10 ms
//   at a time from its ring at the play cursor; the write cursor is one such step ahead. A primary buffer sounds only once the
//   program has locked it (DSSCL_WRITEPRIMARY); otherwise it just keeps the format. PELOAD_AUDIO=0: a silent device that moves
//   the cursors in real time. Anything else logs once ("[d2d] IDirectSoundBuffer8::... not implemented") and says E_NOTIMPL.
//   Per game (games.c): on for h2. halo3.dll and haloreach.dll also look for DirectSound and have only ever been told there is
//   none -- they keep that. PELOAD_DSOUND=1 = on for any game, =0 = off (Miles then uses waveOut, winmm.c).
#define DS_HANDLE ((void*)0x7735000ULL)
static const GUID_ IID_IDirectSound={0x279afa83,0x4981,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}};
static const GUID_ IID_IDirectSound8={0xc50a7e93,0xf395,0x4834,{0x9e,0xf6,0x7f,0xa9,0x9d,0xe5,0x09,0x66}};
static const GUID_ IID_IDirectSoundBuffer={0x279afa85,0x4981,0x11ce,{0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60}};
static const GUID_ IID_IDirectSoundBuffer8={0x6825a449,0x7524,0x4d82,{0x92,0x0f,0x50,0xe3,0x6a,0xb3,0xab,0x1e}};
static const char*const SLOTS_IDirectSound8[12]={"QueryInterface","AddRef","Release","CreateSoundBuffer","GetCaps","DuplicateSoundBuffer",
    "SetCooperativeLevel","Compact","GetSpeakerConfig","SetSpeakerConfig","Initialize","VerifyCertification"};
static const char*const SLOTS_IDirectSoundBuffer8[24]={"QueryInterface","AddRef","Release","GetCaps","GetCurrentPosition","GetFormat","GetVolume",
    "GetPan","GetFrequency","GetStatus","Initialize","Lock","Play","SetCurrentPosition","SetFormat","SetVolume","SetPan","SetFrequency","Stop",
    "Unlock","Restore","SetFX","AcquireResources","GetObjectInPath"};
#define DSERR_INVALIDPARAM 0x80070057
#define DSERR_INVALIDCALL 0x88780032
#define DSERR_NODRIVER 0x88780078
typedef struct { D2Hdr h; int coop; } DsDev;
typedef struct { D2Hdr h; int primary; uint8_t*ring; uint32_t size; uint8_t wfx[64]; uint32_t rate, ch, bits, block, flags;
    volatile uint32_t play; volatile int playing, looping; int32_t vol, pan; uint32_t freq; int written, null, started, closing;
    AudioQueueRef q; uint32_t step; pthread_mutex_t m; double mt0; uint64_t mbytes; int mpeak; } DsBuf;
static D2Cls C_dsdev, C_dsbuf;
static int ds_trace(void){ static int t=-1; if(t<0) t=getenv("PELOAD_TRACE_DSOUND")?1:0; return t; }
// WAVEFORMATEX(-TENSIBLE) -> the buffer's format (PCM 8/16/24/32-bit or float 32)
static int ds_setfmt(DsBuf*b,const uint8_t*w){ if(!w) return 0; uint16_t tag=*(const uint16_t*)w, ch=*(const uint16_t*)(w+2), blk=*(const uint16_t*)(w+12), bits=*(const uint16_t*)(w+14);
    uint32_t rate=*(const uint32_t*)(w+4); uint16_t cb=tag==1?0:*(const uint16_t*)(w+16); if(!ch||ch>8||!rate||!blk) return 0;
    if(tag!=1 && tag!=3 && tag!=0xFFFE) return 0;
    size_t n=18+(size_t)cb; if(n>sizeof b->wfx) n=sizeof b->wfx; memset(b->wfx,0,sizeof b->wfx); memcpy(b->wfx,w,tag==1?16:n);
    b->rate=rate; b->ch=ch; b->bits=bits; b->block=blk; b->freq=rate; return 1; }
// PELOAD_TRACE_DSOUND: a line per playing buffer every 2 s -- bytes played and the peak sample (16-bit) of what was played
static void ds_meter(DsBuf*b,const uint8_t*p,uint32_t n){ if(!ds_trace()) return; b->mbytes+=n;
    if(b->bits==16) for(uint32_t i=0;i+1<n;i+=14){ int v=*(const int16_t*)(p+i); if(v<0) v=-v; if(v>b->mpeak) b->mpeak=v; }
    double now=mono_s(); if(!b->mt0) b->mt0=now;
    if(now-b->mt0>=2.0){ char m[140]; int k=snprintf(m,sizeof m,"  [dsound] %p 2 s: %llu bytes played, peak %d, cursor %u%s\n",(void*)b,(unsigned long long)b->mbytes,b->mpeak,b->play,b->null?" (silent device)":"");
        (void)!write(2,m,k); b->mt0=now; b->mbytes=0; b->mpeak=0; } }
static void ds_aq_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef ab){ DsBuf*b=ud; uint8_t*o=ab->mAudioData; uint32_t n=b->step*b->block, done=0;
    if(!b->closing && b->playing && b->ring && b->size){ uint32_t p=__atomic_load_n(&b->play,__ATOMIC_ACQUIRE);
        while(done<n){ uint32_t k=b->size-p; if(k>n-done) k=n-done; memcpy(o+done,b->ring+p,k); done+=k; p+=k;
            if(p>=b->size){ p=0; if(!b->looping){ b->playing=0; break; } } }
        __atomic_store_n(&b->play,p,__ATOMIC_RELEASE); }
    if(done<n) memset(o+done,(b->bits==8)?0x80:0,n-done);
    ds_meter(b,o,n);
    ab->mAudioDataByteSize=n; if(!b->closing) AudioQueueEnqueueBuffer(q,ab,0,0); }
static void* ds_null_thread(void*a){ DsBuf*b=a; pthread_setname_np("shim-dsound-null");   // PELOAD_AUDIO=0: the cursors move in real time
    double t=mono_s(), owed=0;   // (by the clock: usleep oversleeps, and a slow cursor slows the program's audio clock too)
    while(!b->closing){ usleep(10000); double now=mono_s(); owed+=(now-t)*b->rate; t=now; if(!b->playing || !b->size){ owed=0; continue; }
        uint32_t frames=(uint32_t)owed; owed-=frames; uint32_t adv=frames*b->block; if(adv>=b->size) adv=b->size-b->block; uint32_t p=b->play+adv;
        if(ds_trace() && b->ring){ uint32_t a=b->play, k=adv<b->size-a?adv:b->size-a; ds_meter(b,b->ring+a,k); }
        if(p>=b->size){ if(b->looping) p%=b->size; else { p=0; b->playing=0; } } __atomic_store_n(&b->play,p,__ATOMIC_RELEASE); }
    return 0; }
static void ds_start(DsBuf*b){ if(b->started) return; b->started=1; b->step=b->rate/100; if(b->step<64) b->step=64;
    if(!xa_real()){ b->null=1; pthread_t t; pthread_create(&t,0,ds_null_thread,b); pthread_detach(t); return; }
    AudioStreamBasicDescription d; if(!wo_fmt(b->wfx,&d)){ b->null=1; pthread_t t; pthread_create(&t,0,ds_null_thread,b); pthread_detach(t); return; }
    if(AudioQueueNewOutput(&d,ds_aq_cb,b,0,0,0,&b->q) || !b->q){ b->q=0; b->null=1; pthread_t t; pthread_create(&t,0,ds_null_thread,b); pthread_detach(t);
        (void)!write(2,"  [dsound] AudioQueueNewOutput failed: silent buffer\n",53); return; }
    for(int i=0;i<3;i++){ AudioQueueBufferRef ab=0; if(!AudioQueueAllocateBuffer(b->q,b->step*b->block,&ab) && ab) ds_aq_cb(b,b->q,ab); }
    if(b->vol) AudioQueueSetParameter(b->q,kAudioQueueParam_Volume,(float)pow(10.0,b->vol/2000.0));
    AudioQueueStart(b->q,0);
    if(ds_trace()){ char m[160]; int k=snprintf(m,sizeof m,"  [dsound] buffer %p plays: %u Hz %u ch %u-bit, %u bytes, %s\n",(void*)b,b->rate,b->ch,b->bits,b->size,b->primary?"primary":"secondary"); (void)!write(2,m,k); } }
static void ds_buf_dtor(void*p){ DsBuf*b=p; b->closing=1; if(b->q){ AudioQueueStop(b->q,1); AudioQueueDispose(b->q,1); b->q=0; }
    if(b->null) usleep(20000);   // (the silent device's thread sees closing within 10 ms)
    free(b->ring); }
// ---- IDirectSoundBuffer8
static int MSABI dsb_GetCaps(void*s,uint32_t*c){ DsBuf*b=s; if(!c || c[0]<20) return (int)DSERR_INVALIDPARAM; c[1]=b->flags; c[2]=b->size; c[3]=0; c[4]=0; return 0; }
static int MSABI dsb_GetCurrentPosition(void*s,uint32_t*pp,uint32_t*pw){ DsBuf*b=s; uint32_t p=__atomic_load_n(&b->play,__ATOMIC_ACQUIRE);
    if(pp)*pp=p; if(pw){ uint32_t w=b->size?(p+(b->step?b->step:b->rate/100)*b->block)%b->size:0; *pw=b->playing?w:p; } return 0; }
static int MSABI dsb_GetFormat(void*s,uint8_t*w,uint32_t sz,uint32_t*got){ DsBuf*b=s; uint16_t tag=*(uint16_t*)b->wfx; uint32_t n=tag==1?16:18+*(uint16_t*)(b->wfx+16);
    if(n>sizeof b->wfx) n=sizeof b->wfx; if(got)*got=n; if(w){ if(sz<n) n=sz; memcpy(w,b->wfx,n); } return 0; }
static int MSABI dsb_GetVolume(void*s,int32_t*v){ if(v)*v=((DsBuf*)s)->vol; return 0; }
static int MSABI dsb_GetPan(void*s,int32_t*v){ if(v)*v=((DsBuf*)s)->pan; return 0; }
static int MSABI dsb_GetFrequency(void*s,uint32_t*v){ if(v)*v=((DsBuf*)s)->freq; return 0; }
static int MSABI dsb_GetStatus(void*s,uint32_t*st){ DsBuf*b=s; if(st)*st=(b->playing?1:0)|(b->playing&&b->looping?4:0); return 0; }
static int MSABI dsb_Initialize(void*s,void*ds,const void*desc){ (void)s;(void)ds;(void)desc; return (int)0x88780082; }   // DSERR_ALREADYINITIALIZED
static int MSABI dsb_Lock(void*s,uint32_t off,uint32_t n,void**p1,uint32_t*n1,void**p2,uint32_t*n2,uint32_t fl){ DsBuf*b=s;
    if(!b->ring && b->primary){ b->size=b->block*(b->rate/2); if(!b->size) return (int)DSERR_INVALIDCALL; b->ring=calloc(1,b->size); }   // (a primary written to: half a second)
    if(!b->ring || !b->size) return (int)DSERR_INVALIDCALL;
    if(fl&1){ uint32_t w; dsb_GetCurrentPosition(b,0,&w); off=w; } if(fl&2) n=b->size;
    if(off>=b->size || n>b->size) return (int)DSERR_INVALIDPARAM;
    uint32_t a=b->size-off; if(a>n) a=n; if(p1)*p1=b->ring+off; if(n1)*n1=a;
    if(p2)*p2=n>a?b->ring:0; if(n2)*n2=n>a?n-a:0; b->written=1;
    if(b->primary && b->playing && !b->started) ds_start(b);   // (a primary played before it was first written)
    return 0; }
static int MSABI dsb_Play(void*s,uint32_t r1,uint32_t prio,uint32_t fl){ (void)r1;(void)prio; DsBuf*b=s;
    b->looping=(fl&1)!=0; if(b->primary && !b->written){ b->playing=1; return 0; }   // (the primary: playing = the device runs)
    b->playing=1; ds_start(b); return 0; }
static int MSABI dsb_SetCurrentPosition(void*s,uint32_t p){ DsBuf*b=s; if(b->size && p>=b->size) return (int)DSERR_INVALIDPARAM; __atomic_store_n(&b->play,p,__ATOMIC_RELEASE); return 0; }
static int MSABI dsb_SetFormat(void*s,const uint8_t*w){ DsBuf*b=s; if(!b->primary) return (int)DSERR_INVALIDCALL; if(!ds_setfmt(b,w)) return (int)DSERR_INVALIDPARAM;
    if(ds_trace()){ char m[100]; int k=snprintf(m,sizeof m,"  [dsound] primary format %u Hz %u ch %u-bit\n",b->rate,b->ch,b->bits); (void)!write(2,m,k); } return 0; }
static int MSABI dsb_SetVolume(void*s,int32_t v){ DsBuf*b=s; if(v>0||v<-10000) return (int)DSERR_INVALIDPARAM; b->vol=v;
    if(b->q) AudioQueueSetParameter(b->q,kAudioQueueParam_Volume,(float)pow(10.0,v/2000.0)); return 0; }
static int MSABI dsb_SetPan(void*s,int32_t v){ DsBuf*b=s; if(v<-10000||v>10000) return (int)DSERR_INVALIDPARAM; b->pan=v;
    if(b->q){ float pn=v<0?-(1.0f-(float)pow(10.0,v/2000.0)):(1.0f-(float)pow(10.0,-v/2000.0)); AudioQueueSetParameter(b->q,kAudioQueueParam_Pan,pn); } return 0; }
static int MSABI dsb_SetFrequency(void*s,uint32_t f){ DsBuf*b=s; b->freq=f?f:b->rate; return 0; }   // (kept; playback stays at the buffer's rate)
static int MSABI dsb_Stop(void*s){ ((DsBuf*)s)->playing=0; return 0; }
static int MSABI dsb_Unlock(void*s,void*p1,uint32_t n1,void*p2,uint32_t n2){ (void)s;(void)p1;(void)n1;(void)p2;(void)n2; return 0; }
static int MSABI dsb_Restore(void*s){ (void)s; return 0; }
static void ds_setup_buf(void**v){ v[3]=(void*)dsb_GetCaps; v[4]=(void*)dsb_GetCurrentPosition; v[5]=(void*)dsb_GetFormat; v[6]=(void*)dsb_GetVolume; v[7]=(void*)dsb_GetPan;
    v[8]=(void*)dsb_GetFrequency; v[9]=(void*)dsb_GetStatus; v[10]=(void*)dsb_Initialize; v[11]=(void*)dsb_Lock; v[12]=(void*)dsb_Play; v[13]=(void*)dsb_SetCurrentPosition;
    v[14]=(void*)dsb_SetFormat; v[15]=(void*)dsb_SetVolume; v[16]=(void*)dsb_SetPan; v[17]=(void*)dsb_SetFrequency; v[18]=(void*)dsb_Stop; v[19]=(void*)dsb_Unlock; v[20]=(void*)dsb_Restore; }
// ---- IDirectSound8
static int MSABI ds_CreateSoundBuffer(void*s,const uint8_t*desc,void**pp,void*outer){ (void)s;(void)outer; if(!desc||!pp) return (int)DSERR_INVALIDPARAM;
    uint32_t fl=*(const uint32_t*)(desc+4), bytes=*(const uint32_t*)(desc+8); const uint8_t*w=*(const uint8_t*const*)(desc+16);
    DsBuf*b=d2_alloc(&C_dsbuf,sizeof(DsBuf)); b->flags=fl|0x8/*LOCSOFTWARE*/; b->primary=(fl&1)!=0; pthread_mutex_init(&b->m,0);
    if(b->primary){ static const uint8_t def[16]={1,0,2,0,0x22,0x56,0,0,0x88,0x58,1,0,2,0,8,0}; ds_setfmt(b,def); }   // (22050 Hz 8-bit stereo, Windows' default)
    else { if(!ds_setfmt(b,w) || bytes<4 || bytes>(64u<<20)){ d2_Release(b); *pp=0; return (int)DSERR_INVALIDPARAM; }
        b->size=bytes-bytes%b->block; b->ring=calloc(1,b->size); if(b->bits==8) memset(b->ring,0x80,b->size); }
    if(ds_trace()){ char m[160]; int k=snprintf(m,sizeof m,"  [dsound] CreateSoundBuffer %s flags 0x%x, %u bytes, %u Hz %u ch %u-bit\n",b->primary?"primary":"secondary",fl,b->size,b->rate,b->ch,b->bits); (void)!write(2,m,k); }
    *pp=b; return 0; }
static int MSABI ds_GetCaps(void*s,uint32_t*c){ (void)s; if(!c || c[0]<96) return (int)DSERR_INVALIDPARAM; memset(c+1,0,92);
    c[1]=0x1|0x2|0x4|0x8|0x10|0x100|0x200|0x400|0x800; c[2]=100; c[3]=200000; c[4]=1; return 0; }   // software mixing only
static int MSABI ds_SetCooperativeLevel(void*s,void*hwnd,uint32_t lvl){ (void)hwnd; ((DsDev*)s)->coop=(int)lvl; return 0; }
static int MSABI ds_ok(void*s){ (void)s; return 0; }
static int MSABI ds_GetSpeakerConfig(void*s,uint32_t*c){ (void)s; if(!c) return (int)DSERR_INVALIDPARAM; *c=4u|(20u<<16); return 0; }   // DSSPEAKER_STEREO, wide
static int MSABI ds_SetSpeakerConfig(void*s,uint32_t c){ (void)s;(void)c; return 0; }
static int MSABI ds_Initialize(void*s,const void*g){ (void)s;(void)g; return 0; }
static int MSABI ds_VerifyCertification(void*s,uint32_t*c){ (void)s; if(c)*c=0; return 0; }   // DS_CERTIFIED
static void ds_setup_dev(void**v){ v[3]=(void*)ds_CreateSoundBuffer; v[4]=(void*)ds_GetCaps; v[6]=(void*)ds_SetCooperativeLevel; v[7]=(void*)ds_ok;
    v[8]=(void*)ds_GetSpeakerConfig; v[9]=(void*)ds_SetSpeakerConfig; v[10]=(void*)ds_Initialize; v[11]=(void*)ds_VerifyCertification; }
static D2Cls C_dsdev={"IDirectSound8",SLOTS_IDirectSound8,12,{&IID_IUnknown,&IID_IDirectSound,&IID_IDirectSound8},0,ds_setup_dev};
static D2Cls C_dsbuf={"IDirectSoundBuffer8",SLOTS_IDirectSoundBuffer8,24,{&IID_IUnknown,&IID_IDirectSoundBuffer,&IID_IDirectSoundBuffer8},ds_buf_dtor,ds_setup_buf};
// ---- exports
static int ds_on(void){ return game_default_on("PELOAD_DSOUND",GAME_H2); }
static int MSABI i_DirectSoundCreate8(const GUID_*dev,void**pp,void*outer){ (void)dev;(void)outer; if(!pp) return (int)DSERR_INVALIDPARAM;
    *pp=d2_alloc(&C_dsdev,sizeof(DsDev)); if(ds_trace()) (void)!write(2,"  [dsound] DirectSoundCreate\n",29); return 0; }
static int MSABI i_DirectSoundCreate(const GUID_*dev,void**pp,void*outer){ return i_DirectSoundCreate8(dev,pp,outer); }
static const GUID_ g_ds_devguid={0x5ae0c0de,0x2fa1,0x4e6a,{0x9c,0x2b,0x6d,0x61,0x63,0x00,0x00,0x01}};   // "Mac audio output"
static int MSABI i_DirectSoundEnumerateA(void*cb,void*ctx){ if(!cb) return (int)DSERR_INVALIDPARAM; typedef int(MSABI*Cb)(const GUID_*,const char*,const char*,void*);
    if(((Cb)cb)(0,"Primary Sound Driver","",ctx)) ((Cb)cb)(&g_ds_devguid,"Mac audio output","",ctx); return 0; }
static int MSABI i_DirectSoundEnumerateW(void*cb,void*ctx){ if(!cb) return (int)DSERR_INVALIDPARAM; typedef int(MSABI*Cb)(const GUID_*,const uint16_t*,const uint16_t*,void*);
    static const uint16_t p[]={'P','r','i','m','a','r','y',' ','S','o','u','n','d',' ','D','r','i','v','e','r',0}, m[]={'M','a','c',' ','a','u','d','i','o',' ','o','u','t','p','u','t',0}, e[]={0};
    if(((Cb)cb)(0,p,e,ctx)) ((Cb)cb)(&g_ds_devguid,m,e,ctx); return 0; }
static int MSABI i_DirectSoundCaptureCreate(const GUID_*dev,void**pp,void*outer){ (void)dev;(void)outer; if(pp)*pp=0; return (int)DSERR_NODRIVER; }
static int MSABI i_DirectSoundCaptureEnumerate(void*cb,void*ctx){ (void)cb;(void)ctx; return 0; }
static int MSABI i_DS_GetDeviceID(const GUID_*in,GUID_*out){ (void)in; if(!out) return (int)DSERR_INVALIDPARAM; *out=g_ds_devguid; return 0; }
static int MSABI i_DS_DllCanUnloadNow(void){ return 1; }   // S_FALSE
static void* ds_resolve(const char*n){ if(!n) return 0;
    static const struct { const char*n; void*f; } T[]={{"DirectSoundCreate",(void*)i_DirectSoundCreate},{"DirectSoundCreate8",(void*)i_DirectSoundCreate8},
        {"DirectSoundEnumerateA",(void*)i_DirectSoundEnumerateA},{"DirectSoundEnumerateW",(void*)i_DirectSoundEnumerateW},
        {"DirectSoundCaptureCreate",(void*)i_DirectSoundCaptureCreate},{"DirectSoundCaptureCreate8",(void*)i_DirectSoundCaptureCreate},
        {"DirectSoundCaptureEnumerateA",(void*)i_DirectSoundCaptureEnumerate},{"DirectSoundCaptureEnumerateW",(void*)i_DirectSoundCaptureEnumerate},
        {"GetDeviceID",(void*)i_DS_GetDeviceID},{"DllCanUnloadNow",(void*)i_DS_DllCanUnloadNow}};
    for(size_t i=0;i<sizeof T/sizeof T[0];i++) if(!strcmp(T[i].n,n)) return T[i].f; return 0; }
// PELOAD_DSOUND_SELFTEST: a looping secondary buffer through the real output path at volume -10000 (nothing is heard), 1.5 s:
// the play cursor must advance at the sample rate (shim/tools/dsound-test.sh). No game.
static int ds_selftest(void){ void*ds=0,*b=0; i_DirectSoundCreate8(0,&ds,0);
    uint8_t wfx[18]={1,0,2,0,0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,0,0}; uint8_t desc[40]={0}; *(uint32_t*)desc=40; *(uint32_t*)(desc+4)=0x80|0x8000; *(uint32_t*)(desc+8)=44100*4/2;
    *(uint8_t**)(desc+16)=wfx; int hr=((int(MSABI*)(void*,const uint8_t*,void**,void*))VT(ds,3))(ds,desc,&b,0); if(hr<0||!b){ printf("FAIL CreateSoundBuffer 0x%08x\n",(unsigned)hr); return 1; }
    void*p1=0,*p2=0; uint32_t n1=0,n2=0; ((int(MSABI*)(void*,uint32_t,uint32_t,void**,uint32_t*,void**,uint32_t*,uint32_t))VT(b,11))(b,0,0,&p1,&n1,&p2,&n2,2);
    for(uint32_t i=0;i<n1/4;i++){ int16_t v=(int16_t)(8000*sin(i*2*M_PI*440/44100)); ((int16_t*)p1)[i*2]=v; ((int16_t*)p1)[i*2+1]=v; }
    ((int(MSABI*)(void*,void*,uint32_t,void*,uint32_t))VT(b,19))(b,p1,n1,p2,n2); ((int(MSABI*)(void*,int32_t))VT(b,15))(b,-10000);
    ((int(MSABI*)(void*,uint32_t,uint32_t,uint32_t))VT(b,12))(b,0,0,1);
    uint32_t c0=0,c1=0,w=0,st=0; double t0=mono_s(); ((int(MSABI*)(void*,uint32_t*,uint32_t*))VT(b,4))(b,&c0,&w);
    uint64_t total=0; uint32_t last=c0; while(mono_s()-t0<1.5){ usleep(20000); ((int(MSABI*)(void*,uint32_t*,uint32_t*))VT(b,4))(b,&c1,&w); total+=(c1+n1-last)%n1; last=c1; }
    double secs=mono_s()-t0, rate=(double)total/4/secs; ((int(MSABI*)(void*,uint32_t*))VT(b,9))(b,&st);
    int ok=rate>44100*0.9 && rate<44100*1.1 && (st&5)==5; DsBuf*bb=b;
    printf("%s DirectSound: cursor advanced %.0f frames/s (want 44100), status 0x%x (playing+looping 0x5), %s output\n",ok?"PASS":"FAIL",rate,st,bb->null?"silent":"CoreAudio");
    ((int(MSABI*)(void*))VT(b,18))(b); d2_release(b); d2_release(ds); return ok?0:1; }
