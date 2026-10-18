// rec.m -- clip recording for the stats page (backlog 42): what the window shows (H.264) and what the game plays (AAC),
// into ~/Movies/Reclaimer/<map>_<YYYYmmdd-HHMMSS>.mp4, or the sound alone into .m4a. Compiled beside peload.c
// (build.sh); C interface rec.h; design, formats and limits in shim/docs/recording.md.
//
// Video: once the present thread has drawn the window (game frame, the client's layered windows, the overlay), the
// picture's rect of the back buffer is blitted -- scaled to the recording size, flipped to top-down rows -- into a
// CVPixelBuffer from an IOSurface pool, through CVOpenGLTextureCache (a GL_TEXTURE_RECTANGLE on the buffer's IOSurface,
// so the pixels never cross the CPU). A fence marks the blit; a later present (or the stop) finds it signalled and
// hands the buffer to AVAssetWriter -> VideoToolbox's hardware H.264 encoder. At most 60 frames a second
// (PELOAD_REC_FPS), each stamped with its real present time; a frame the GPU or the encoder isn't ready for is dropped,
// never waited for.
// Audio: the output callbacks (the XAudio2 mix, the client's WASAPI streams) add their samples, converted to 48 kHz
// stereo float, into one ring laid out on the recording's timeline (position = time since the start, kept continuous
// per source); a 20 ms timer on the writer queue takes what is older than 200 ms (every source has written it by then)
// and appends it as AAC input. Silence where nothing played, so the sound track always spans the whole clip.
// Every AVAssetWriter call runs on one serial queue (g_rq). Manual retain/release (this file shares peload.c's flags).
#define GL_SILENCE_DEPRECATION 1
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <os/lock.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rec.h"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // CVOpenGLTextureCache / CGL: deprecated with OpenGL, which the window still is

#define REC_RATE 48000                 // the recording's sound: 48 kHz stereo float in, AAC out
#define REC_AR   (REC_RATE*4)          // mix ring: 4 s
#define REC_LAG  0.2                   // the timer takes sound this far behind "now" (every callback has added its part)
#define REC_VTS  90000                 // video timestamps in 1/90000 s

volatile int g_rec_video, g_rec_audio;
static volatile int g_rec_vstop;       // present thread: hand on the last frames, report, clear g_rec_video (under g_vmx)

static double rec_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void rec_log(const char*fmt,...){ char m[700]; va_list ap; va_start(ap,fmt); int k=vsnprintf(m,sizeof m,fmt,ap); va_end(ap);
    if(k>(int)sizeof m-1) k=(int)sizeof m-1; if(k>0) (void)!write(2,m,(size_t)k); }
static dispatch_queue_t g_rq;
static dispatch_queue_t rq(void){ static dispatch_once_t o; dispatch_once(&o,^{ g_rq=dispatch_queue_create("reclaimer.rec",DISPATCH_QUEUE_SERIAL); }); return g_rq; }
static int rec_fps(void){ const char*e=getenv("PELOAD_REC_FPS"); int f=e?atoi(e):60; return f<1?1:f>120?120:f; }
// ~/Movies/Reclaimer (made on first use): the only place recordings are written (PELOAD_REC_DIR = another folder, for test runs)
static int rec_dir(char*d,size_t n,int make){ const char*h=getenv("HOME"); if(!h||!*h) return 0;
    { const char*o=getenv("PELOAD_REC_DIR"); if(o && *o){ snprintf(d,n,"%s",o); if(make && mkdir(d,0755) && errno!=EEXIST) return 0; return 1; } }   // (test runs: a scratch folder)
    snprintf(d,n,"%s/Movies",h); if(make) mkdir(d,0755);
    snprintf(d,n,"%s/Movies/Reclaimer",h); if(make && mkdir(d,0755) && errno!=EEXIST) return 0; return 1; }

// ---- the writer side (g_rq only) ----------------------------------------------------------------------------------
enum { RS_IDLE, RS_REC, RS_STOPPING, RS_FINISHING };
static struct {
    int state, audio_only, sess, vw, vh, video_done;
    double t0, secs, stop_t, stopping_at;
    AVAssetWriter*w; AVAssetWriterInput*vin, *ain; AVAssetWriterInputPixelBufferAdaptor*ad;
    CVPixelBufferPoolRef pool; CMAudioFormatDescriptionRef afmt; dispatch_source_t timer;
    long frames, vdrop, adrop; int64_t last_vpts, apos;
    char file[160], path[1024];
} R;
// what rec_status_json shows (copied from R under g_smx: the stats thread never touches R)
static pthread_mutex_t g_smx=PTHREAD_MUTEX_INITIALIZER;
static struct { int state, audio_only, vw, vh; double t0, secs, stop_t; long frames, vdrop; char file[160], last_file[160], last_err[200]; } S;
static volatile long g_vdrop_pt;       // frames the present thread dropped (GPU / encoder behind)
// why frames were dropped (PELOAD_REC_DEBUG=1 prints them with the result): 0 GPU never finished, 1 context change,
// 2 GPU behind, 3 encoder queue behind, 4 no texture cache, 5 pool empty, 6 no texture, 7 encoder not ready, 8 append failed
static volatile long g_vwhy[9];
static void rec_vdrop(int why){ g_vdrop_pt++; g_vwhy[why]++; }
static void rec_pub(void){ pthread_mutex_lock(&g_smx); S.state=R.state; S.audio_only=R.audio_only; S.vw=R.vw; S.vh=R.vh; S.t0=R.t0; S.secs=R.secs; S.stop_t=R.stop_t;
    S.frames=R.frames; S.vdrop=R.vdrop+g_vdrop_pt; snprintf(S.file,sizeof S.file,"%s",R.state==RS_IDLE?"":R.file); pthread_mutex_unlock(&g_smx); }
static void rec_note(const char*file,const char*err){ pthread_mutex_lock(&g_smx); if(file) snprintf(S.last_file,sizeof S.last_file,"%s",file);
    snprintf(S.last_err,sizeof S.last_err,"%s",err?err:""); pthread_mutex_unlock(&g_smx); }

// the present thread's view of the running recording (g_vmx: start / stop on g_rq vs every recorded present)
static pthread_mutex_t g_vmx=PTHREAD_MUTEX_INITIALIZER;
static struct { CVPixelBufferPoolRef pool; int sess, w, h, accept; double t0, per; } V;   // per: 1 / the frame rate cap

// ---- sound: one mix ring on the recording's timeline ----------------------------------------------------------------
typedef struct { const void*key; int64_t pos; double ph, seen; float prev[2]; int fresh; } RecSrc;
static os_unfair_lock g_al=OS_UNFAIR_LOCK_INIT;
static float g_ar[REC_AR*2];
static struct { int on; double t0; int64_t rd; RecSrc src[16]; } A;
static inline void rec_frame_in(const uint8_t*p,int ch,int bits,int isfloat,float*l,float*r){
    float c[3]={0,0,0}; int m=ch<3?ch:3;
    for(int i=0;i<m;i++){ float v=0;
        if(isfloat) v=bits==64?(float)((const double*)p)[i]:((const float*)p)[i];
        else switch(bits){ case 16: v=((const int16_t*)p)[i]*(1.0f/32768.0f); break;
            case 32: v=((const int32_t*)p)[i]*(1.0f/2147483648.0f); break;
            case 8: v=(p[i]-128)*(1.0f/128.0f); break;
            case 24: { const uint8_t*s=p+3*i; int32_t x=(int32_t)((uint32_t)s[0]<<8|(uint32_t)s[1]<<16|(uint32_t)s[2]<<24)>>8; v=x*(1.0f/8388608.0f); } break; }
        c[i]=v; }
    if(ch==1){ *l=*r=c[0]; } else { *l=c[0]; *r=c[1]; if(ch>=6){ *l+=0.707f*c[2]; *r+=0.707f*c[2]; } } }   // 5.1/7.1: front pair + centre
static inline void rec_mix(int64_t pos,float l,float r){ if(pos<A.rd || pos>=A.rd+REC_AR) return; int64_t k=pos%REC_AR; g_ar[2*k]+=l; g_ar[2*k+1]+=r; }
// Runs on the audio output threads: convert, place at the source's position (time since the start, then continuous;
// re-anchored when it drifts 125 ms from the clock: a stalled or restarted stream), add. Holds g_al for a few us.
void rec_audio_push(const void*key,const void*data,uint32_t frames,int ch,int rate,int bits,int blk,int isfloat){
    if(!g_rec_audio || !data || !frames || ch<1 || ch>32 || rate<4000 || rate>768000) return;
    if(isfloat ? (bits!=32 && bits!=64) : (bits!=8 && bits!=16 && bits!=24 && bits!=32)) return;
    if(blk<ch*(bits/8)) return;
    double now=rec_now(); const uint8_t*p=(const uint8_t*)data;
    os_unfair_lock_lock(&g_al);
    if(!A.on){ os_unfair_lock_unlock(&g_al); return; }
    RecSrc*s=0; for(int i=0;i<16 && !s;i++) if(A.src[i].key==key) s=&A.src[i];
    for(int i=0;i<16 && !s;i++) if(!A.src[i].key || now-A.src[i].seen>2.0){ s=&A.src[i]; s->key=0; }   // a free slot, or one silent for 2 s
    if(!s){ os_unfair_lock_unlock(&g_al); return; }
    int64_t clk=(int64_t)((now-A.t0)*REC_RATE);
    if(s->key!=key || llabs(s->pos-clk)>REC_RATE/8){ s->key=key; s->pos=clk; s->ph=0; s->fresh=1; }
    s->seen=now;
    if(rate==REC_RATE){ for(uint32_t i=0;i<frames;i++){ float l,r; rec_frame_in(p+(size_t)i*blk,ch,bits,isfloat,&l,&r); rec_mix(s->pos++,l,r); } }
    else {   // linear interpolation; ph = the next output's place in this block's input frames (-1..0: after the last block's end)
        if(s->fresh){ rec_frame_in(p,ch,bits,isfloat,&s->prev[0],&s->prev[1]); s->ph=0; }
        double step=(double)rate/REC_RATE, t=s->ph;
        while(t<(double)frames-1){ long i0=(long)floor(t); float fr=(float)(t-i0), a0,a1,b0,b1;
            if(i0<0){ a0=s->prev[0]; a1=s->prev[1]; } else rec_frame_in(p+(size_t)i0*blk,ch,bits,isfloat,&a0,&a1);
            rec_frame_in(p+(size_t)(i0+1)*blk,ch,bits,isfloat,&b0,&b1);
            rec_mix(s->pos++,a0+(b0-a0)*fr,a1+(b1-a1)*fr); t+=step; }
        rec_frame_in(p+(size_t)(frames-1)*blk,ch,bits,isfloat,&s->prev[0],&s->prev[1]); s->ph=t-frames; }
    s->fresh=0;
    os_unfair_lock_unlock(&g_al); }

static float g_apump[4800*2];
static void rec_append_audio_q(int64_t pos,int n){
    size_t bytes=(size_t)n*8; CMBlockBufferRef bb=NULL; CMSampleBufferRef sb=NULL;
    if(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,NULL,bytes,kCFAllocatorDefault,NULL,0,bytes,kCMBlockBufferAssureMemoryNowFlag,&bb) || !bb){ R.adrop+=n; return; }
    CMBlockBufferReplaceDataBytes(g_apump,bb,0,bytes);
    if(!CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault,bb,R.afmt,(CMItemCount)n,CMTimeMake(pos,REC_RATE),NULL,&sb) && sb){
        if(![R.ain appendSampleBuffer:sb]) R.adrop+=n; CFRelease(sb); }
    else R.adrop+=n;
    CFRelease(bb); }
// take the ring up to upto_s (seconds on the timeline) and append it; an encoder that isn't ready keeps it for the next
// tick (the ring holds 4 s), unless it fell 3 s behind
static void rec_audio_pump_q(double upto_s){
    int64_t upto=(int64_t)floor(upto_s*REC_RATE);
    for(int guard=0;guard<64;guard++){
        int drop=0; if(!R.ain.readyForMoreMediaData){ if(upto-A.rd<REC_AR-REC_RATE) break; drop=1; }   // A.rd: only this queue moves it
        os_unfair_lock_lock(&g_al);
        int64_t pos=A.rd, m=upto-pos; if(m<=0){ os_unfair_lock_unlock(&g_al); break; } int n=m>4800?4800:(int)m;
        for(int i=0;i<n;i++){ int64_t k=(pos+i)%REC_AR; float l=g_ar[2*k], r=g_ar[2*k+1]; g_ar[2*k]=g_ar[2*k+1]=0;
            g_apump[2*i]=l>1?1:l<-1?-1:l; g_apump[2*i+1]=r>1?1:r<-1?-1:r; }
        A.rd=pos+n; os_unfair_lock_unlock(&g_al);
        if(drop) R.adrop+=n; else rec_append_audio_q(pos,n);
        R.apos=pos+n; } }

// ---- video: the writer side ---------------------------------------------------------------------------------------
static void rec_vappend_q(CVPixelBufferRef pb,int64_t pts,int sess){
    if(sess!=R.sess || (R.state!=RS_REC && R.state!=RS_STOPPING) || !R.ad) return;
    if(R.state==RS_STOPPING && pts>(int64_t)(R.stop_t*REC_VTS)) return;    // captured after the stop
    if(pts<=R.last_vpts || !R.vin.readyForMoreMediaData){ R.vdrop++; g_vwhy[7]++; return; }
    if([R.ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(pts,REC_VTS)]){ R.frames++; R.last_vpts=pts; } else { R.vdrop++; g_vwhy[8]++; } }
// no picture ever arrived (the window was hidden or covered the whole time): one black frame keeps the .mp4 valid
static void rec_black_frame_q(void){
    CVPixelBufferRef pb=NULL; if(!R.pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,R.pool,&pb) || !pb) return;
    CVPixelBufferLockBaseAddress(pb,0); uint8_t*b=CVPixelBufferGetBaseAddress(pb); size_t bpr=CVPixelBufferGetBytesPerRow(pb), hh=CVPixelBufferGetHeight(pb);
    if(b) for(size_t y=0;y<hh;y++) for(size_t x=0;x<(size_t)R.vw;x++){ uint8_t*q=b+y*bpr+x*4; q[0]=q[1]=q[2]=0; q[3]=255; }
    CVPixelBufferUnlockBaseAddress(pb,0);
    if(R.vin.readyForMoreMediaData && [R.ad appendPixelBuffer:pb withPresentationTime:kCMTimeZero]) R.frames++;
    CVPixelBufferRelease(pb); }

static void rec_release_q(void){
    [R.ad release]; [R.vin release]; [R.ain release]; [R.w release]; R.ad=nil; R.vin=nil; R.ain=nil; R.w=nil;
    if(R.pool){ CVPixelBufferPoolRelease(R.pool); R.pool=NULL; } if(R.afmt){ CFRelease(R.afmt); R.afmt=NULL; } }
// the hooks off: sound stops at once; the present thread drops its frames and clears g_rec_video at its next present
static void rec_hooks_off_q(void){
    g_rec_audio=0; os_unfair_lock_lock(&g_al); A.on=0; os_unfair_lock_unlock(&g_al);
    if(R.timer){ dispatch_source_cancel(R.timer); dispatch_release(R.timer); R.timer=0; }
    pthread_mutex_lock(&g_vmx); V.accept=0; if(V.pool){ CVPixelBufferPoolRelease(V.pool); V.pool=NULL; } pthread_mutex_unlock(&g_vmx); }
// An Objective-C exception from AVFoundation (a misuse it throws on instead of failing) must never reach the game: it
// would abort the process. Every queue entry point catches it here: the recording is dropped, the reason shown.
static void rec_abandon_q(NSException*ex){
    char e[200]; snprintf(e,sizeof e,"%s: %s",ex.name.UTF8String?ex.name.UTF8String:"exception",ex.reason.UTF8String?ex.reason.UTF8String:"?");
    rec_log("  [rec] %s FAILED: %s\n",R.path,e);
    if(R.state==RS_IDLE) return;
    rec_hooks_off_q(); @try { if(R.w.status==AVAssetWriterStatusWriting) [R.w cancelWriting]; } @catch(NSException*e2){ (void)e2; }
    rec_note(R.file,e); rec_release_q(); R.state=RS_IDLE; rec_pub(); }
#define REC_GUARD(body) do{ @try { body; } @catch(NSException*ex_){ rec_abandon_q(ex_); } }while(0)
// an NSError as one line: its text, domain and code, and the underlying error's (AVFoundation's own are vague)
static void rec_errstr(NSError*ne,char*o,size_t n,const char*dflt){
    if(!ne){ snprintf(o,n,"%s",dflt); return; } NSError*u=ne.userInfo[NSUnderlyingErrorKey];
    if(u) snprintf(o,n,"%s (%s %ld; %s %ld)",ne.localizedDescription.UTF8String,ne.domain.UTF8String,(long)ne.code,u.domain.UTF8String,(long)u.code);
    else snprintf(o,n,"%s (%s %ld)",ne.localizedDescription.UTF8String,ne.domain.UTF8String,(long)ne.code); }
static void rec_done_q(int sess){
    if(sess!=R.sess || R.state!=RS_FINISHING) return;
    int ok=R.w.status==AVAssetWriterStatusCompleted; char e[200]=""; if(!ok) rec_errstr(R.w.error,e,sizeof e,"the writer failed");
    struct stat st; double mb=stat(R.path,&st)?0:st.st_size/1048576.0;
    if(ok) rec_log("  [rec] saved %s: %.1f s, %s%.1f MB\n",R.path,R.stop_t,R.audio_only?"":[NSString stringWithFormat:@"%ld frames (%ld dropped), ",R.frames,R.vdrop+g_vdrop_pt].UTF8String,mb);
    else rec_log("  [rec] %s FAILED: %s\n",R.path,e);
    if(getenv("PELOAD_REC_DEBUG")) rec_log("  [rec] dropped: fence %ld, context %ld, GPU behind %ld, queue behind %ld, cache %ld, pool %ld, texture %ld, not ready %ld, append %ld; sound dropped %ld frames\n",
        g_vwhy[0],g_vwhy[1],g_vwhy[2],g_vwhy[3],g_vwhy[4],g_vwhy[5],g_vwhy[6],g_vwhy[7],g_vwhy[8],R.adrop);
    rec_note(R.file,ok?NULL:e); rec_release_q(); R.state=RS_IDLE; rec_pub(); }
static void rec_finish_q(void){
    R.state=RS_FINISHING; rec_hooks_off_q();
    if(!R.audio_only && !R.frames) rec_black_frame_q();
    [R.vin markAsFinished]; [R.ain markAsFinished];
    [R.w endSessionAtSourceTime:CMTimeMakeWithSeconds(R.stop_t,REC_VTS)];
    int sess=R.sess; [R.w finishWritingWithCompletionHandler:^{ dispatch_async(g_rq,^{ @autoreleasepool { REC_GUARD(rec_done_q(sess)); } }); }];
    rec_pub(); }
static void rec_fail_q(const char*why){   // the writer failed mid-way (disk full, ...): drop the file's tail, say why
    char e[200]; if(why) snprintf(e,sizeof e,"%s",why); else rec_errstr(R.w.error,e,sizeof e,"the writer failed");
    rec_log("  [rec] %s FAILED: %s\n",R.path,e); rec_hooks_off_q(); [R.w cancelWriting];
    rec_note(R.file,e); rec_release_q(); R.state=RS_IDLE; rec_pub(); }
static void rec_begin_stop_q(double at){
    double el=rec_now()-R.t0; R.stop_t=at<el?at:el; R.state=RS_STOPPING; R.stopping_at=rec_now();
    if(!R.audio_only){ pthread_mutex_lock(&g_vmx); V.accept=0; g_rec_vstop=1; pthread_mutex_unlock(&g_vmx); } }
static void rec_tick_q(void){
    if(R.state!=RS_REC && R.state!=RS_STOPPING) return;
    if(R.w.status==AVAssetWriterStatusFailed){ rec_fail_q(NULL); return; }
    double now=rec_now(), el=now-R.t0;
    if(R.state==RS_REC && el>=R.secs) rec_begin_stop_q(R.secs);
    double upto=el-REC_LAG; if(R.state==RS_STOPPING && upto>R.stop_t) upto=R.stop_t;
    rec_audio_pump_q(upto);
    // done when the sound reached the stop and the present thread handed on its last frames (or is not presenting: 2 s)
    if(R.state==RS_STOPPING && el-REC_LAG>=R.stop_t && (R.audio_only || R.video_done || now-R.stopping_at>2.0)) rec_finish_q();
    else rec_pub(); }

static int rec_start_q(int ao,double secs,const char*name,int vw,int vh,char*err,size_t errn){
    AVAssetWriter*wr=nil; AVAssetWriterInput*vin=nil,*ain=nil; AVAssetWriterInputPixelBufferAdaptor*ad=nil; CVPixelBufferPoolRef pool=NULL; CMAudioFormatDescriptionRef afmt=NULL;
    #define FAIL(...) do{ snprintf(err,errn,__VA_ARGS__); goto fail; }while(0)
    if(R.state!=RS_IDLE) FAIL("already recording");
    if(!(secs>=1)) secs=1; if(secs>600) secs=600;
    if(!ao && (vw<16 || vh<16)) FAIL("no game window to record (hidden): choose Audio only");
    char dir[900]; if(!rec_dir(dir,sizeof dir,1)) FAIL("cannot create ~/Movies/Reclaimer");
    { char stem[48]; int j=0; for(const char*p=name?name:"";*p && j<40;p++){ char c=*p; if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-') stem[j++]=c; }
      stem[j]=0; if(!j) snprintf(stem,sizeof stem,"game");
      time_t tt=time(0); struct tm lt; localtime_r(&tt,&lt); char ts[32]; strftime(ts,sizeof ts,"%Y%m%d-%H%M%S",&lt);
      for(int k=1;k<100;k++){ struct stat st;
          if(k==1) snprintf(R.file,sizeof R.file,"%s_%s.%s",stem,ts,ao?"m4a":"mp4"); else snprintf(R.file,sizeof R.file,"%s_%s-%d.%s",stem,ts,k,ao?"m4a":"mp4");
          snprintf(R.path,sizeof R.path,"%s/%s",dir,R.file); if(stat(R.path,&st)) break; } }
    // the recording's size: the window's picture, at most 1920x1080, aspect kept, even (H.264's 4:2:0)
    int w=0,h=0; if(!ao){ double s=1; if(vw>1920) s=1920.0/vw; if(vh*s>1080) s=1080.0/vh; w=(int)(vw*s+0.5)&~1; h=(int)(vh*s+0.5)&~1; }
    // bit rate: ~0.1 bit per pixel per frame (1080p60: 12.4 Mbit/s; PELOAD_REC_KBPS overrides), AAC 192 kbit/s
    int fps=rec_fps(); const char*kb=getenv("PELOAD_REC_KBPS"); long kbps=ao?0:kb?atol(kb):(long)((double)w*h*fps*0.1/1000.0);
    if(!ao){ if(kbps<1000) kbps=1000; if(kbps>60000) kbps=60000; }
    { struct statfs sf; double need=(kbps+192)*125.0*secs+256e6;   // the whole clip plus 256 MB to spare
      if(!statfs(dir,&sf) && (double)sf.f_bavail*sf.f_bsize<need) FAIL("not enough free disk space for %.0f s (%.0f MB needed)",secs,need/1e6); }
    NSError*ne=nil;
    wr=[[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:R.path]] fileType:ao?AVFileTypeAppleM4A:AVFileTypeMPEG4 error:&ne];
    if(!wr){ char es[200]; rec_errstr(ne,es,sizeof es,"?"); FAIL("cannot write %s: %s",R.file,es); }
    // fragments every 2 s: a game that crashes or quits mid-clip leaves a file that plays up to the last fragment
    if(!getenv("PELOAD_REC_NOFRAG")) wr.movieFragmentInterval=CMTimeMakeWithSeconds(2.0,600);
    if(!ao){
        int hevc=getenv("PELOAD_REC_HEVC")&&atoi(getenv("PELOAD_REC_HEVC"));
        // no B-frames: with them, finishing a fragmented .mp4 failed in ~1 of 8 test runs (AVFoundation -11800 /
        // OSStatus -16341, the file cut at its last fragment); 0 of 60 without. Also no reorder delay for a live source.
        NSMutableDictionary*comp=[NSMutableDictionary dictionaryWithDictionary:@{ AVVideoAverageBitRateKey:@(kbps*1000), AVVideoExpectedSourceFrameRateKey:@(fps), AVVideoMaxKeyFrameIntervalKey:@(fps*2), AVVideoAllowFrameReorderingKey:@NO }];
        if(!hevc) comp[AVVideoProfileLevelKey]=AVVideoProfileLevelH264HighAutoLevel;
        NSDictionary*vs=@{ AVVideoCodecKey:hevc?AVVideoCodecTypeHEVC:AVVideoCodecTypeH264, AVVideoWidthKey:@(w), AVVideoHeightKey:@(h), AVVideoCompressionPropertiesKey:comp,
            AVVideoColorPropertiesKey:@{ AVVideoColorPrimariesKey:AVVideoColorPrimaries_ITU_R_709_2, AVVideoTransferFunctionKey:AVVideoTransferFunction_ITU_R_709_2, AVVideoYCbCrMatrixKey:AVVideoYCbCrMatrix_ITU_R_709_2 } };
        vin=[[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:vs]; vin.expectsMediaDataInRealTime=YES;
        NSDictionary*pa=@{ (id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA), (id)kCVPixelBufferWidthKey:@(w), (id)kCVPixelBufferHeightKey:@(h),
            (id)kCVPixelBufferIOSurfacePropertiesKey:@{}, (id)kCVPixelBufferOpenGLCompatibilityKey:@YES };
        ad=[[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:vin sourcePixelBufferAttributes:pa];
        if(![wr canAddInput:vin]) FAIL("the video encoder refused %dx%d",w,h);
        [wr addInput:vin];
        if(CVPixelBufferPoolCreate(kCFAllocatorDefault,(CFDictionaryRef)@{ (id)kCVPixelBufferPoolMinimumBufferCountKey:@4 },(CFDictionaryRef)pa,&pool) || !pool) FAIL("no pixel buffer pool");
        rec_log("  [rec] video %dx%d (window picture %dx%d), %s %ld kbit/s, up to %d fps\n",w,h,vw,vh,hevc?"HEVC":"H.264",kbps,fps); }
    AudioChannelLayout cl; memset(&cl,0,sizeof cl); cl.mChannelLayoutTag=kAudioChannelLayoutTag_Stereo;
    ain=[[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio outputSettings:@{ AVFormatIDKey:@(kAudioFormatMPEG4AAC), AVSampleRateKey:@(REC_RATE),
        AVNumberOfChannelsKey:@2, AVEncoderBitRateKey:@192000, AVChannelLayoutKey:[NSData dataWithBytes:&cl length:sizeof cl] }];
    ain.expectsMediaDataInRealTime=YES;
    if(![wr canAddInput:ain]) FAIL("the AAC encoder refused the sound settings");
    [wr addInput:ain];
    { AudioStreamBasicDescription f; memset(&f,0,sizeof f); f.mSampleRate=REC_RATE; f.mFormatID=kAudioFormatLinearPCM; f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked;
      f.mBytesPerPacket=8; f.mFramesPerPacket=1; f.mBytesPerFrame=8; f.mChannelsPerFrame=2; f.mBitsPerChannel=32;
      if(CMAudioFormatDescriptionCreate(kCFAllocatorDefault,&f,sizeof cl,&cl,0,NULL,NULL,&afmt) || !afmt) FAIL("no audio format"); }
    if(![wr startWriting]){ char es[200]; rec_errstr(wr.error,es,sizeof es,"?"); FAIL("cannot write %s: %s",R.file,es); }
    [wr startSessionAtSourceTime:kCMTimeZero];
    R.w=wr; R.vin=vin; R.ain=ain; R.ad=ad; R.pool=pool; R.afmt=afmt; R.audio_only=ao; R.secs=secs; R.vw=w; R.vh=h;
    R.frames=R.vdrop=R.adrop=0; g_vdrop_pt=0; memset((void*)g_vwhy,0,sizeof g_vwhy); R.last_vpts=-1; R.apos=0; R.video_done=0; R.stop_t=0; R.sess++;
    double t0=rec_now(); R.t0=t0;
    os_unfair_lock_lock(&g_al); memset(g_ar,0,sizeof g_ar); memset(A.src,0,sizeof A.src); A.rd=0; A.t0=t0; A.on=1; os_unfair_lock_unlock(&g_al);
    if(!ao){ pthread_mutex_lock(&g_vmx); if(V.pool) CVPixelBufferPoolRelease(V.pool); V.pool=CVPixelBufferPoolRetain(pool); V.sess=R.sess; V.w=w; V.h=h; V.t0=t0; V.per=1.0/rec_fps(); V.accept=1; g_rec_vstop=0; pthread_mutex_unlock(&g_vmx); }
    R.state=RS_REC; __sync_synchronize();
    g_rec_audio=1; if(!ao) g_rec_video=1;
    R.timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,g_rq);
    dispatch_source_set_timer(R.timer,dispatch_time(DISPATCH_TIME_NOW,20*NSEC_PER_MSEC),20*NSEC_PER_MSEC,5*NSEC_PER_MSEC);
    dispatch_source_set_event_handler(R.timer,^{ @autoreleasepool { REC_GUARD(rec_tick_q()); } }); dispatch_resume(R.timer);
    rec_note(NULL,NULL); rec_pub();
    rec_log("  [rec] recording %s (%s, %.0f s)\n",R.path,ao?"sound only":"picture + sound",secs);
    return 1;
fail:
    #undef FAIL
    rec_log("  [rec] not started: %s\n",err);
    if(wr && wr.status==AVAssetWriterStatusWriting) [wr cancelWriting];
    [ad release]; [vin release]; [ain release]; [wr release]; if(pool) CVPixelBufferPoolRelease(pool); if(afmt) CFRelease(afmt);
    if(R.state==RS_IDLE){ R.file[0]=0; rec_note(NULL,err); }
    return 0; }
int rec_start(int audio_only,double secs,const char*name,int vw,int vh,char*err,size_t errn){
    char e[200]=""; char*ep=e; __block int ok=0;
    dispatch_sync(rq(),^{ @autoreleasepool { @try { ok=rec_start_q(audio_only,secs,name,vw,vh,ep,sizeof e); }
        @catch(NSException*ex){ snprintf(ep,sizeof e,"%s",ex.reason.UTF8String?ex.reason.UTF8String:"exception"); rec_abandon_q(ex); if(R.state==RS_IDLE) rec_note(NULL,ep); ok=0; } } });
    if(err && errn) snprintf(err,errn,"%s",e);
    return ok; }
void rec_stop(void){ dispatch_async(rq(),^{ if(R.state==RS_REC){ rec_begin_stop_q(1e9); rec_log("  [rec] stop requested at %.1f s\n",R.stop_t); } }); }

// ---- video: the present thread ------------------------------------------------------------------------------------
typedef struct { CVPixelBufferRef pb; CVOpenGLTextureRef tex; GLsync fence; int64_t pts; int sess; } RecPend;
static RecPend g_vq[4]; static int g_vqn;                       // blits whose fence hasn't been seen yet, oldest first
static CGLContextObj g_vctx; static GLuint g_vfbo; static CVOpenGLTextureCacheRef g_vcache;
static double g_vnext; static int g_vsess; static volatile int g_vinflight;
static void rec_vsend(RecPend*p,int append){   // the blit is done (or given up): the buffer goes to the writer queue
    if(p->fence) glDeleteSync(p->fence); if(p->tex) CFRelease(p->tex);
    CVPixelBufferRef pb=p->pb; int64_t pts=p->pts; int sess=p->sess; p->pb=NULL; p->tex=NULL; p->fence=NULL;
    if(!append){ CVPixelBufferRelease(pb); rec_vdrop(0); return; }
    __sync_fetch_and_add(&g_vinflight,1);
    dispatch_async(rq(),^{ @autoreleasepool { REC_GUARD(rec_vappend_q(pb,pts,sess)); } CVPixelBufferRelease(pb); __sync_fetch_and_sub(&g_vinflight,1); }); }
static void rec_vpoll(int wait){   // hand on every finished blit in order; wait (the stop): up to 50 ms each
    int k=0;
    for(;k<g_vqn;k++){ GLenum r=glClientWaitSync(g_vq[k].fence,wait?GL_SYNC_FLUSH_COMMANDS_BIT:0,wait?50000000ull:0);
        if(!wait && r==GL_TIMEOUT_EXPIRED) break;
        rec_vsend(&g_vq[k],r==GL_ALREADY_SIGNALED || r==GL_CONDITION_SATISFIED); }
    if(k){ memmove(g_vq,g_vq+k,sizeof(RecPend)*(size_t)(g_vqn-k)); g_vqn-=k; } }
void rec_video_frame(unsigned srcfbo,int x,int y,int w,int h){
    CGLContextObj ctx=CGLGetCurrentContext(); if(!ctx) return;
    if(ctx!=g_vctx){   // first use, or another presenting context: the old one's GL objects can't be touched from here
        for(int i=0;i<g_vqn;i++){ if(g_vq[i].tex) CFRelease(g_vq[i].tex); CVPixelBufferRelease(g_vq[i].pb); rec_vdrop(1); }
        g_vqn=0; g_vctx=ctx; g_vfbo=0; g_vcache=NULL; }
    rec_vpoll(0);
    if(g_vcache) CVOpenGLTextureCacheFlush(g_vcache,0);   // textures handed on: the cache lets go of their surfaces (else the pool runs dry)
    pthread_mutex_lock(&g_vmx); int accept=V.accept, sess=V.sess, ow=V.w, oh=V.h; double t0=V.t0, per=V.per; CVPixelBufferPoolRef pool=accept&&V.pool?CVPixelBufferPoolRetain(V.pool):NULL; pthread_mutex_unlock(&g_vmx);
    if(!accept){   // stopped: the last frames go out, the writer hears we're done, the hook switches itself off
        rec_vpoll(1); if(g_vcache) CVOpenGLTextureCacheFlush(g_vcache,0);
        pthread_mutex_lock(&g_vmx); int told=0; if(!V.accept){ told=g_rec_vstop; g_rec_vstop=0; g_rec_video=0; } int s=V.sess; pthread_mutex_unlock(&g_vmx);
        if(told) dispatch_async(rq(),^{ if(R.sess==s) R.video_done=1; });
        return; }
    if(sess!=g_vsess){ g_vsess=sess; g_vnext=0; }
    double now=rec_now();
    if(now<g_vnext-0.004) goto out;                                 // over the frame rate: skip this present
    g_vnext=(now-g_vnext>per)?now+per:g_vnext+per;
    if(g_vqn>=3){ rec_vdrop(2); goto out; }                        // the GPU is behind
    if(g_vinflight>=6 || !pool || w<1 || h<1){ rec_vdrop(3); goto out; }   // the encoder is behind
    if(!g_vfbo) glGenFramebuffers(1,&g_vfbo);
    if(!g_vcache && (CVOpenGLTextureCacheCreate(kCFAllocatorDefault,NULL,ctx,CGLGetPixelFormat(ctx),NULL,&g_vcache) || !g_vcache)){ g_vcache=NULL; rec_vdrop(4); goto out; }
    { static CFDictionaryRef aux; if(!aux) aux=(CFDictionaryRef)[@{ (id)kCVPixelBufferPoolAllocationThresholdKey:@8 } retain];   // never more than 8 buffers
      CVPixelBufferRef pb=NULL; CVOpenGLTextureRef tex=NULL;
      if(CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,pool,aux,&pb) || !pb){ rec_vdrop(5); goto out; }
      if(CVOpenGLTextureCacheCreateTextureFromImage(kCFAllocatorDefault,g_vcache,pb,NULL,&tex) || !tex){ CVPixelBufferRelease(pb); rec_vdrop(6); goto out; }
      GLenum tgt=CVOpenGLTextureGetTarget(tex); GLuint name=CVOpenGLTextureGetName(tex);
      GLint rfb=0, dfb=0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&rfb); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&dfb);
      GLboolean sc=glIsEnabled(GL_SCISSOR_TEST); if(sc) glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER,g_vfbo); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tgt,name,0);
      glBindFramebuffer(GL_READ_FRAMEBUFFER,srcfbo);
      // window rows run bottom-up, the video's top-down: the window's top row lands in the buffer's first row
      glBlitFramebuffer(x,y,x+w,y+h, 0,oh,ow,0, GL_COLOR_BUFFER_BIT,(w==ow && h==oh)?GL_NEAREST:GL_LINEAR);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tgt,0,0);   // detached: the pool can recycle the surface
      glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)rfb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)dfb); if(sc) glEnable(GL_SCISSOR_TEST);
      GLsync f=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0); glFlush();
      RecPend p={ pb, tex, f, (int64_t)llround((now-t0)*REC_VTS), sess }; g_vq[g_vqn++]=p; }
out:
    if(pool) CVPixelBufferPoolRelease(pool); }

// ---- status, the folder's list, file lookup (stats server threads) ----------------------------------------------------
static void rec_jstr(char*o,size_t n,const char*s){ size_t j=0; for(;*s && j+7<n;s++){ unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){ o[j++]='\\'; o[j++]=(char)c; } else if(c<0x20){ j+=(size_t)snprintf(o+j,n-j,"\\u%04x",c); } else o[j++]=(char)c; } o[j]=0; }
int rec_status_json(char*b,size_t n,int video_ok){
    pthread_mutex_lock(&g_smx); __typeof__(S) s=S; pthread_mutex_unlock(&g_smx);
    const char*st=s.state==RS_REC?"recording":s.state==RS_IDLE?"idle":"finishing";
    double el=s.state==RS_REC?rec_now()-s.t0:s.state==RS_IDLE?0:s.stop_t; if(el<0) el=0; if(el>s.secs && s.secs>0) el=s.secs;
    char le[420]; rec_jstr(le,sizeof le,s.last_err);
    return snprintf(b,n,"{\"state\":\"%s\",\"mode\":\"%s\",\"secs\":%.0f,\"elapsed\":%.1f,\"left\":%.1f,\"file\":\"%s\",\"width\":%d,\"height\":%d,\"frames\":%ld,\"dropped\":%ld,"
        "\"video_ok\":%s,\"dir\":\"~/Movies/Reclaimer\",\"last_file\":\"%s\",\"last_error\":\"%s\"}",
        st,s.audio_only?"audio":"av",s.secs,el,s.state==RS_REC?(s.secs-el>0?s.secs-el:0):0.0,s.file,s.vw,s.vh,s.frames,s.vdrop,video_ok?"true":"false",s.last_file,le); }
static uint32_t rec_be32(const uint8_t*p){ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint64_t rec_be64(const uint8_t*p){ return (uint64_t)rec_be32(p)<<32|rec_be32(p+4); }
// the clip's length from its MPEG-4 boxes: moov/mvhd (timescale, duration), or moov/mvex/mehd when fragmented
static double rec_mp4_duration(const char*path){
    FILE*f=fopen(path,"rb"); if(!f) return 0; fseeko(f,0,SEEK_END); long long end=ftello(f); double d=0; uint32_t ts=0; uint64_t du=0, fdu=0;
    long long lo[3]={0}, hi[3]={end}; int lv=0, guard=0; lo[0]=0;   // a small box walk: moov (level 0) -> mvhd / mvex (1) -> mehd (2)
    while(lv>=0 && guard++<4000){
        if(lo[lv]+8>hi[lv]){ lv--; continue; }
        uint8_t h[16]; fseeko(f,lo[lv],SEEK_SET); if(fread(h,1,8,f)!=8) break;
        uint64_t sz=rec_be32(h); int hl=8; if(sz==1){ if(fread(h+8,1,8,f)!=8) break; sz=rec_be64(h+8); hl=16; } else if(sz==0) sz=(uint64_t)(hi[lv]-lo[lv]);
        if(sz<(uint64_t)hl || lo[lv]+(long long)sz>hi[lv]) break;
        long long bs=lo[lv]; lo[lv]+=(long long)sz;
        if((lv==0 && !memcmp(h+4,"moov",4)) || (lv==1 && !memcmp(h+4,"mvex",4))){ lv++; lo[lv]=bs+hl; hi[lv]=bs+(long long)sz; continue; }
        uint8_t c[32]; size_t got=fread(c,1,32,f);
        if(lv==1 && !memcmp(h+4,"mvhd",4) && got>=20){ if(c[0]==1 && got>=32){ ts=rec_be32(c+20); du=rec_be64(c+24); } else { ts=rec_be32(c+12); du=rec_be32(c+16); } }
        if(lv==2 && !memcmp(h+4,"mehd",4) && got>=8) fdu=c[0]==1&&got>=12?rec_be64(c+4):rec_be32(c+4); }
    fclose(f);
    if(ts){ uint64_t x=(du && du!=0xFFFFFFFFu && du!=~0ull)?du:fdu; d=(double)x/ts; }
    return d; }
static int rec_name_ok(const char*s){ size_t L=strlen(s); if(L<5 || L>=150 || s[0]=='.') return 0;
    for(size_t i=0;i<L;i++){ char c=s[i]; if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0; }
    return !strstr(s,"..") && (!strcmp(s+L-4,".mp4") || !strcmp(s+L-4,".m4a")); }
int rec_file_path(const char*name,char*path,size_t n){ char d[900]; struct stat st;
    if(!name || !rec_name_ok(name) || !rec_dir(d,sizeof d,0)) return 0;
    snprintf(path,n,"%s/%s",d,name); return !stat(path,&st) && S_ISREG(st.st_mode); }
typedef struct { char name[152]; long long size; double mtime, dur; } RecEnt;
static int rec_ent_cmp(const void*a,const void*b){ double x=((const RecEnt*)a)->mtime, y=((const RecEnt*)b)->mtime; return x<y?1:x>y?-1:0; }
char* rec_list_json(size_t*len){
    char d[900], busy[160]; pthread_mutex_lock(&g_smx); snprintf(busy,sizeof busy,"%s",S.state==RS_IDLE?"":S.file); pthread_mutex_unlock(&g_smx);
    RecEnt*e=calloc(500,sizeof *e); int ne=0; DIR*dd=(e && rec_dir(d,sizeof d,0))?opendir(d):NULL;
    if(dd){ struct dirent*de; while((de=readdir(dd)) && ne<500){ if(!rec_name_ok(de->d_name) || (busy[0] && !strcmp(busy,de->d_name))) continue;   // not the clip being written
            char p[1100]; struct stat st; snprintf(p,sizeof p,"%s/%s",d,de->d_name); if(stat(p,&st) || !S_ISREG(st.st_mode)) continue;
            snprintf(e[ne].name,sizeof e[ne].name,"%s",de->d_name); e[ne].size=st.st_size; e[ne].mtime=st.st_mtimespec.tv_sec+st.st_mtimespec.tv_nsec*1e-9; e[ne].dur=rec_mp4_duration(p); ne++; }
        closedir(dd); }
    if(ne>1) qsort(e,(size_t)ne,sizeof *e,rec_ent_cmp);
    size_t cap=64+(size_t)ne*300; char*o=malloc(cap); size_t k=1; if(!o){ free(e); if(len) *len=0; return NULL; }
    o[0]='[';
    for(int i=0;i<ne;i++){ int q=snprintf(o+k,cap-k-2,"%s{\"file\":\"%s\",\"size\":%lld,\"duration\":%.2f,\"time\":%.0f,\"audio_only\":%s}",i?",":"",e[i].name,e[i].size,e[i].dur,e[i].mtime,
            !strcmp(e[i].name+strlen(e[i].name)-4,".m4a")?"true":"false");
        if(q<0 || (size_t)q>=cap-k-2) break; k+=(size_t)q; }   // (names are at most 149 characters: never cut short)
    o[k++]=']'; o[k]=0; free(e); if(len) *len=k; return o; }
