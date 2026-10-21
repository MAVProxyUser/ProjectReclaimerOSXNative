// winmm.c -- waveOut on CoreAudio (2026-10-20). MCC's Miles Sound System for Halo 2 (halo2\mss64.dll) plays through winmm's
// waveOut; every wave function was a logging stub (and then a "no driver" answer), so its digital driver never opened.
//   waveOutOpen: PCM 8/16/24/32-bit, IEEE float 32, WAVE_FORMAT_EXTENSIBLE; WAVE_MAPPER or device 0; WAVE_FORMAT_QUERY;
//     callbacks CALLBACK_NULL (the caller polls WHDR_DONE), CALLBACK_EVENT (SetEvent), CALLBACK_FUNCTION (WOM_OPEN / WOM_DONE /
//     WOM_CLOSE; WOM_DONE runs on a dispatcher thread made with our CreateThread, as guest code needs a TEB).
//   waveOutWrite: each WAVEHDR's data is copied into an AudioQueue buffer; when it has played, the header gets WHDR_DONE (and
//     loses WHDR_INQUEUE) and the callback runs. waveOutReset returns every queued header as done; Pause / Restart; GetPosition
//     (bytes / samples / ms played); GetDevCaps (one device, "Mac audio output"); GetID 0. GetNumDevs stays 0 (see below).
//   PELOAD_AUDIO=0 (test runs) -- or an AudioQueue that won't open -- uses a silent device that returns headers at the format's
//     real playback rate. PELOAD_WINMM_STUB=1 = the old "no driver". waveIn (microphone) still reports no device.
//   Diagnostics: PELOAD_TRACE_WINMM=1 logs every write, =stats a line per stream every 2 s (writes, bytes, peak sample, headers
//   played, queued); PELOAD_WINMM_DUMP=<file> appends every written buffer's PCM (%p = pid) -- silence vs nothing vs not played.
#define WO_MAGIC 0x574F7574u
enum { WHDR_DONE=1, WHDR_PREPARED=2, WHDR_INQUEUE=0x10 };
enum { WOM_OPEN=0x3BB, WOM_CLOSE=0x3BC, WOM_DONE=0x3BD };
typedef struct WOut {
    uint32_t magic; int null_dev; AudioQueueRef q; pthread_mutex_t m; pthread_cond_t cv;
    uint32_t cbtype; uint64_t cb, inst; uint32_t rate, block, gen; int closing, started, paused;
    uint8_t*inq[1024]; int nin;            // queued headers, oldest first
    uint8_t*doneq[1024]; int nd;           // finished headers waiting for a CALLBACK_FUNCTION dispatch
    uint64_t played;                       // bytes returned as done
    uint32_t bits; double st_t0; unsigned long st_writes, st_done; uint64_t st_bytes; int st_peak;   // PELOAD_TRACE_WINMM=stats
} WOut;
typedef struct { WOut*w; uint8_t*hdr; uint32_t gen; } WoSlot;
typedef void (MSABI *WoProc)(void*,uint32_t,uint64_t,uint64_t,uint64_t);
static int wo_trace(void){ static int t=-1; if(t<0){ const char*e=getenv("PELOAD_TRACE_WINMM"); t=(e&&!strcmp(e,"stats"))?2:(e&&*e&&strcmp(e,"0"))?1:0; } return t; }
#define WO_T(...) do{ if(wo_trace()==1){ static int _n; if(_n++<200){ char _m[200]; int _k=snprintf(_m,sizeof _m,"  [winmm] " __VA_ARGS__); (void)!write(2,_m,_k); } } }while(0)
static void wo_stats(WOut*w,const void*data,uint32_t len){   // (PELOAD_TRACE_WINMM=stats / PELOAD_WINMM_DUMP)
    static FILE*dump; static int dumpinit; if(!dumpinit){ dumpinit=1; const char*e=getenv("PELOAD_WINMM_DUMP"); if(e&&*e){ char p[512]; const char*q=strstr(e,"%p");
            if(q) snprintf(p,sizeof p,"%.*s%d%s",(int)(q-e),e,(int)getpid(),q+2); else snprintf(p,sizeof p,"%s",e); dump=fopen(p,"ab"); } }
    if(dump && data && len){ fwrite(data,1,len,dump); fflush(dump); }
    if(wo_trace()!=2) return;
    w->st_writes++; w->st_bytes+=len;
    if(data && w->bits==16){ const int16_t*a=data; for(uint32_t i=0;i<len/2;i+=7){ int v=a[i]<0?-a[i]:a[i]; if(v>w->st_peak) w->st_peak=v; } }
    double now=mono_s(); if(!w->st_t0) w->st_t0=now;
    if(now-w->st_t0>=2.0){ char m[200]; int k=snprintf(m,sizeof m,"  [winmm] %p 2 s: %lu writes, %llu bytes, peak %d, %lu played, %d queued%s\n",(void*)w,w->st_writes,
            (unsigned long long)w->st_bytes,w->st_peak,w->st_done,w->nin,w->null_dev?" (silent device)":""); (void)!write(2,m,k);
        w->st_t0=now; w->st_writes=0; w->st_bytes=0; w->st_peak=0; w->st_done=0; } }
static WOut* wo_get(void*h){ WOut*w=h; return (w && (uintptr_t)w>0x10000 && w->magic==WO_MAGIC)?w:0; }
// one header has played (w->m held): flags, position, notification
static void wo_complete_l(WOut*w,uint8_t*hdr){
    for(int i=0;i<w->nin;i++) if(w->inq[i]==hdr){ memmove(w->inq+i,w->inq+i+1,(size_t)(w->nin-i-1)*sizeof(void*)); w->nin--; break; }
    uint32_t*fl=(uint32_t*)(hdr+24); __atomic_store_n(fl,(*fl|WHDR_DONE)&~(uint32_t)WHDR_INQUEUE,__ATOMIC_RELEASE);
    w->played+=*(uint32_t*)(hdr+8); w->st_done++;
    if(w->cbtype==0x50000 && w->cb) i_SetEvent((void*)w->cb);                                  // CALLBACK_EVENT
    else if(w->cbtype==0x30000 && w->cb && w->nd<1024){ w->doneq[w->nd++]=hdr; pthread_cond_broadcast(&w->cv); } }   // CALLBACK_FUNCTION
static uint32_t MSABI wo_dispatch(void*a){ WOut*w=a;   // guest thread: runs the CALLBACK_FUNCTION for every finished header
    WO_T("dispatch thread for %p running\n",(void*)w);
    for(;;){ pthread_mutex_lock(&w->m); while(!w->nd && !w->closing) pthread_cond_wait(&w->cv,&w->m);
        if(!w->nd && w->closing){ pthread_mutex_unlock(&w->m); return 0; }
        uint8_t*h=w->doneq[0]; memmove(w->doneq,w->doneq+1,(size_t)(w->nd-1)*sizeof(void*)); w->nd--; pthread_mutex_unlock(&w->m);
        WO_T("WOM_DONE %p hdr %p -> callback %p\n",(void*)w,h,(void*)w->cb); ((WoProc)w->cb)(w,WOM_DONE,w->inst,(uint64_t)h,0); WO_T("WOM_DONE %p returned\n",(void*)w); } }
static void wo_aq_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef b){ WoSlot*s=b->mUserData; WOut*w=s?s->w:0;
    if(w){ pthread_mutex_lock(&w->m); if(s->gen==w->gen && !w->closing) wo_complete_l(w,s->hdr); pthread_mutex_unlock(&w->m); }
    free(s); b->mUserData=0; AudioQueueFreeBuffer(q,b); (void)ud; }
static void* wo_null_thread(void*a){ WOut*w=a; pthread_setname_np("shim-waveout-null");   // the silent device: real-time pacing
    for(;;){ pthread_mutex_lock(&w->m); while(!w->closing && (!w->nin || w->paused)) pthread_cond_wait(&w->cv,&w->m);
        if(w->closing){ pthread_mutex_unlock(&w->m); return 0; }
        uint8_t*h=w->inq[0]; uint32_t g=w->gen; double secs=(double)*(uint32_t*)(h+8)/((double)w->block*w->rate); pthread_mutex_unlock(&w->m);
        if(secs>0) usleep((useconds_t)(secs*1e6));
        pthread_mutex_lock(&w->m); if(g==w->gen && w->nin && w->inq[0]==h) wo_complete_l(w,h); pthread_mutex_unlock(&w->m); } }
// WAVEFORMATEX -> AudioStreamBasicDescription (0 = not supported)
static int wo_fmt(const uint8_t*f,AudioStreamBasicDescription*d){ if(!f) return 0;
    uint16_t tag=*(const uint16_t*)f, ch=*(const uint16_t*)(f+2), blk=*(const uint16_t*)(f+12), bits=*(const uint16_t*)(f+14); uint32_t rate=*(const uint32_t*)(f+4);
    int isf=tag==3; if(tag==0xFFFE && *(const uint16_t*)(f+16)>=22){ uint32_t sub=*(const uint32_t*)(f+24); if(sub==3) isf=1; else if(sub!=1) return 0; }
    else if(tag!=1 && tag!=3) return 0;
    if(!ch || ch>8 || !rate || !blk || (bits!=8 && bits!=16 && bits!=24 && bits!=32) || (isf && bits!=32)) return 0;
    memset(d,0,sizeof *d); d->mSampleRate=rate; d->mFormatID=kAudioFormatLinearPCM; d->mFramesPerPacket=1; d->mChannelsPerFrame=ch;
    d->mBitsPerChannel=bits; d->mBytesPerFrame=blk; d->mBytesPerPacket=blk;
    d->mFormatFlags=kAudioFormatFlagIsPacked|(isf?kAudioFormatFlagIsFloat:(bits>8?kAudioFormatFlagIsSignedInteger:0)); return 1; }
static int wo_off(void){ static int o=-1; if(o<0){ const char*e=getenv("PELOAD_WINMM_STUB"); o=e&&*e&&strcmp(e,"0"); } return o; }
static uint32_t MSABI i_waveOutOpen(void**ph,uint32_t dev,const uint8_t*fmt,uint64_t cb,uint64_t inst,uint32_t fl){
    if(wo_off()){ if(ph) *ph=0; return 6; }   // MMSYSERR_NODRIVER
    if(dev!=0 && dev!=0xFFFFFFFFu) return 2;   // MMSYSERR_BADDEVICEID
    AudioStreamBasicDescription d; if(!wo_fmt(fmt,&d)) return 32;   // WAVERR_BADFORMAT
    if(fl&1) return 0;                        // WAVE_FORMAT_QUERY
    if(!ph) return 11;                        // MMSYSERR_INVALPARAM
    WOut*w=calloc(1,sizeof *w); w->magic=WO_MAGIC; pthread_mutex_init(&w->m,0); pthread_cond_init(&w->cv,0);
    w->cbtype=fl&0x70000; w->cb=cb; w->inst=inst; w->rate=(uint32_t)d.mSampleRate; w->block=d.mBytesPerFrame; w->bits=d.mBitsPerChannel;
    w->null_dev=!xa_real();
    if(!w->null_dev){ OSStatus st=AudioQueueNewOutput(&d,wo_aq_cb,w,0,0,0,&w->q); if(st || !w->q){ w->null_dev=1; w->q=0;
            char m[96]; int k=snprintf(m,sizeof m,"  [winmm] AudioQueueNewOutput failed %d: silent device\n",(int)st); (void)!write(2,m,k); } }
    if(w->null_dev){ pthread_t t; pthread_create(&t,0,wo_null_thread,w); pthread_detach(t); }
    if(w->cbtype==0x30000 && w->cb){ uint32_t tid=0; i_CreateThread(0,0,(WinThreadProc)wo_dispatch,w,0,&tid); ((WoProc)w->cb)(w,WOM_OPEN,w->inst,0,0); }
    char m[160]; int k=snprintf(m,sizeof m,"  [winmm] waveOut open: %u Hz, %u ch, %u-bit%s, callback %s%s\n",w->rate,d.mChannelsPerFrame,d.mBitsPerChannel,
        (d.mFormatFlags&kAudioFormatFlagIsFloat)?" float":"",w->cbtype==0x30000?"function":w->cbtype==0x50000?"event":w->cbtype?"other":"none",w->null_dev?" (silent)":"");
    (void)!write(2,m,k); *ph=w; return 0; }
static uint32_t MSABI i_waveOutPrepareHeader(void*h,uint8_t*hdr,uint32_t cb){ (void)cb; WO_T("prepare %p hdr %p len %u flags 0x%x\n",h,hdr,hdr?*(uint32_t*)(hdr+8):0,hdr?*(uint32_t*)(hdr+24):0);
    if(!wo_get(h)) return 5; if(!hdr) return 11;   // MMSYSERR_INVALHANDLE
    *(uint32_t*)(hdr+24)|=WHDR_PREPARED; return 0; }
static uint32_t MSABI i_waveOutUnprepareHeader(void*h,uint8_t*hdr,uint32_t cb){ (void)cb; WO_T("unprepare %p hdr %p flags 0x%x\n",h,hdr,hdr?*(uint32_t*)(hdr+24):0);
    if(!wo_get(h)) return 5; if(!hdr) return 11;
    if(*(uint32_t*)(hdr+24)&WHDR_INQUEUE) return 33;   // WAVERR_STILLPLAYING
    *(uint32_t*)(hdr+24)&=~(uint32_t)WHDR_PREPARED; return 0; }
static uint32_t MSABI i_waveOutWrite(void*h,uint8_t*hdr,uint32_t cb){ (void)cb; WOut*w=wo_get(h); if(!w) return 5; if(!hdr) return 11;
    uint32_t*fl=(uint32_t*)(hdr+24); if(!(*fl&WHDR_PREPARED)) return 34;   // WAVERR_UNPREPARED
    uint32_t len=*(uint32_t*)(hdr+8); const void*data=*(void*const*)hdr; wo_stats(w,data,len);
    pthread_mutex_lock(&w->m); if(w->nin>=1024){ pthread_mutex_unlock(&w->m); return 7; }   // MMSYSERR_NOMEM
    *fl=(*fl|WHDR_INQUEUE)&~(uint32_t)WHDR_DONE; w->inq[w->nin++]=hdr; uint32_t g=w->gen;
    if(w->null_dev){ pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m); return 0; }
    pthread_mutex_unlock(&w->m);
    AudioQueueBufferRef b=0; if(AudioQueueAllocateBuffer(w->q,len?len:4,&b) || !b){ pthread_mutex_lock(&w->m); wo_complete_l(w,hdr); pthread_mutex_unlock(&w->m); return 0; }
    if(len && data) memcpy(b->mAudioData,data,len); else memset(b->mAudioData,0,4); b->mAudioDataByteSize=len?len:4;
    WoSlot*s=malloc(sizeof *s); s->w=w; s->hdr=hdr; s->gen=g; b->mUserData=s;
    if(AudioQueueEnqueueBuffer(w->q,b,0,0)){ free(s); AudioQueueFreeBuffer(w->q,b); pthread_mutex_lock(&w->m); wo_complete_l(w,hdr); pthread_mutex_unlock(&w->m); return 0; }
    if(!w->started && !w->paused){ w->started=1; AudioQueueStart(w->q,0); }
    if(wo_trace()==1){ char m[96]; int k=snprintf(m,sizeof m,"  [winmm] write %u bytes (%d queued)\n",len,w->nin); (void)!write(2,m,k); }
    return 0; }
static uint32_t MSABI i_waveOutReset(void*h){ WO_T("reset %p\n",h); WOut*w=wo_get(h); if(!w) return 5;
    pthread_mutex_lock(&w->m); w->gen++; while(w->nin) wo_complete_l(w,w->inq[0]); pthread_mutex_unlock(&w->m);
    if(w->q){ AudioQueueReset(w->q); } return 0; }
static uint32_t MSABI i_waveOutPause(void*h){ WO_T("pause %p\n",h); WOut*w=wo_get(h); if(!w) return 5; w->paused=1; if(w->q) AudioQueuePause(w->q); return 0; }
static uint32_t MSABI i_waveOutRestart(void*h){ WO_T("restart %p\n",h); WOut*w=wo_get(h); if(!w) return 5; pthread_mutex_lock(&w->m); w->paused=0; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m);
    if(w->q && w->started) AudioQueueStart(w->q,0); return 0; }
static uint32_t MSABI i_waveOutClose(void*h){ WO_T("close %p\n",h); WOut*w=wo_get(h); if(!w) return 5;
    pthread_mutex_lock(&w->m); if(w->nin){ pthread_mutex_unlock(&w->m); return 33; }   // WAVERR_STILLPLAYING (Windows: reset first)
    w->closing=1; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m);
    if(w->q){ AudioQueueDispose(w->q,1); w->q=0; }
    if(w->cbtype==0x30000 && w->cb) ((WoProc)w->cb)(w,WOM_CLOSE,w->inst,0,0);
    w->magic=0; return 0; }   // (the struct is kept: a late AudioQueue callback or the threads may still look at it)
static uint32_t MSABI i_waveOutGetPosition(void*h,uint8_t*mt,uint32_t cb){ WOut*w=wo_get(h); WO_T("getposition %p type %u -> %llu bytes played\n",h,mt?*(uint32_t*)mt:0,w?(unsigned long long)w->played:0ULL);
    if(!w) return 5; if(!mt || cb<8) return 11;
    uint64_t by=w->played; uint32_t ty=*(uint32_t*)mt;
    if(ty==2) *(uint32_t*)(mt+4)=(uint32_t)(by/w->block); else if(ty==1) *(uint32_t*)(mt+4)=(uint32_t)(by*1000/((uint64_t)w->block*w->rate));
    else { *(uint32_t*)mt=4; *(uint32_t*)(mt+4)=(uint32_t)by; }   // TIME_BYTES
    return 0; }
// (waveOutGetNumDevs stays peload.c's 0: halo3.dll and haloreach.dll ask it before using waveOut -- they keep XAudio2 only, as
// before; Miles opens WAVE_MAPPER without asking)
static uint32_t MSABI i_waveOutGetID(void*h,uint32_t*id){ if(!wo_get(h)) return 5; if(id) *id=0; return 0; }
static uint32_t MSABI i_waveOutGetDevCapsA(uint64_t dev,uint8_t*c,uint32_t cb){ (void)dev; if(!c || cb<52) return 11; memset(c,0,52);
    *(uint16_t*)c=1; *(uint16_t*)(c+2)=1; *(uint32_t*)(c+4)=0x0100; snprintf((char*)c+8,32,"Mac audio output");
    *(uint32_t*)(c+40)=0xFFFFF; *(uint16_t*)(c+44)=2; *(uint32_t*)(c+48)=0x1|0x2|0x20; return 0; }   // WAVECAPS_VOLUME|LRVOLUME|SAMPLEACCURATE
static uint32_t MSABI i_waveOutGetDevCapsW(uint64_t dev,uint8_t*c,uint32_t cb){ (void)dev; if(!c || cb<84) return 11; memset(c,0,84);
    *(uint16_t*)c=1; *(uint16_t*)(c+2)=1; *(uint32_t*)(c+4)=0x0100; const char*n="Mac audio output"; for(int i=0;n[i];i++) *(uint16_t*)(c+8+i*2)=(uint8_t)n[i];
    *(uint32_t*)(c+72)=0xFFFFF; *(uint16_t*)(c+76)=2; *(uint32_t*)(c+80)=0x1|0x2|0x20; return 0; }
static uint32_t MSABI i_waveOutGetVolume(void*h,uint32_t*v){ (void)h; if(v) *v=0xFFFFFFFFu; return 0; }
static uint32_t MSABI i_waveOutSetVolume(void*h,uint32_t v){ (void)h;(void)v; return 0; }
