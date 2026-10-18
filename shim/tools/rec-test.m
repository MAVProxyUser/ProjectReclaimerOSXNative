// rec-test.m -- the clip recorder (shim/src/rec.m) without the game: an offscreen GL context draws an animation that
// stands in for the window (top half red, bottom half blue, a green bar sweeping across), and a thread plays two
// tones the way the audio callbacks do (an XAudio2-like 48 kHz stereo float mix and a WASAPI-like 44.1 kHz mono int16
// stream). Built and checked by rec-test.sh (x86_64 under Rosetta, like the loader).
//     rec-test <av|audio|noframes|stop> <secs> [window w] [window h]
//   av: picture + sound for secs; audio: sound only; noframes: a video recording that never gets a picture (the
//   window hidden the whole time); stop: a long recording stopped after 1.2 s. Prints the last status and the list.
//   av clips of 3 s or more carry a sync mark 1.5 s in: a white picture and a 2 kHz burst for 100 ms, made at the same
//   moment, so the recording's picture and sound line up when both start at the same time in the file.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/rec.h"

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static volatile int g_run=1;
static int g_keya, g_keyb; static double g_t0; static volatile double g_sync=-1;   // g_sync: harness time of the sync mark (white picture + 2 kHz burst, 100 ms)
static void* tones(void*a){ (void)a;
    float fa[480*2]; int16_t fb[441]; double pa=0, pb=0, next=now_s();
    while(g_run){
        double t=now_s()-g_t0; int mark=g_sync>=0 && t>=g_sync && t<g_sync+0.1;
        for(int i=0;i<480;i++){ float v=mark?0.5f*(float)sin(pa*2000/440):0.25f*(float)sin(pa); pa+=2*M_PI*440/48000; fa[2*i]=v; fa[2*i+1]=v; }
        for(int i=0;i<441;i++){ fb[i]=(int16_t)(0.15*32767*sin(pb)); pb+=2*M_PI*660/44100; }
        if(g_rec_audio){ rec_audio_push(&g_keya,fa,480,2,48000,32,8,1); rec_audio_push(&g_keyb,fb,441,1,44100,16,2,0); }   // as xa_aq_cb / wa_aq_cb do
        next+=0.010; double d=next-now_s(); if(d>0) usleep((useconds_t)(d*1e6)); }
    return 0; }
static int state_is(const char*st){ char b[1024]; rec_status_json(b,sizeof b,1); char k[48]; snprintf(k,sizeof k,"\"state\":\"%s\"",st); return strstr(b,k)!=0; }

int main(int argc,char**argv){
    const char*mode=argc>1?argv[1]:"av"; double secs=argc>2?atof(argv[2]):3; int W=argc>3?atoi(argv[3]):2560, H=argc>4?atoi(argv[4]):1440;
    { const char*h=getenv("REC_TEST_HOME"); if(h && *h) setenv("HOME",h,1); }   // the recorder writes to $HOME/Movies/Reclaimer: a scratch one
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf=0; GLint np=0; CGLChoosePixelFormat(at,&pf,&np); CGLContextObj ctx=0;
    if(!pf || CGLCreateContext(pf,0,&ctx) || !ctx){ fprintf(stderr,"no GL context\n"); return 2; }
    CGLSetCurrentContext(ctx);
    GLuint fbo, col; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);   // the "window"
    glGenTextures(1,&col); glBindTexture(GL_TEXTURE_2D,col); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,col,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){ fprintf(stderr,"fbo incomplete\n"); return 2; }
    g_t0=now_s();
    pthread_t th; pthread_create(&th,0,tones,0);
    char err[200]; int ao=!strcmp(mode,"audio"), noframes=!strcmp(mode,"noframes"), stop=!strcmp(mode,"stop");
    if(!rec_start(ao,secs,"test",W,H,err,sizeof err)){ fprintf(stderr,"rec_start failed: %s\n",err); return 1; }
    char again[200]; if(rec_start(0,5,"test",W,H,again,sizeof again)) { fprintf(stderr,"a second rec_start was accepted\n"); return 1; }   // one at a time
    double t0=g_t0, trec=now_s(); long frames=0;   // trec: the recording runs from about here
    if(!strcmp(mode,"av") && secs>=3) g_sync=trec-t0+1.5;   // the sync mark 1.5 s into the clip
    while(now_s()-t0<secs+15){
        double t=now_s()-t0;
        if(stop && now_s()-trec>1.2){ rec_stop(); stop=0; }
        glBindFramebuffer(GL_FRAMEBUFFER,fbo); glViewport(0,0,W,H); glEnable(GL_SCISSOR_TEST);
        glScissor(0,H/2,W,H-H/2); glClearColor(0.9f,0.1f,0.1f,1); glClear(GL_COLOR_BUFFER_BIT);   // GL's top half (rows H/2..H) = the window's top
        glScissor(0,0,W,H/2); glClearColor(0.1f,0.1f,0.9f,1); glClear(GL_COLOR_BUFFER_BIT);
        int bx=(int)(fmod(t*0.5,1.0)*W*0.9); glScissor(bx,0,W/20,H); glClearColor(0.1f,0.9f,0.1f,1); glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        if(g_sync>=0 && t>=g_sync && t<g_sync+0.1){ glClearColor(1,1,1,1); glClear(GL_COLOR_BUFFER_BIT); }   // the sync mark's picture half
        if(g_rec_video && !noframes){ CGLLockContext(ctx); rec_video_frame(fbo,0,0,W,H); CGLUnlockContext(ctx); frames++; }   // as the present paths do after the overlay (context locked)
        glFlush();
        if(t>0.5 && state_is("idle") && !g_rec_video) break;
        usleep(8333);   // ~120 presents a second: the recorder keeps 60
    }
    g_run=0; pthread_join(th,0);
    char st[1024]; rec_status_json(st,sizeof st,1); printf("STATUS %s\nPRESENTS %ld\n",st,frames);
    size_t n=0; char*l=rec_list_json(&n); printf("LIST %s\n",l?l:"null"); free(l);
    if(!state_is("idle")){ fprintf(stderr,"the recording did not finish\n"); return 1; }
    return strstr(st,"\"last_error\":\"\"")?0:1; }
