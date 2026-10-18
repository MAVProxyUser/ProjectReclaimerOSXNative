// glhelper: the native GL helper (PERF-PLAN option 1). A native arm64 process started by the loader (glh_client.c,
// PELOAD_GL_HELPER=1) that maps the loader's command ring, owns the real OpenGL context and runs the D3D11->GL path's GL
// calls at native speed, in parallel with the loader. Frames go back through three IOSurfaces.
//     glhelper <shm name> <wake fd> <loader pid>          (built by shim/build.sh as shim/build/glhelper, arm64)
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/CGLIOSurface.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include "glh_shm.h"
#include "glh_dec.inc"

static GlhHdr* H;
static uint64_t g_draws;   // draw calls run (statistics)
static uint64_t op_n[GLH_OP_GEN_END+1], op_t[GLH_OP_GEN_END+1];   // PELOAD_GLH_OPSTATS: calls and time per GL function
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static void fail(const char*m){ if(H){ snprintf(H->err,sizeof H->err,"%s",m); atomic_store(&H->failed,1); } fprintf(stderr,"  [glhelper] %s\n",m); exit(1); }

// ---- name pools -------------------------------------------------------------------------------------------------------
static void pool_fill(int k){
    uint32_t h=atomic_load_explicit(&H->pool_head[k],memory_order_relaxed), t=atomic_load_explicit(&H->pool_tail[k],memory_order_acquire);
    uint32_t want=GLH_POOLN-(h-t); if(want<GLH_POOLN/2) return;
    GLuint tmp[GLH_POOLN];
    switch(k){
    case GLH_POOL_TEX: glGenTextures((GLsizei)want,tmp); break;
    case GLH_POOL_BUF: glGenBuffers((GLsizei)want,tmp); break;
    case GLH_POOL_FBO: glGenFramebuffers((GLsizei)want,tmp); break;
    case GLH_POOL_RBO: glGenRenderbuffers((GLsizei)want,tmp); break;
    case GLH_POOL_VAO: glGenVertexArrays((GLsizei)want,tmp); break;
    case GLH_POOL_SAMP: glGenSamplers((GLsizei)want,tmp); break;
    case GLH_POOL_QUERY: glGenQueries((GLsizei)want,tmp); break;
    case GLH_POOL_PIPE: glGenProgramPipelines((GLsizei)want,tmp); break;
    case GLH_POOL_PROG: for(uint32_t i=0;i<want;i++) tmp[i]=glCreateProgram(); break;
    case GLH_POOL_VS: for(uint32_t i=0;i<want;i++) tmp[i]=glCreateShader(GL_VERTEX_SHADER); break;
    case GLH_POOL_FS: for(uint32_t i=0;i<want;i++) tmp[i]=glCreateShader(GL_FRAGMENT_SHADER); break; }
    for(uint32_t i=0;i<want;i++) H->pool[k][(h+i)%GLH_POOLN]=tmp[i];
    atomic_store_explicit(&H->pool_head[k],h+want,memory_order_release); }
static void pools_fill(void){ for(int k=0;k<GLH_NPOOL;k++) pool_fill(k); }

// ---- fences (by loader id), queries ---------------------------------------------------------------------------------------
enum { NFENCE=4096 };
static struct { uint64_t id; GLsync s; } fences[NFENCE];
static GLsync fence_get(uint64_t id){ return fences[id%NFENCE].id==id?fences[id%NFENCE].s:0; }
// ended queries, oldest first (a ring): results become available in submission order, so polling stops at the first one
// that is not ready (polling every pending query every 2 ms was thousands of GL calls)
enum { QP=8192 };
static struct { GLuint id; uint32_t gen; } qactive[2], qpend[QP]; static unsigned qp_head, qp_tail;
static unsigned long st_qpolls;
static void queries_poll(void){
    while(qp_tail!=qp_head){ unsigned k=qp_tail%QP; GLint av=0; glGetQueryObjectiv(qpend[k].id,GL_QUERY_RESULT_AVAILABLE,&av); st_qpolls++; if(!av) break;
        GLuint64 v=0; glGetQueryObjectui64v(qpend[k].id,GL_QUERY_RESULT,&v); GLuint id=qpend[k].id;
        if(id<GLH_QMAX){ H->q[id].value=v; atomic_store_explicit(&H->q[id].avail,1,memory_order_relaxed); atomic_store_explicit(&H->q[id].gen,qpend[k].gen,memory_order_release); }
        qp_tail++; } }

// ---- presentation ---------------------------------------------------------------------------------------------------------
static IOSurfaceRef pq_surf[3]; static GLuint pq_tex[3], pq_fbo[3]; static int pq_w, pq_h;
static struct { int slot; GLsync f; uint64_t frameno; double t; } pq_pend[3]; static int npq;
static CGLContextObj ctx;
static CFNumberRef num(int v){ return CFNumberCreate(0,kCFNumberIntType,&v); }
static void pq_make(int W,int Hh){
    for(int i=0;i<3;i++){ if(pq_surf[i]) CFRelease(pq_surf[i]);
        const void*k[]={kIOSurfaceWidth,kIOSurfaceHeight,kIOSurfaceBytesPerElement,kIOSurfacePixelFormat,kIOSurfaceIsGlobal};
        int fmt='BGRA'; const void*v[]={num(W),num(Hh),num(4),num(fmt),kCFBooleanTrue};
        CFDictionaryRef d=CFDictionaryCreate(0,k,v,5,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        pq_surf[i]=IOSurfaceCreate(d); CFRelease(d); if(!pq_surf[i]) fail("IOSurfaceCreate failed");
        if(!pq_tex[i]){ glGenTextures(1,&pq_tex[i]); glGenFramebuffers(1,&pq_fbo[i]); }
        glBindTexture(GL_TEXTURE_RECTANGLE,pq_tex[i]);
        if(CGLTexImageIOSurface2D(ctx,GL_TEXTURE_RECTANGLE,GL_RGBA,W,Hh,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,pq_surf[i],0)) fail("CGLTexImageIOSurface2D failed");
        glBindFramebuffer(GL_FRAMEBUFFER,pq_fbo[i]); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_RECTANGLE,pq_tex[i],0);
        H->pq_ios[i]=IOSurfaceGetID(pq_surf[i]); }
    H->pq_w=(uint32_t)W; H->pq_h=(uint32_t)Hh; pq_w=W; pq_h=Hh; }
// PELOAD_GLH_MP: Apple's multithreaded GL engine in the helper (1 = on, ab:<secs> = alternate and log each phase)
static int mp_mode=-1; static double mp_ab_s, mp_t0, mp_busy0; static int mp_on; static uint64_t mp_f0, mp_d0, mp_frames; static double mp_tot[2], mp_totd[2];
static void mp_set(int on){ if(on) CGLEnable(ctx,kCGLCEMPEngine); else CGLDisable(ctx,kCGLCEMPEngine); mp_on=on; }
static const char* op_name(int op);
static void opstats_tick(void){ static double tl; double t=now_s(); if(!op_n[0] && t-tl<4.0) { if(!tl) tl=t; return; } if(t-tl<4.0) return;
    static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb);
    int idx[GLH_OP_GEN_END]; int n=0; uint64_t tot=0, totn=0; for(int i=1;i<GLH_OP_GEN_END;i++) if(op_n[i]){ idx[n++]=i; tot+=op_t[i]; totn+=op_n[i]; }
    for(int a=0;a<n;a++) for(int b=a+1;b<n;b++) if(op_t[idx[b]]>op_t[idx[a]]){ int x=idx[a]; idx[a]=idx[b]; idx[b]=x; }
    fprintf(stderr,"  [glhelper ops] %.1f s: %llu GL calls, %.0f ms in them, %llu draws (%.1f calls per draw):",t-tl,(unsigned long long)totn,tot*(double)tb.numer/tb.denom/1e6,(unsigned long long)g_draws,g_draws?(double)totn/g_draws:0);
    for(int a=0;a<n && a<14;a++){ int i=idx[a]; fprintf(stderr," %s %llu/%.0fms",op_name(i),(unsigned long long)op_n[i],op_t[i]*(double)tb.numer/tb.denom/1e6); }
    fprintf(stderr,"\n"); memset(op_n,0,sizeof op_n); memset(op_t,0,sizeof op_t); g_draws=0; tl=t; }
/* op names */
static const char* op_name(int op){ switch(op){ case 1: return "ActiveTexture"; case 2: return "AttachShader"; case 3: return "BeginQuery"; case 4: return "BindBuffer"; case 5: return "BindBufferRange"; case 6: return "BindFramebuffer"; case 7: return "BindProgramPipeline"; case 8: return "BindRenderbuffer"; case 9: return "BindSampler"; case 10: return "BindTexture"; case 11: return "BindVertexArray"; case 12: return "BlendColor"; case 13: return "BlendEquationSeparate"; case 14: return "BlendEquationSeparatei"; case 15: return "BlendFunc"; case 16: return "BlendFuncSeparate"; case 17: return "BlendFuncSeparatei"; case 18: return "BlitFramebuffer"; case 19: return "BufferData"; case 20: return "BufferSubData"; case 21: return "CheckFramebufferStatus"; case 22: return "Clear"; case 23: return "ClearColor"; case 24: return "ClearDepth"; case 25: return "ClearStencil"; case 26: return "ColorMask"; case 27: return "ColorMaski"; case 28: return "CompileShader"; case 29: return "CompressedTexImage2D"; case 30: return "CreateProgram"; case 31: return "CreateShader"; case 32: return "CullFace"; case 33: return "DeleteBuffers"; case 34: return "DeleteFramebuffers"; case 35: return "DeleteQueries"; case 36: return "DeleteSamplers"; case 37: return "DeleteShader"; case 38: return "DeleteSync"; case 39: return "DeleteTextures"; case 40: return "DepthFunc"; case 41: return "DepthMask"; case 42: return "DepthRange"; case 43: return "DetachShader"; case 44: return "Disable"; case 45: return "DisableVertexAttribArray"; case 46: return "Disablei"; case 47: return "DrawArrays"; case 48: return "DrawArraysInstanced"; case 49: return "DrawBuffer"; case 50: return "DrawBuffers"; case 51: return "DrawElements"; case 52: return "DrawElementsBaseVertex"; case 53: return "DrawElementsInstanced"; case 54: return "DrawElementsInstancedBaseVertex"; case 55: return "Enable"; case 56: return "EnableVertexAttribArray"; case 57: return "Enablei"; case 58: return "EndQuery"; case 59: return "FenceSync"; case 60: return "Finish"; case 61: return "Flush"; case 62: return "FramebufferRenderbuffer"; case 63: return "FramebufferTexture2D"; case 64: return "FrontFace"; case 65: return "GenBuffers"; case 66: return "GenFramebuffers"; case 67: return "GenProgramPipelines"; case 68: return "GenQueries"; case 69: return "GenRenderbuffers"; case 70: return "GenSamplers"; case 71: return "GenTextures"; case 72: return "GenVertexArrays"; case 73: return "GenerateMipmap"; case 74: return "GetBooleanv"; case 75: return "GetBufferSubData"; case 76: return "GetError"; case 77: return "GetFloatv"; case 78: return "GetIntegerv"; case 79: return "GetProgramInfoLog"; case 80: return "GetProgramiv"; case 81: return "GetQueryObjectiv"; case 82: return "GetQueryObjectui64v"; case 83: return "GetShaderInfoLog"; case 84: return "GetShaderSource"; case 85: return "GetShaderiv"; case 86: return "GetString"; case 87: return "GetTexImage"; case 88: return "GetTexLevelParameteriv"; case 89: return "GetUniformBlockIndex"; case 90: return "GetUniformLocation"; case 91: return "GetUniformfv"; case 92: return "IsEnabled"; case 93: return "LinkProgram"; case 94: return "MapBufferRange"; case 95: return "PixelStorei"; case 96: return "PolygonOffset"; case 97: return "PrimitiveRestartIndex"; case 98: return "ProgramParameteri"; case 99: return "ProgramUniform1i"; case 100: return "ProgramUniform4fv"; case 101: return "ReadBuffer"; case 102: return "ReadPixels"; case 103: return "RenderbufferStorage"; case 104: return "RenderbufferStorageMultisample"; case 105: return "SamplerParameterf"; case 106: return "SamplerParameterfv"; case 107: return "SamplerParameteri"; case 108: return "Scissor"; case 109: return "ShaderSource"; case 110: return "StencilFuncSeparate"; case 111: return "StencilMask"; case 112: return "StencilOpSeparate"; case 113: return "TexBuffer"; case 114: return "TexImage2D"; case 115: return "TexImage2DMultisample"; case 116: return "TexImage3D"; case 117: return "TexParameterf"; case 118: return "TexParameteri"; case 119: return "TexSubImage2D"; case 120: return "UniformBlockBinding"; case 121: return "UnmapBuffer"; case 122: return "UseProgram"; case 123: return "UseProgramStages"; case 124: return "VertexAttribDivisor"; case 125: return "VertexAttribIPointer"; case 126: return "VertexAttribPointer"; case 127: return "Viewport"; case 128: return "WaitSync"; case 129: return "DeleteVertexArrays"; case 130: return "DeleteRenderbuffers"; case 131: return "DeleteProgram"; case 132: return "DeleteProgramPipelines"; case 133: return "TexSubImage3D"; case 134: return "CopyTexSubImage2D"; case 135: return "PolygonMode"; case 136: return "Uniform1i"; case 137: return "Uniform4fv"; case 138: return "GetActiveUniformBlockiv"; case 139: return "ValidateProgram"; case 140: return "BindAttribLocation"; case 141: return "BindFragDataLocation"; default: return "?"; } }
static void mp_tick(void){   // at every presented frame
    if(op_n[1]||op_n[2]||1) opstats_tick();
    mp_frames++;
    if(mp_mode<0){ const char*e=getenv("PELOAD_GLH_MP"); mp_mode=!e?0:!strncmp(e,"ab",2)?2:atoi(e)?1:0; if(mp_mode==2){ const char*c=strchr(e,':'); mp_ab_s=c?atof(c+1):30; }
        if(mp_mode==1) mp_set(1); mp_t0=now_s(); mp_busy0=H->st_busy_s; mp_f0=mp_frames; mp_d0=g_draws; return; }
    if(mp_mode!=2) return;
    double t=now_s(); if(t-mp_t0<mp_ab_s) return;
    uint64_t fr=mp_frames-mp_f0, dr=g_draws-mp_d0; double busy=H->st_busy_s-mp_busy0;   // (st_busy_s is reset by the loader every 2 s: use wall time)
    (void)busy; double us=dr?(t-mp_t0)*1e6/(double)dr:0;
    mp_tot[mp_on]+=t-mp_t0; mp_totd[mp_on]+=(double)dr;
    fprintf(stderr,"  [glhelper ab] MP engine %s: %.1f fps, %.0f draws/frame, %.2f us per draw (wall) | totals: off %.2f us, on %.2f us per draw\n",mp_on?"on ":"off",
        fr/(t-mp_t0),fr?(double)dr/fr:0,us,mp_totd[0]?mp_tot[0]*1e6/mp_totd[0]:0,mp_totd[1]?mp_tot[1]*1e6/mp_totd[1]:0);
    mp_set(!mp_on); mp_t0=t; mp_f0=mp_frames; mp_d0=g_draws; }
static void pq_blit(const uint64_t*s){
    GLuint src=(GLuint)s[0]; int vw=(int)s[1], vh=(int)s[2], W=(int)s[3], Hh=(int)s[4]; uint64_t fno=s[5];
    if(W!=pq_w||Hh!=pq_h){ while(npq){ glClientWaitSync(pq_pend[0].f,GL_SYNC_FLUSH_COMMANDS_BIT,1000000000ull); glDeleteSync(pq_pend[0].f); pq_pend[0]=pq_pend[--npq]; }
        atomic_store(&H->pq_latest,-1); pq_make(W,Hh); }
    int busy[3]={0}; int rd=atomic_load(&H->pq_reading), lt=atomic_load(&H->pq_latest); if(rd>=0) busy[rd]=1; if(lt>=0) busy[lt]=1; for(int i=0;i<npq;i++) busy[pq_pend[i].slot]=1;
    int slot=-1; for(int i=0;i<3;i++) if(!busy[i]){ slot=i; break; }
    if(slot<0){ atomic_store(&H->frames_done,atomic_load(&H->frames_done)+1); return; }   // all busy: drop this frame
    // the state the blit depends on (the loader resets it after its own present blit too)
    GLint dfb=0, rfb=0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&dfb); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&rfb);
    GLboolean sc=glIsEnabled(GL_SCISSOR_TEST), fs=glIsEnabled(GL_FRAMEBUFFER_SRGB); GLboolean cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK,cm);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB); glColorMask(1,1,1,1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,src); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,pq_fbo[slot]);
    glBlitFramebuffer(0,0,vw,vh, 0,Hh,W,0, GL_COLOR_BUFFER_BIT,GL_LINEAR);   // as pq_publish: D3D row 0 at the top
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)rfb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)dfb);
    if(sc) glEnable(GL_SCISSOR_TEST); if(fs) glEnable(GL_FRAMEBUFFER_SRGB); glColorMask(cm[0],cm[1],cm[2],cm[3]);
    GLsync f=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0); glFlush();
    pq_pend[npq].slot=slot; pq_pend[npq].f=f; pq_pend[npq].frameno=fno; pq_pend[npq].t=now_s(); npq++; mp_tick(); }
static void pq_poll(int wait){
    for(int i=0;i<npq;i++){ GLenum r=glClientWaitSync(pq_pend[i].f,0,wait?2000000ull:0); if(r!=GL_ALREADY_SIGNALED && r!=GL_CONDITION_SATISFIED) continue;
        int s=pq_pend[i].slot; H->pq_frameno[s]=pq_pend[i].frameno; glDeleteSync(pq_pend[i].f); H->st_gpu_s+=now_s()-pq_pend[i].t; H->st_gpu_n++;
        atomic_store(&H->pq_latest,s); atomic_fetch_add(&H->pq_seq,1); atomic_store(&H->frames_done,atomic_load(&H->frames_done)+1);
        pq_pend[i]=pq_pend[--npq]; i--; } }

// ---- one command -----------------------------------------------------------------------------------------------------------
static uint64_t syncs_done;
static void sync_finish(uint64_t rv,uint64_t len){ H->sync_rv=rv; H->sync_len=len; atomic_store_explicit(&H->sync_done,++syncs_done,memory_order_release); }
static volatile int g_lastop=-1; static volatile uint64_t g_ncmd_dbg; static int g_trace=-1;
static void on_crash(int sig){ char m[160]; int k=snprintf(m,sizeof m,"  [glhelper] CRASH signal %d in command #%llu (op %d)\n",sig,(unsigned long long)g_ncmd_dbg,g_lastop); (void)!write(2,m,k); _exit(3); }
static int run_one(const uint8_t*c){
    uint16_t op=((const uint16_t*)c)[0], m=((const uint16_t*)c)[1]; const uint64_t*s=(const uint64_t*)(c+8);
    g_lastop=op; g_ncmd_dbg++;
    if(g_trace<0) g_trace=getenv("PELOAD_GLH_TRACE")?atoi(getenv("PELOAD_GLH_TRACE")):0;
    if(g_trace && g_ncmd_dbg<=(uint64_t)g_trace){ fprintf(stderr,"  [glhelper] #%llu op %u mask 0x%x len %u s0 0x%llx s1 0x%llx s2 0x%llx\n",(unsigned long long)g_ncmd_dbg,op,m,((const uint32_t*)c)[1],
        (unsigned long long)s[0],(unsigned long long)s[1],(unsigned long long)s[2]); }
    uint8_t*reply=glh_reply(H);
    if(op<GLH_OP_GEN_END){
        if(op==GLH_OP_glReadPixels && (m&0x8000)){   // into client memory: the bytes go back in the reply area
            uint64_t n=s[6]; if(n>GLH_REPLY) n=GLH_REPLY;
            glReadPixels((GLint)(int64_t)s[0],(GLint)(int64_t)s[1],(GLsizei)(int64_t)s[2],(GLsizei)(int64_t)s[3],(GLenum)s[4],(GLenum)s[5],reply);
            sync_finish(0,n); return 1; }
        if(op==GLH_OP_glDrawElements||op==GLH_OP_glDrawArrays||op==GLH_OP_glDrawElementsBaseVertex||op==GLH_OP_glDrawElementsInstanced||
           op==GLH_OP_glDrawArraysInstanced||op==GLH_OP_glDrawElementsInstancedBaseVertex) g_draws++;
        static int ops=-1; if(ops<0) ops=getenv("PELOAD_GLH_OPSTATS")?1:0;
        uint64_t rv=0; int r;
        if(ops){ uint64_t t0=mach_absolute_time(); r=glh_exec_gen(op,m,s,0,reply,&rv); uint64_t dt=mach_absolute_time()-t0; op_n[op]++; op_t[op]+=dt;
            if(op==GLH_OP_glBlitFramebuffer && ops<60){ static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); double us=dt*(double)tb.numer/tb.denom/1e3;
                if(us>200){ ops++; GLint rf=0,df=0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&rf); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&df);
                    fprintf(stderr,"  [glhelper blit] %.0f us: fbo %d (%lld,%lld %lldx%lld) -> fbo %d (%lld,%lld %lldx%lld) mask 0x%llx filter 0x%llx\n",us,rf,(long long)s[0],(long long)s[1],(long long)(s[2]-s[0]),(long long)(s[3]-s[1]),
                        df,(long long)s[4],(long long)s[5],(long long)(s[6]-s[4]),(long long)(s[7]-s[5]),(unsigned long long)s[8],(unsigned long long)s[9]); } } }
        else r=glh_exec_gen(op,m,s,0,reply,&rv);
        if(r==1){ sync_finish(rv,0); return 1; }
        if(r<0){ char e[80]; snprintf(e,sizeof e,"unknown GL command %u",op); fail(e); }
        return 0; }
    switch(op){
    case GLH_X_POOLFILL: pools_fill(); sync_finish(0,0); return 1;
    case GLH_X_TFVARYINGS: { const char*nm[64]; int n=(int)s[1]; if(n>64) n=64; const char*q=(const char*)(s+3);
        for(int i=0;i<n;i++){ nm[i]=q; q+=strlen(q)+1; } glTransformFeedbackVaryings((GLuint)s[0],n,nm,(GLenum)s[2]); return 0; }
    case GLH_X_SHADERSOURCE: { const GLchar*src=(const GLchar*)(s+2); GLint n=(GLint)s[1]; glShaderSource((GLuint)s[0],1,&src,&n); return 0; }
    case GLH_X_MAPWRITE: { void*p=glMapBufferRange((GLenum)s[0],(GLintptr)s[1],(GLsizeiptr)s[2],(GLbitfield)s[3]); if(p){ memcpy(p,s+4,(size_t)s[2]); glUnmapBuffer((GLenum)s[0]); } return 0; }
    case GLH_X_MAPREAD: { uint64_t n=s[2]>GLH_REPLY?GLH_REPLY:s[2]; void*p=glMapBufferRange((GLenum)s[0],(GLintptr)s[1],(GLsizeiptr)n,GL_MAP_READ_BIT);
        if(p){ memcpy(reply,p,(size_t)n); glUnmapBuffer((GLenum)s[0]); } else memset(reply,0,(size_t)n); sync_finish(0,n); return 1; }
    case GLH_X_FENCE: { uint64_t id=s[0]; if(fences[id%NFENCE].s) glDeleteSync(fences[id%NFENCE].s); fences[id%NFENCE].id=id; fences[id%NFENCE].s=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0); return 0; }
    case GLH_X_WAITSYNC: { GLsync f=fence_get(s[0]); if(f) glWaitSync(f,0,GL_TIMEOUT_IGNORED); return 0; }
    case GLH_X_DELSYNC: { uint64_t id=s[0]; if(fences[id%NFENCE].id==id && fences[id%NFENCE].s){ glDeleteSync(fences[id%NFENCE].s); fences[id%NFENCE].s=0; } return 0; }
    case GLH_X_GETERROR: sync_finish(glGetError(),0); return 1;
    case GLH_X_GETSTRING: { const GLubyte*str=glGetString((GLenum)s[0]); size_t n=str?strlen((const char*)str)+1:0; if(n>4096) n=4096; if(n){ memcpy(reply,str,n); reply[n-1]=0; } sync_finish(n?1:0,n); return 1; }
    case GLH_X_CHECKFB: sync_finish(glCheckFramebufferStatus((GLenum)s[0]),0); return 1;
    case GLH_X_FINISH: glFinish(); sync_finish(0,0); return 1;
    case GLH_X_NOP_SYNC: sync_finish(0,0); return 1;
    case GLH_X_BEGINQUERY: { glBeginQuery((GLenum)s[0],(GLuint)s[1]); int t=s[0]==GL_SAMPLES_PASSED?0:1; qactive[t].id=(GLuint)s[1]; qactive[t].gen=(uint32_t)s[2]; return 0; }
    case GLH_X_ENDQUERY: { glEndQuery((GLenum)s[0]); int t=s[0]==GL_SAMPLES_PASSED?0:1;
        if(qactive[t].id){ if(qp_head-qp_tail>=QP) qp_tail++; qpend[qp_head%QP].id=qactive[t].id; qpend[qp_head%QP].gen=qactive[t].gen; qp_head++; } qactive[t].id=0; return 0; }
    case GLH_X_CREATESHADER: sync_finish(glCreateShader((GLenum)s[0]),0); return 1;
    case GLH_X_GETTEXIMAGE: { GLenum t=(GLenum)s[0]; GLint lv=(GLint)(int64_t)s[1]; GLint w=0,h=0,d=0;
        GLenum qt=(t>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&t<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)?t:t;
        glGetTexLevelParameteriv(qt,lv,GL_TEXTURE_WIDTH,&w); glGetTexLevelParameteriv(qt,lv,GL_TEXTURE_HEIGHT,&h); glGetTexLevelParameteriv(qt,lv,GL_TEXTURE_DEPTH,&d);
        uint64_t n=(uint64_t)(w>0?w:0)*(h>0?h:0)*(d>0?d:1)*16; if(n>GLH_REPLY) n=0;   // generous bound; the real size is reported below
        GLint pa=4; glGetIntegerv(GL_PACK_ALIGNMENT,&pa);
        uint64_t bpp=16; { GLenum f=(GLenum)s[2], ty=(GLenum)s[3]; int comps=(f==GL_RED||f==GL_DEPTH_COMPONENT||f==GL_RED_INTEGER)?1:(f==GL_RG||f==GL_DEPTH_STENCIL||f==GL_RG_INTEGER)?2:(f==GL_RGB||f==GL_BGR)?3:4;
            bpp=(ty==GL_UNSIGNED_BYTE||ty==GL_BYTE)?comps:(ty==GL_HALF_FLOAT||ty==GL_SHORT||ty==GL_UNSIGNED_SHORT)?comps*2:(ty==GL_UNSIGNED_INT_24_8||ty==GL_UNSIGNED_INT_8_8_8_8_REV)?4:(ty==GL_FLOAT_32_UNSIGNED_INT_24_8_REV)?8:comps*4; }
        uint64_t row=((uint64_t)w*bpp+pa-1)/pa*pa; uint64_t need=row*(uint64_t)h*(d>0?d:1);
        if(n && need<=GLH_REPLY){ glGetTexImage(t,lv,(GLenum)s[2],(GLenum)s[3],reply); sync_finish(0,need); } else sync_finish(0,0);
        return 1; }
    case GLH_X_LOCS: { GLuint prog=(GLuint)s[0]; int n=(int)s[1], fb=(int)s[2]; const char*nm=(const char*)(s+3); int*out=(int*)reply;
        for(int i=0;i<n && (uint64_t)i*4<GLH_REPLY;i++){ out[i]=i<fb?glGetUniformLocation(prog,nm):(int)glGetUniformBlockIndex(prog,nm); nm+=strlen(nm)+1; }
        sync_finish(0,(uint64_t)n*4); return 1; }
    case GLH_X_READBACK: { GLint prev=0; glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&prev); glBindBuffer(GL_PIXEL_PACK_BUFFER,(GLuint)s[0]);
        uint64_t n=s[1], off=s[2]; if(off+n<=GLH_REPLY-GLH_MAILBOX){ const void*p=glMapBufferRange(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)n,GL_MAP_READ_BIT);
            if(p){ memcpy(glh_reply(H)+GLH_MAILBOX+off,p,(size_t)n); glUnmapBuffer(GL_PIXEL_PACK_BUFFER); } }
        glBindBuffer(GL_PIXEL_PACK_BUFFER,(GLuint)prev); atomic_store_explicit(&H->rb_done,s[3],memory_order_release); return 0; }
    case GLH_X_PQ_BLIT: pq_blit(s); return 0;
    default: { char e[80]; snprintf(e,sizeof e,"unknown command %u",op); fail(e); } }
    return 0; }

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"usage: glhelper <shm> <wake fd> <loader pid>\n"); return 2; }
    int fd=shm_open(argv[1],O_RDWR,0600); if(fd<0){ perror("shm_open"); return 1; }
    void*mp=mmap(0,glh_shm_size(),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd); if(mp==MAP_FAILED){ perror("mmap"); return 1; }
    H=mp; if(H->magic!=GLH_MAGIC||H->version!=GLH_VERSION) fail("shared memory version mismatch (rebuild both: bash shim/build.sh)");
    int wake=atoi(argv[2]); pid_t loader=(pid_t)atoi(argv[3]);
    signal(SIGSEGV,on_crash); signal(SIGBUS,on_crash); signal(SIGABRT,on_crash); signal(SIGILL,on_crash);
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,(CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,(CGLPixelFormatAttribute)8,kCGLPFADepthSize,(CGLPixelFormatAttribute)24,kCGLPFAStencilSize,(CGLPixelFormatAttribute)8,(CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf; GLint np=0; if(CGLChoosePixelFormat(at,&pf,&np)||!pf) fail("CGLChoosePixelFormat failed");
    if(CGLCreateContext(pf,0,&ctx)||!ctx) fail("CGLCreateContext failed"); CGLDestroyPixelFormat(pf); CGLSetCurrentContext(ctx);
    pools_fill();
    { GLuint vao; glGenVertexArrays(1,&vao); glBindVertexArray(vao); }   // core profile: a VAO must be bound (the loader binds its own)
    atomic_store(&H->ready,1);
    fprintf(stderr,"  [glhelper] running (native %s, %s)\n",
#if defined(__arm64__)
        "arm64",
#else
        "x86_64",
#endif
        (const char*)glGetString(GL_RENDERER));
    // the wake pipe doubles as the loader's lifeline: it reads end-of-file once the loader is gone
    uint64_t tail=atomic_load(&H->tail), ncmd=0; double busy0=now_s(), lastpoll=0;
    mach_timebase_info_data_t tbi; mach_timebase_info(&tbi); uint64_t tb_num=tbi.numer, tb_den=tbi.denom;
    (void)wake;
    for(;;){
        uint64_t head=atomic_load_explicit(&H->head,memory_order_acquire);
        if(head==tail){
            pq_poll(0); queries_poll(); pools_fill();
            atomic_store_explicit(&H->tail,tail,memory_order_release);
            H->st_busy_s+=now_s()-busy0;
            uint32_t seq=atomic_load(&H->wake_seq);
            atomic_store(&H->helper_sleeping,1);   // seq_cst, then a seq_cst load of head: pairs with the loader's store head / load sleeping
            if(atomic_load(&H->head)==tail){
                int t=npq||qp_head!=qp_tail?1:20;   // ms: sooner while a frame or a query result is pending
                glh_addr_wait_ms(&H->wake_seq,seq,t,tb_num,tb_den);
                { static double lk; double tn=now_s(); if(tn-lk>0.25){ lk=tn; if(kill(loader,0) && errno==ESRCH) _exit(0); } } }   // the loader is gone
            atomic_store(&H->helper_sleeping,0); busy0=now_s(); continue; }
        while(tail!=head){
            const uint8_t*c=glh_ring(H)+tail%GLH_RING; uint16_t op=((const uint16_t*)c)[0]; uint32_t len=((const uint32_t*)c)[1];
            if(op==GLH_X_WRAP){ tail+=GLH_RING-tail%GLH_RING; continue; }
            if(!len) fail("corrupt ring (zero-length command)");
            run_one(c); tail+=len; ncmd++; H->st_cmds=ncmd;
            if((ncmd&255)==0){ atomic_store_explicit(&H->tail,tail,memory_order_release); double t=now_s(); H->st_busy_s+=t-busy0; busy0=t; if(t-lastpoll>0.002){ lastpoll=t; pq_poll(0); queries_poll(); } }
            if((ncmd&4095)==0) pools_fill(); }
        atomic_store_explicit(&H->tail,tail,memory_order_release); }
}
