// wasapi.c — a WASAPI render endpoint on CoreAudio, for the Reclaimer client's OWN audio. Included by peload.c.
// Halo 3's game audio goes through XAudio2 (xaudio.c); the client itself plays menu music and voice chat through the
// Rust crate cpal 0.15 (WASAPI host): CoCreateInstance(MMDeviceEnumerator) -> GetDefaultAudioEndpoint(eRender) ->
// IMMDevice::Activate(IAudioClient) -> GetMixFormat / IsFormatSupported -> Initialize(shared, EVENTCALLBACK) ->
// SetEventHandle -> GetBufferSize -> GetService(IAudioRenderClient, IAudioClock) -> Start, then a thread that waits on
// the event and fills GetCurrentPadding..GetBuffer..ReleaseBuffer. The old stub reported no devices ("Menu music output
// unavailable: No music playback device", "Voice audio unavailable"). PELOAD_NO_WASAPI=1 brings that stub back.
// Each IAudioClient gets its own AudioQueue in exactly the format the client initialised (no conversion); its callback
// drains the stream's ring buffer every 10 ms and signals the client's event so it refills. PELOAD_AUDIO=0 (test
// players) mutes these queues too.
// Capture (2026-10-17): the Mac's microphone is the client's capture endpoint, so its voice chat starts once instead of
// retrying (and building a playback stream) every 5 s without one -- those retries were the trigger of the caulk crash.
// The client sends voice only while its talk key is held or toggled (its modes: Voice off / Mic off / Hold to talk /
// Toggle to talk). PELOAD_MIC=0: a SILENT microphone (no Mac mic opened; the client's voice chat starts once and listens);
// PELOAD_MIC=none: no capture device (the old behaviour: retries every 5 s); PELOAD_MIC=tone: a quiet 440 Hz test tone
// instead of the microphone (test clients; never opens the real one); PELOAD_MIC_MUTED=1 starts muted; the stats page's
// Mic button (POST /control?do=mic&on=0|1) mutes it -- the client then gets silence, flagged AUDCLNT_BUFFERFLAGS_SILENT.

typedef struct WaStream WaStream;
typedef struct { void**vtbl; WaStream*s; } WaItf;           // one COM interface of a stream (container: WaStream)
struct WaStream {
    WaItf ac, rc, clk;                                       // IAudioClient, IAudioRenderClient, IAudioClock
    volatile int ref, svc; pthread_mutex_t m;                // svc: references held on the render client + clock
    int inited, started; uint32_t rate, ch, bits, blk, isfloat, bufframes;
    uint8_t*ring, *scratch; uint32_t scratch_cap; uint64_t rd, wr, played;   // ring positions in frames (monotonic)
    void*event; AudioQueueRef q; int closing;      // closing: the queue is being stopped -- callbacks must not re-enqueue
    struct WaAq*aq;                                // the pooled AudioQueue this stream plays through (q = aq->q)
    int capture; WaItf cc; AudioQueueRef iq; AudioQueueBufferRef ib[3]; uint32_t packet; pthread_t tone; volatile int tone_run; double tone_ph; int silent, cap_idle; };   // capture streams (silent: zeros, no device)
#define WA_E_NOINTERFACE 0x80004002
#define WA_E_POINTER     0x80004003
#define WA_E_NOT_INIT    0x88890001
#define WA_E_ALREADY     0x88890002
#define WA_E_TOO_LARGE   0x88890006
#define WA_E_UNSUPPORTED 0x88890008
static int wa_guid(const GUID_*g,uint32_t d1){ return g && g->D1==d1; }
static void wa_log(const char*s){ if(getenv("PELOAD_TRACE_WASAPI")) (void)!write(2,s,strlen(s)); }

// ---- the stream ---------------------------------------------------------------------------------------------------
// Runs on the AudioQueue's own thread. The enqueue happens under s->m and not at all once the queue is closing: a
// callback that re-enqueued a buffer while another thread disposed the queue tripped caulk's allocator check (a SIGILL
// in caulk::alloc, seen as "exception 0xC000001D at unmapped module" right after the client's 5 s voice retries).
// Every AudioQueue create / start / stop / dispose, in all streams, under one lock (callbacks never take it): the
// client's voice retries create and dispose a queue every ~5 s, and a create racing another stream's dispose still
// tripped caulk's allocator trap with per-stream locking (2026-10-17, input-less test client).
static pthread_mutex_t g_wa_lm=PTHREAD_MUTEX_INITIALIZER;
// AudioQueues are POOLED, never disposed in normal use (2026-10-17): the client's voice chat retries every ~5 s without a
// microphone and each try builds and drops a playback stream; creating and disposing an AudioQueue that often kept
// tripping caulk's allocator trap (SIGILL in caulk::alloc, "exception 0xC000001D at unmapped module") even with every
// create/dispose serialised -- in the user's own game as well as the test target. Now an idle stream hands its queue,
// stopped, with its three buffers, back to g_wa_pool, and the next stream of the same format takes it. A queue's
// callback reaches its current stream through WaAq.s under g_wa_cbmx (NULL while pooled). PELOAD_WASAPI_NOPOOL=1 =
// dispose as before.
typedef struct WaAq { AudioQueueRef q; AudioQueueBufferRef buf[3]; int nbuf; uint32_t rate, ch, bits, blk, isfloat, bytes; WaStream*s; struct WaAq*next; } WaAq;
static WaAq*g_wa_pool;                                      // idle queues (under g_wa_lm)
static pthread_mutex_t g_wa_cbmx=PTHREAD_MUTEX_INITIALIZER; // WaAq.s: callbacks vs streams binding/unbinding
static void wa_aq_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef b){
    WaAq*aq=ud; pthread_mutex_lock(&g_wa_cbmx); WaStream*s=aq->s; if(!s){ pthread_mutex_unlock(&g_wa_cbmx); return; }   // pooled: idle
    uint32_t frames=b->mAudioDataBytesCapacity/s->blk;
    pthread_mutex_lock(&s->m);
    if(s->closing || q!=s->q){ pthread_mutex_unlock(&s->m); pthread_mutex_unlock(&g_wa_cbmx); return; }
    uint64_t avail=s->wr-s->rd; uint32_t n=avail<frames?(uint32_t)avail:frames;
    for(uint32_t i=0;i<n;){ uint32_t pos=(uint32_t)((s->rd+i)%s->bufframes), run=s->bufframes-pos; if(run>n-i) run=n-i;
        memcpy((uint8_t*)b->mAudioData+(size_t)i*s->blk, s->ring+(size_t)pos*s->blk, (size_t)run*s->blk); i+=run; }
    if(n<frames) memset((uint8_t*)b->mAudioData+(size_t)n*s->blk,0,(size_t)(frames-n)*s->blk);   // underrun: silence
    if(g_rec_audio) rec_audio_push(s,b->mAudioData,frames,(int)s->ch,(int)s->rate,(int)s->bits,(int)s->blk,(int)s->isfloat);   // clip recording (rec.m): menu music, voices
    s->rd+=n; s->played+=frames; void*ev=s->event;
    // PELOAD_WASAPI_MUTE=1 (test runs): the client's own sound -- menu music, other players' voice -- consumed as usual and
    // played as silence (PELOAD_AUDIO_MUTE does the same for the game's XAudio2 sound); a clip recording still gets it
    { static int mute=-1; if(mute<0) mute=getenv("PELOAD_WASAPI_MUTE")?1:0; if(mute) memset(b->mAudioData,0,(size_t)frames*s->blk); }
    b->mAudioDataByteSize=frames*s->blk; AudioQueueEnqueueBuffer(q,b,0,0);
    pthread_mutex_unlock(&s->m); pthread_mutex_unlock(&g_wa_cbmx);
    if(ev) i_SetEvent(ev); }                                  // room in the buffer: let the client refill
// Take the queue out of the stream (one locked step: a second closer finds none), unbind it from the stream, stop it,
// and put it in the pool. The stop runs outside the stream and callback locks (it waits for a running callback).
static void wa_close_queue(WaStream*s){
    pthread_mutex_lock(&g_wa_lm);
    pthread_mutex_lock(&s->m); WaAq*aq=s->aq; s->aq=0; s->q=0; s->closing=1; s->started=0; pthread_mutex_unlock(&s->m);
    if(aq){ pthread_mutex_lock(&g_wa_cbmx); aq->s=0; pthread_mutex_unlock(&g_wa_cbmx);
        AudioQueueStop(aq->q,1);
        if(getenv("PELOAD_WASAPI_NOPOOL")){ AudioQueueDispose(aq->q,1); free(aq); }
        else { aq->next=g_wa_pool; g_wa_pool=aq; } }
    pthread_mutex_unlock(&g_wa_lm); }
// ---- capture: an input AudioQueue (or the test tone) fills the ring; the client takes 10 ms packets with GetBuffer --
// 0 none (no capture device), 1 the Mac's microphone, 2 a test tone, 3 silence. Without a capture device the client's voice
// chat rebuilds a playback stream every ~5 s, which still trips caulk's allocator trap now and then (backlog 22): so
// PELOAD_MIC=0 is a SILENT microphone (the voice chat starts once, sends nothing), and a real microphone that cannot be
// opened (permission refused) falls back to silence too. PELOAD_MIC=none = no device at all (old PELOAD_MIC=0).
static int wa_mic_mode(void){ static int m=-1; if(m<0){ const char*e=getenv("PELOAD_MIC"); m=e&&!strcmp(e,"none")?0:e&&!strcmp(e,"0")?3:e&&!strcmp(e,"tone")?2:1; } return m; }
volatile int g_mic_muted=-1;   // -1: not read yet (PELOAD_MIC_MUTED)
static int wa_mic_muted(void){ if(g_mic_muted<0) g_mic_muted=getenv("PELOAD_MIC_MUTED")?1:0; return g_mic_muted; }
static volatile int g_mic_streams;   // capture streams open (the stats page shows the mic as in use)
static void wa_cap_put(WaStream*s,const uint8_t*src,uint32_t frames){   // (s->m held) an overrun drops the oldest frames
    for(uint32_t i=0;i<frames;){ uint32_t pos=(uint32_t)((s->wr+i)%s->bufframes), run=s->bufframes-pos; if(run>frames-i) run=frames-i;
        memcpy(s->ring+(size_t)pos*s->blk,src+(size_t)i*s->blk,(size_t)run*s->blk); i+=run; }
    s->wr+=frames; if(s->wr-s->rd>s->bufframes) s->rd=s->wr-s->bufframes; }
static void wa_in_cb(void*ud,AudioQueueRef q,AudioQueueBufferRef b,const AudioTimeStamp*ts,UInt32 np,const AudioStreamPacketDescription*pd){
    (void)ts;(void)np;(void)pd; WaStream*s=ud; pthread_mutex_lock(&s->m);
    if(s->closing || q!=s->iq){ pthread_mutex_unlock(&s->m); return; }
    uint32_t frames=b->mAudioDataByteSize/s->blk; if(frames) wa_cap_put(s,b->mAudioData,frames);
    AudioQueueEnqueueBuffer(q,b,0,0); void*ev=s->event; pthread_mutex_unlock(&s->m);
    if(ev && frames) i_SetEvent(ev); }
static void* wa_tone_thread(void*a){ WaStream*s=a; pthread_setname_np("shim-mic-tone");
    uint32_t n=s->packet; uint8_t*buf=calloc(n,s->blk);
    while(s->tone_run){ usleep(10000);
        double amp=(wa_mic_mode()==2 && !s->silent)?0.05:0.0;   // silence: the same 10 ms packets, all zero
        for(uint32_t i=0;i<n;i++){ double v=amp*sin(s->tone_ph); s->tone_ph+=2*3.14159265358979*440.0/s->rate;
            for(uint32_t c=0;c<s->ch;c++){ uint8_t*o=buf+(size_t)i*s->blk+(size_t)c*(s->bits/8);
                if(s->isfloat && s->bits==32) *(float*)o=(float)v; else if(s->bits==16) *(int16_t*)o=(int16_t)(v*32767);
                else if(s->bits==32) *(int32_t*)o=(int32_t)(v*2147483647.0); else { int32_t x=(int32_t)(v*8388607); o[0]=(uint8_t)x; o[1]=(uint8_t)(x>>8); o[2]=(uint8_t)(x>>16); } } }
        pthread_mutex_lock(&s->m); if(!s->closing) wa_cap_put(s,buf,n); void*ev=s->event; pthread_mutex_unlock(&s->m); if(ev) i_SetEvent(ev); }
    free(buf); return 0; }
static int wa_cap_open(WaStream*s){   // (g_wa_lm held)
    s->packet=s->rate/100; if(!s->packet) s->packet=480;
    if(wa_mic_mode()==2 || wa_mic_mode()==3){ s->silent=wa_mic_mode()==3; __atomic_add_fetch(&g_mic_streams,1,__ATOMIC_ACQ_REL); return 1; }   // the tone thread starts with the stream
    s->silent=0;   // (a reopen after an idle close tries the real microphone again)
    AudioStreamBasicDescription f={0}; f.mSampleRate=s->rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=(s->isfloat?kAudioFormatFlagIsFloat:kAudioFormatFlagIsSignedInteger)|kAudioFormatFlagIsPacked;
    f.mFramesPerPacket=1; f.mChannelsPerFrame=s->ch; f.mBitsPerChannel=s->bits; f.mBytesPerFrame=s->blk; f.mBytesPerPacket=s->blk;
    OSStatus st=AudioQueueNewInput(&f,wa_in_cb,s,0,0,0,&s->iq);
    if(st || !s->iq){ char t[120]; int k=snprintf(t,sizeof t,"  [wasapi] microphone: AudioQueueNewInput failed %d -- a silent microphone instead\n",(int)st); (void)!write(2,t,k);
        s->iq=0; s->silent=1; __atomic_add_fetch(&g_mic_streams,1,__ATOMIC_ACQ_REL); return 1; }
    for(int i=0;i<3;i++){ AudioQueueBufferRef bf=0; if(!AudioQueueAllocateBuffer(s->iq,s->packet*s->blk,&bf)){ s->ib[i]=bf; AudioQueueEnqueueBuffer(s->iq,bf,0,0); } }
    __atomic_add_fetch(&g_mic_streams,1,__ATOMIC_ACQ_REL); return 1; }
static void wa_cap_start(WaStream*s){   // (g_wa_lm held)
    if(wa_mic_mode()==2 || s->silent){ if(!s->tone_run){ s->tone_run=1; pthread_create(&s->tone,0,wa_tone_thread,s); } }
    else if(s->iq) AudioQueueStart(s->iq,0); }
static void wa_cap_stop(WaStream*s){   // (g_wa_lm held)
    if(s->tone_run){ s->tone_run=0; pthread_join(s->tone,0); }
    if(s->iq) AudioQueuePause(s->iq); }
static void wa_cap_close(WaStream*s){
    pthread_mutex_lock(&g_wa_lm); wa_cap_stop(s);
    pthread_mutex_lock(&s->m); AudioQueueRef q=s->iq; s->iq=0; s->closing=1; pthread_mutex_unlock(&s->m);
    if(q){ AudioQueueStop(q,1); AudioQueueDispose(q,1); }
    if(s->inited && !s->cap_idle) __atomic_sub_fetch(&g_mic_streams,1,__ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&g_wa_lm); }
// IAudioCaptureClient: GetBuffer hands out the next packet (copied out of the ring; silent while muted), ReleaseBuffer
// consumes it
static int MSABI wacc_GetBuffer(WaItf*self,uint8_t**data,uint32_t*frames,uint32_t*flags,uint64_t*devpos,uint64_t*qpc){ WaStream*s=self->s;
    if(!data || !frames) return WA_E_POINTER;
    pthread_mutex_lock(&s->m); uint64_t avail=s->wr-s->rd; uint32_t n=avail<s->packet?(uint32_t)avail:s->packet;
    if(!n){ pthread_mutex_unlock(&s->m); *data=0; *frames=0; if(flags)*flags=0; return 0x08890001; }   // AUDCLNT_S_BUFFER_EMPTY
    if(n>s->scratch_cap){ free(s->scratch); s->scratch_cap=n; s->scratch=calloc(n,s->blk); }
    int mute=wa_mic_muted();
    for(uint32_t i=0;i<n;){ uint32_t pos=(uint32_t)((s->rd+i)%s->bufframes), run=s->bufframes-pos; if(run>n-i) run=n-i;
        if(mute) memset(s->scratch+(size_t)i*s->blk,0,(size_t)run*s->blk); else memcpy(s->scratch+(size_t)i*s->blk,s->ring+(size_t)pos*s->blk,(size_t)run*s->blk); i+=run; }
    if(devpos)*devpos=s->rd; pthread_mutex_unlock(&s->m);
    if(qpc)*qpc=(uint64_t)(mono_s()*1e7); *data=s->scratch; *frames=n; if(flags)*flags=mute?2:0; return HR_OK; }
static int MSABI wacc_ReleaseBuffer(WaItf*self,uint32_t n){ WaStream*s=self->s; pthread_mutex_lock(&s->m); uint64_t avail=s->wr-s->rd; s->rd+=n<avail?n:avail; pthread_mutex_unlock(&s->m); return HR_OK; }
static int MSABI wacc_GetNextPacketSize(WaItf*self,uint32_t*n){ WaStream*s=self->s; if(!n) return WA_E_POINTER;
    pthread_mutex_lock(&s->m); uint64_t avail=s->wr-s->rd; pthread_mutex_unlock(&s->m); *n=avail<s->packet?(uint32_t)avail:s->packet; return HR_OK; }
// A capture stream the client dropped (one IAudioClient reference left, like the playback streams below): stop its tone
// thread / dispose its input queue. The client opens a new one with every map load: 18 tone threads after 18 loads (2026-10-18),
// and with the real microphone as many live input queues. Reopened if the client starts it again.
static void wa_cap_idle(WaStream*s){
    pthread_mutex_lock(&g_wa_lm); if(s->cap_idle){ pthread_mutex_unlock(&g_wa_lm); return; }   // (two Releases racing here)
    wa_cap_stop(s); s->started=0;
    pthread_mutex_lock(&s->m); AudioQueueRef q=s->iq; s->iq=0; pthread_mutex_unlock(&s->m);
    if(q){ AudioQueueStop(q,1); AudioQueueDispose(q,1); }
    s->cap_idle=1; __atomic_sub_fetch(&g_mic_streams,1,__ATOMIC_ACQ_REL); pthread_mutex_unlock(&g_wa_lm); }
static void wa_free(WaStream*s){
    if(s->capture) wa_cap_close(s);
    if(s->q) wa_close_queue(s);
    free(s->ring); free(s->scratch); pthread_mutex_destroy(&s->m); free(s); wa_log("  [wasapi] stream released\n"); }
static void wa_ref_log(const char*op,WaItf*self,int r){ if(!getenv("PELOAD_TRACE_WASAPI")) return; char m[120];
    const char*w=self==&self->s->ac?"client":self==&self->s->rc?"render":self==&self->s->cc?"capture":"clock"; int k=snprintf(m,sizeof m,"  [wasapi] %s %s %p -> %d\n",op,w,(void*)self->s,r); (void)!write(2,m,k); }
// The client's voice chat retries every ~5 s while there is no microphone, and each try builds a playback stream that
// it drops with one IAudioClient reference still held: the stream is never freed. Once nothing holds its render
// client or clock any more and only that one reference is left, the stream is idle -- close its AudioQueue (no
// callbacks, no CoreAudio resources) and reopen it if the client starts the stream again.
static unsigned MSABI wa_AddRef(WaItf*self){ int r=__atomic_add_fetch(&self->s->ref,1,__ATOMIC_ACQ_REL); if(self!=&self->s->ac) __atomic_add_fetch(&self->s->svc,1,__ATOMIC_ACQ_REL);
    wa_ref_log("AddRef",self,r); return (unsigned)r; }
static unsigned MSABI wa_Release(WaItf*self){ WaStream*s=self->s; int sv=self!=&s->ac ? __atomic_sub_fetch(&s->svc,1,__ATOMIC_ACQ_REL) : s->svc;
    int r=__atomic_sub_fetch(&s->ref,1,__ATOMIC_ACQ_REL); wa_ref_log("Release",self,r); if(r==0){ wa_free(s); return 0; }
    if(r==1 && sv<=0 && s->q && s->inited && !getenv("PELOAD_WASAPI_KEEP_IDLE")){ wa_close_queue(s); wa_log("  [wasapi] stream idle: queue closed\n"); }
    if(r==1 && sv<=0 && s->capture && s->inited && !s->cap_idle && !getenv("PELOAD_WASAPI_KEEP_IDLE")){ wa_cap_idle(s); wa_log("  [wasapi] capture stream idle: closed\n"); }
    return (unsigned)(r<0?0:r); }
static int MSABI wa_QI(WaItf*self,GUID_*riid,void**ppv){ if(!ppv) return WA_E_POINTER;
    if(getenv("PELOAD_TRACE_WASAPI")){ char m[96]; int k=snprintf(m,sizeof m,"  [wasapi] QI %08x\n",riid?riid->D1:0); (void)!write(2,m,k); }
    *ppv=self; wa_AddRef(self); return HR_OK; }

// IAudioClient
static int wa_open_queue(WaStream*s){   // (g_wa_lm held)
    uint32_t bytes=(s->rate/100)*s->blk; WaAq*aq=0;
    for(WaAq**pp=&g_wa_pool;*pp;pp=&(*pp)->next){ WaAq*c=*pp;
        if(c->rate==s->rate && c->ch==s->ch && c->bits==s->bits && c->blk==s->blk && c->isfloat==s->isfloat && c->bytes==bytes){ *pp=c->next; aq=c; break; } }
    if(!aq){
        aq=calloc(1,sizeof *aq); if(!aq) return 0; aq->rate=s->rate; aq->ch=s->ch; aq->bits=s->bits; aq->blk=s->blk; aq->isfloat=s->isfloat; aq->bytes=bytes;
        AudioStreamBasicDescription f={0}; f.mSampleRate=s->rate; f.mFormatID=kAudioFormatLinearPCM;
        f.mFormatFlags=(s->isfloat?kAudioFormatFlagIsFloat:kAudioFormatFlagIsSignedInteger)|kAudioFormatFlagIsPacked;
        f.mFramesPerPacket=1; f.mChannelsPerFrame=s->ch; f.mBitsPerChannel=s->bits; f.mBytesPerFrame=s->blk; f.mBytesPerPacket=s->blk;
        OSStatus st=AudioQueueNewOutput(&f,wa_aq_cb,aq,0,0,0,&aq->q);
        if(st||!aq->q){ char t[96]; int k=snprintf(t,sizeof t,"  [wasapi] AudioQueueNewOutput failed %d\n",(int)st); (void)!write(2,t,k); free(aq); return 0; }
        if(!xa_real()) AudioQueueSetParameter(aq->q,kAudioQueueParam_Volume,0.0f);   // PELOAD_AUDIO=0: keep the clock running, play nothing
        for(int i=0;i<3;i++){ AudioQueueBufferRef bf=0; if(!AudioQueueAllocateBuffer(aq->q,bytes,&bf)) aq->buf[aq->nbuf++]=bf; }
        wa_log("  [wasapi] new AudioQueue\n"); }
    else wa_log("  [wasapi] AudioQueue reused from the pool\n");
    pthread_mutex_lock(&s->m); s->aq=aq; s->q=aq->q; s->closing=0; pthread_mutex_unlock(&s->m);
    pthread_mutex_lock(&g_wa_cbmx); aq->s=s; pthread_mutex_unlock(&g_wa_cbmx);
    for(int i=0;i<aq->nbuf;i++){ AudioQueueBufferRef bf=aq->buf[i]; memset(bf->mAudioData,0,bytes); bf->mAudioDataByteSize=bytes; AudioQueueEnqueueBuffer(aq->q,bf,0,0); }
    return 1; }
static int MSABI wac_Initialize(WaItf*self,int mode,uint32_t flags,int64_t dur,int64_t period,const uint8_t*fmt,const GUID_*session){
    (void)period;(void)session; WaStream*s=self->s; if(!fmt) return WA_E_POINTER; if(s->inited) return WA_E_ALREADY;
    uint16_t tag=*(const uint16_t*)fmt, ch=*(const uint16_t*)(fmt+2), blk=*(const uint16_t*)(fmt+12), bits=*(const uint16_t*)(fmt+14);
    uint32_t rate=*(const uint32_t*)(fmt+4); int isfloat = tag==3 || (tag==0xFFFE && *(const uint32_t*)(fmt+24)==3);
    if(!ch || ch>8 || !rate || !blk || !(bits==16||bits==24||bits==32)) return WA_E_UNSUPPORTED;
    s->rate=rate; s->ch=ch; s->bits=bits; s->blk=blk; s->isfloat=isfloat;
    uint64_t want=dur>0?(uint64_t)dur*rate/10000000ull:0; if(want<rate/10) want=rate/10; if(want>rate) want=rate;   // 100 ms .. 1 s
    s->bufframes=(uint32_t)want; s->ring=calloc(s->bufframes,blk); s->scratch_cap=s->bufframes; s->scratch=calloc(s->scratch_cap,blk);
    pthread_mutex_lock(&g_wa_lm); int ok=s->capture?wa_cap_open(s):wa_open_queue(s); pthread_mutex_unlock(&g_wa_lm); if(!ok) return WA_E_UNSUPPORTED;
    s->inited=1;
    { char t[200]; int k=snprintf(t,sizeof t,"  [wasapi] %s: %u Hz, %u ch, %u-bit %s, buffer %u frames, mode %d flags 0x%x\n",s->capture?(wa_mic_mode()==2?"microphone (test tone)":s->silent?"microphone (silent)":"microphone"):"stream",rate,ch,bits,isfloat?"float":"int",s->bufframes,mode,flags); (void)!write(2,t,k); }
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
static int MSABI wac_Start(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT;
    if(s->capture){ pthread_mutex_lock(&g_wa_lm); if(s->cap_idle){ s->cap_idle=0; wa_cap_open(s); } if(!s->started){ wa_cap_start(s); s->started=1; } pthread_mutex_unlock(&g_wa_lm); return HR_OK; }
    pthread_mutex_lock(&g_wa_lm); if(!s->q && !wa_open_queue(s)){ pthread_mutex_unlock(&g_wa_lm); return WA_E_UNSUPPORTED; }
    if(!s->started){ AudioQueueStart(s->q,0); s->started=1; if(s->event) i_SetEvent(s->event); } pthread_mutex_unlock(&g_wa_lm); return HR_OK; }
static int MSABI wac_Stop(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT;
    if(s->capture){ pthread_mutex_lock(&g_wa_lm); if(s->started) wa_cap_stop(s); s->started=0; pthread_mutex_unlock(&g_wa_lm); return HR_OK; }
    pthread_mutex_lock(&g_wa_lm); if(s->started && s->q) AudioQueuePause(s->q); s->started=0; pthread_mutex_unlock(&g_wa_lm); return HR_OK; }
static int MSABI wac_Reset(WaItf*self){ WaStream*s=self->s; if(!s->inited) return WA_E_NOT_INIT; pthread_mutex_lock(&s->m); s->rd=s->wr; pthread_mutex_unlock(&s->m); return HR_OK; }
static int MSABI wac_SetEventHandle(WaItf*self,void*h){ self->s->event=h; return HR_OK; }
static int MSABI wac_GetService(WaItf*self,GUID_*riid,void**ppv){ WaStream*s=self->s; if(!ppv) return WA_E_POINTER; *ppv=0;
    if(wa_guid(riid,0xF294ACFC)){ *ppv=&s->rc; wa_AddRef(&s->rc); return HR_OK; }    // IAudioRenderClient
    if(wa_guid(riid,0xCD63314F)){ *ppv=&s->clk; wa_AddRef(&s->clk); return HR_OK; }   // IAudioClock
    if(s->capture && wa_guid(riid,0xC8ADBD64)){ *ppv=&s->cc; wa_AddRef(&s->cc); return HR_OK; }   // IAudioCaptureClient
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
static void* g_wa_cc_vtbl[]={ (void*)wa_QI,(void*)wa_AddRef,(void*)wa_Release,(void*)wacc_GetBuffer,(void*)wacc_ReleaseBuffer,(void*)wacc_GetNextPacketSize };
static WaStream* wa_stream_new(int capture){ WaStream*s=calloc(1,sizeof *s); pthread_mutex_init(&s->m,0); s->ref=1; s->capture=capture;
    s->ac.vtbl=g_wa_ac_vtbl; s->ac.s=s; s->rc.vtbl=g_wa_rc_vtbl; s->rc.s=s; s->clk.vtbl=g_wa_clk_vtbl; s->clk.s=s; s->cc.vtbl=g_wa_cc_vtbl; s->cc.s=s; return s; }

// ---- the device: IMMDevice + IMMEndpoint + IPropertyStore (static singletons) ---------------------------------------
static ComObj g_wa_dev, g_wa_ep, g_wa_props;          // the render device (speakers)
static ComObj g_wa_cdev, g_wa_cep, g_wa_cprops;       // the capture device (microphone)
static int wa_is_cap(void*self){ return self==&g_wa_cdev || self==&g_wa_cep || self==&g_wa_cprops; }
static int MSABI wadev_QI(void*self,GUID_*riid,void**ppv){ if(!ppv) return WA_E_POINTER; int c=wa_is_cap(self);
    *ppv = wa_guid(riid,0x1BE09788) ? (void*)(c?&g_wa_cep:&g_wa_ep) : (void*)(c?&g_wa_cdev:&g_wa_dev); return HR_OK; }        // IMMEndpoint, else IMMDevice/IUnknown
static int MSABI wadev_Activate(void*self,GUID_*iid,uint32_t ctx,void*params,void**ppv){ (void)ctx;(void)params; if(!ppv) return WA_E_POINTER; *ppv=0;
    if(wa_guid(iid,0x1CB9AD4C) || wa_guid(iid,0x726778CD) || wa_guid(iid,0x7ED4EE07)){ WaStream*s=wa_stream_new(wa_is_cap(self)); *ppv=&s->ac; wa_log(s->capture?"  [wasapi] Activate IAudioClient (capture)\n":"  [wasapi] Activate IAudioClient\n"); return HR_OK; }
    return WA_E_NOINTERFACE; }
static int MSABI wadev_OpenPropertyStore(void*self,uint32_t acc,void**pp){ (void)acc; if(pp)*pp=wa_is_cap(self)?&g_wa_cprops:&g_wa_props; return HR_OK; }
static uint16_t* wa_wstr(const char*s){ size_t n=strlen(s); uint16_t*w=i_CoTaskMemAlloc((n+1)*2); for(size_t i=0;i<=n;i++) w[i]=(uint8_t)s[i]; return w; }
static int MSABI wadev_GetId(void*self,uint16_t**id){ if(id)*id=wa_wstr(wa_is_cap(self)?"{0.0.1.00000000}.{macos-coreaudio-default-capture}":"{0.0.0.00000000}.{macos-coreaudio-default-render}"); return HR_OK; }
static int MSABI wadev_GetState(void*self,uint32_t*st){ (void)self; if(st)*st=1; return HR_OK; }   // DEVICE_STATE_ACTIVE
static void* g_wa_dev_vtbl[]={ (void*)wadev_QI,(void*)com_AddRef,(void*)com_Release,(void*)wadev_Activate,(void*)wadev_OpenPropertyStore,(void*)wadev_GetId,(void*)wadev_GetState };
static int MSABI waep_GetDataFlow(void*self,int*df){ if(df)*df=wa_is_cap(self)?1:0; return HR_OK; }      // eCapture / eRender
static void* g_wa_ep_vtbl[]={ (void*)wadev_QI,(void*)com_AddRef,(void*)com_Release,(void*)waep_GetDataFlow };
// IPropertyStore: every key reads as the device's friendly name (PROPVARIANT: vt @0, value @8; VT_LPWSTR = 31)
static int MSABI waps_GetCount(void*self,uint32_t*n){ (void)self; if(n)*n=1; return HR_OK; }
static int MSABI waps_GetAt(void*self,uint32_t i,void*key){ (void)self;(void)i; if(key) memset(key,0,20); return HR_OK; }
static int MSABI waps_GetValue(void*self,const void*key,uint8_t*pv){ (void)key; if(!pv) return WA_E_POINTER; memset(pv,0,24);
    *(uint16_t*)pv=31; *(uint16_t**)(pv+8)=wa_wstr(self==&g_wa_cprops?(wa_mic_mode()==2?"Test tone (no microphone)":wa_mic_mode()==3?"Silent microphone":"Mac microphone (CoreAudio)"):"Mac speakers (CoreAudio)"); return HR_OK; }
static int MSABI waps_SetValue(void*self,const void*key,const void*pv){ (void)self;(void)key;(void)pv; return HR_OK; }
static int MSABI waps_Commit(void*self){ (void)self; return HR_OK; }
static void* g_wa_props_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)waps_GetCount,(void*)waps_GetAt,(void*)waps_GetValue,(void*)waps_SetValue,(void*)waps_Commit };

// ---- the enumerator + collections ------------------------------------------------------------------------------------
static ComObj g_wa_col1, g_wa_col0, g_wa_colc, g_wa_col2;   // render only, none, capture only, both
static int MSABI wacol_GetCount(void*self,unsigned*n){ if(n)*n = self==&g_wa_col2 ? 2 : (self==&g_wa_col1 || self==&g_wa_colc) ? 1 : 0; return HR_OK; }
static int MSABI wacol_Item(void*self,unsigned i,void**pp){ if(!pp) return WA_E_POINTER; *pp=0;
    if(self==&g_wa_col1 && i==0) *pp=&g_wa_dev; else if(self==&g_wa_colc && i==0) *pp=&g_wa_cdev; else if(self==&g_wa_col2 && i<2) *pp=i?(void*)&g_wa_cdev:(void*)&g_wa_dev;
    return *pp?HR_OK:HR_INVALIDARG; }
static void* g_wa_col_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)wacol_GetCount,(void*)wacol_Item };
static int MSABI wae_Enum(void*self,int df,unsigned mask,void**pp){ (void)self;(void)mask; int mic=wa_mic_mode()!=0;   // eRender 0 / eCapture 1 / eAll 2
    if(pp)*pp = df==0 ? &g_wa_col1 : df==1 ? (mic?&g_wa_colc:&g_wa_col0) : (mic?&g_wa_col2:&g_wa_col1); return HR_OK; }
static int MSABI wae_Default(void*self,int df,int role,void**pp){ (void)self;(void)role; if(!pp) return WA_E_POINTER;
    if(df==0){ *pp=&g_wa_dev; wa_log("  [wasapi] default render endpoint\n"); return HR_OK; }
    if(df==1 && wa_mic_mode()){ *pp=&g_wa_cdev; wa_log("  [wasapi] default capture endpoint\n"); return HR_OK; }
    *pp=0; return HR_NOTFOUND; }   // PELOAD_MIC=0: no capture device
static int MSABI wae_GetDevice(void*self,const uint16_t*id,void**pp){ (void)self; int cap=0;   // the capture device's id has "capture" in it
    for(int i=0; id && id[i] && !cap; i++) cap = id[i]=='c' && id[i+1]=='a' && id[i+2]=='p' && id[i+3]=='t';
    if(pp)*pp=cap && wa_mic_mode()?&g_wa_cdev:&g_wa_dev; return HR_OK; }
static int MSABI wae_RegCB(void*self,void*c){ (void)self;(void)c; return HR_OK; }
static void* g_wa_enum_vtbl[]={ (void*)com_QI,(void*)com_AddRef,(void*)com_Release,(void*)wae_Enum,(void*)wae_Default,(void*)wae_GetDevice,(void*)wae_RegCB,(void*)wae_RegCB };
static ComObj g_wa_enum;
static void* wasapi_enumerator(void){
    if(!g_wa_enum.vtbl){ g_wa_dev.vtbl=g_wa_dev_vtbl; g_wa_ep.vtbl=g_wa_ep_vtbl; g_wa_props.vtbl=g_wa_props_vtbl;
        g_wa_cdev.vtbl=g_wa_dev_vtbl; g_wa_cep.vtbl=g_wa_ep_vtbl; g_wa_cprops.vtbl=g_wa_props_vtbl;
        g_wa_col1.vtbl=g_wa_col_vtbl; g_wa_col0.vtbl=g_wa_col_vtbl; g_wa_colc.vtbl=g_wa_col_vtbl; g_wa_col2.vtbl=g_wa_col_vtbl; g_wa_enum.vtbl=g_wa_enum_vtbl; }
    return &g_wa_enum; }
