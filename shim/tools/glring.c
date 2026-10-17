// glring: the native GL helper experiment (PERF-PLAN option 1). The same draw stream as glbench (texture + sampler
// binds, uniform-buffer uploads, a program change every few draws, blend/depth toggles, indexed draws into a 1080p
// FBO), but split across two processes like the planned helper:
//   producer (x86_64, under Rosetta like the loader): encodes each GL call into a shared-memory ring;
//   consumer (arm64, native): maps the ring, owns the GL context and replays the calls.
// It reports the producer's cost per draw (what the loader's GL thread would spend), the consumer's (Apple's driver,
// native), and the frame rate with both running in parallel -- against glbench's in-process x86_64 number.
//     bash shim/tools/glring.sh [draws/frame] [frames] [textures per draw] [constant buffers per draw]
//     glring consume <shm name> | glring produce <shm name> [draws] [frames] [tpd] [cbpd]
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RING = 64 << 20 };
typedef struct {                       // shared header; the command bytes follow (same layout on both architectures)
    _Atomic uint64_t head, tail;       // byte positions (monotonic): producer writes at head, consumer reads at tail
    _Atomic uint32_t frames_done, ready, quit;
    uint32_t dpf, frames, tpd, cbpd;
    double cons_us;                    // consumer: CPU time spent replaying (sum over measured frames)
    uint8_t pad[64];
} Hdr;
enum { OP_PROG = 1, OP_TEX, OP_UBO, OP_STATE, OP_DRAW, OP_FRAME, OP_END };

static double now_us(void){ static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); return mach_absolute_time()*(double)tb.numer/tb.denom/1000.0; }

static Hdr* map_ring(const char*name,int create){
    int fd=shm_open(name,create?O_CREAT|O_RDWR:O_RDWR,0600); if(fd<0){ perror("shm_open"); exit(1); }
    if(create && ftruncate(fd,sizeof(Hdr)+RING)){ perror("ftruncate"); exit(1); }
    void*p=mmap(0,sizeof(Hdr)+RING,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd); if(p==MAP_FAILED){ perror("mmap"); exit(1); }
    return p; }
static uint8_t* ring_data(Hdr*h){ return (uint8_t*)(h+1); }

// ---- producer ------------------------------------------------------------------------------------------------------
static Hdr*H; static uint8_t*R; static uint64_t wpos;
static void put(const void*p,uint32_t n){   // n <= a few KB; waits while the consumer is a ring behind
    while(wpos+n-atomic_load_explicit(&H->tail,memory_order_acquire) > RING) sched_yield();
    uint64_t o=wpos%RING; if(o+n<=RING) memcpy(R+o,p,n); else { uint32_t a=(uint32_t)(RING-o); memcpy(R+o,p,a); memcpy(R,(const uint8_t*)p+a,n-a); }
    wpos+=n; }
static void publish(void){ atomic_store_explicit(&H->head,wpos,memory_order_release); }

static int produce(const char*name,int dpf,int frames,int tpd,int cbpd){
    H=map_ring(name,0); R=ring_data(H); while(!atomic_load(&H->ready)) usleep(1000);
    float cb[64]; for(int i=0;i<64;i++) cb[i]=0.01f*i;
    double best=1e30, sum=0, wall0=0; int n=0; unsigned seed=1;
    for(int f=0;f<frames+5;f++){
        if(f==5) wall0=now_us();
        double t0=now_us();
        for(int d=0; d<dpf; d++){
            seed=seed*1103515245u+12345u;
            if(d%4==0){ uint32_t c[2]={OP_PROG,(seed>>8)%8}; put(c,8); }
            for(int u=0;u<tpd;u++){ uint32_t c[4]={OP_TEX,(uint32_t)u,((seed>>(u*3))+d)%64,(seed>>(u+5))%8}; put(c,16); }
            for(int k=0;k<cbpd;k++){ uint32_t c[2]={OP_UBO,(uint32_t)k}; put(c,8); put(cb,256); }
            if(d%16==0){ uint32_t c[3]={OP_STATE,(seed>>12)&1,(seed>>13)&1}; put(c,12); }
            uint32_t c[2]={OP_DRAW,((seed>>16)%64+1)*3}; put(c,8);
            if((d&63)==63) publish();
        }
        uint32_t fe[1]={OP_FRAME}; put(fe,4); publish();
        double cpu=now_us()-t0;
        // at most 2 frames ahead of the consumer (as the loader's submission thread allows one or two)
        while((int)f-(int)atomic_load_explicit(&H->frames_done,memory_order_acquire) >= 2) sched_yield();
        if(f>=5){ sum+=cpu; n++; if(cpu<best) best=cpu; }
    }
    uint32_t end[1]={OP_END}; put(end,4); publish();
    while(atomic_load(&H->frames_done)<(uint32_t)(frames+5)) usleep(500);
    double wall=now_us()-wall0;
    printf("producer x86_64 (Rosetta): %.2f us/draw encoding (mean), %.2f (best frame)\n",sum/n/dpf,best/dpf);
    printf("consumer arm64 (native GL): %.2f us/draw replaying\n",H->cons_us/frames/dpf);
    printf("pipelined: %.1f ms/frame = %.0f fps at %d draws/frame (%.2f us/draw end to end)\n",wall/frames/1000,1e6/(wall/frames),dpf,wall/frames/dpf);
    atomic_store(&H->quit,1); return 0; }

// ---- consumer ------------------------------------------------------------------------------------------------------
static GLuint sh(GLenum t,const char*src){ GLuint s=glCreateShader(t); glShaderSource(s,1,&src,0); glCompileShader(s); GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[2000]; glGetShaderInfoLog(s,sizeof log,0,log); fprintf(stderr,"shader: %s\n",log); exit(1); } return s; }
static GLuint prog(int variant){   // as glbench
    char fs[2000]; snprintf(fs,sizeof fs,
        "#version 410 core\nuniform sampler2D t0,t1,t2,t3,t4,t5,t6,t7;\nlayout(std140) uniform CB{ vec4 c[16]; };\nin vec2 uv; out vec4 o;\n"
        "void main(){ o=texture(t0,uv)+texture(t1,uv)*c[0]+texture(t2,uv)*c[1]+texture(t3,uv)*c[2]+texture(t4,uv)+texture(t5,uv)+texture(t6,uv)+texture(t7,uv)*%d.0; }\n",variant+1);
    const char*vs="#version 410 core\nlayout(location=0) in vec3 p; layout(location=1) in vec2 t; layout(std140) uniform CB{ vec4 c[16]; }; out vec2 uv;\n"
        "void main(){ uv=t; gl_Position=vec4(p*0.1+c[3].xyz*0.0,1.0); }\n";
    GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,vs)); glAttachShader(pr,sh(GL_FRAGMENT_SHADER,fs)); glLinkProgram(pr);
    glUseProgram(pr); for(int i=0;i<8;i++){ char n[4]={'t',(char)('0'+i),0}; glUniform1i(glGetUniformLocation(pr,n),i); }
    glUniformBlockBinding(pr,glGetUniformBlockIndex(pr,"CB"),0); return pr; }
static uint64_t rpos;
static void get(void*p,uint32_t n){
    while(atomic_load_explicit(&H->head,memory_order_acquire)-rpos < n) sched_yield();
    uint64_t o=rpos%RING; if(o+n<=RING) memcpy(p,R+o,n); else { uint32_t a=(uint32_t)(RING-o); memcpy(p,R+o,a); memcpy((uint8_t*)p+a,R,n-a); }
    rpos+=n; if((rpos&0xFFFF)<n) atomic_store_explicit(&H->tail,rpos,memory_order_release); }
static int consume(const char*name){
    shm_unlink(name); H=map_ring(name,1); R=ring_data(H); memset(H,0,sizeof *H);
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,24,kCGLPFADepthSize,24,0};
    CGLPixelFormatObj pf; GLint np=0; CGLChoosePixelFormat(at,&pf,&np); CGLContextObj ctx; if(CGLCreateContext(pf,0,&ctx)){ fprintf(stderr,"no GL context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    GLuint fbo,col,dep; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&col); glBindTexture(GL_TEXTURE_2D,col); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1920,1080,0,GL_RGBA,GL_UNSIGNED_BYTE,0); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,col,0);
    glGenTextures(1,&dep); glBindTexture(GL_TEXTURE_2D,dep); glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,1920,1080,0,GL_DEPTH_COMPONENT,GL_FLOAT,0); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,dep,0);
    glViewport(0,0,1920,1080);
    enum { NT=64, NS=8, NP=8 }; GLuint tex[NT], smp[NS], pr[NP]; unsigned char*px=malloc(256*256*4); for(int i=0;i<256*256*4;i++) px[i]=(unsigned char)(i*7);
    glGenTextures(NT,tex); for(int i=0;i<NT;i++){ glBindTexture(GL_TEXTURE_2D,tex[i]); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,px); glGenerateMipmap(GL_TEXTURE_2D); }
    glGenSamplers(NS,smp); for(int i=0;i<NS;i++){ glSamplerParameteri(smp[i],GL_TEXTURE_MIN_FILTER,i&1?GL_LINEAR_MIPMAP_LINEAR:GL_LINEAR); glSamplerParameteri(smp[i],GL_TEXTURE_WRAP_S,i&2?GL_CLAMP_TO_EDGE:GL_REPEAT); }
    for(int i=0;i<NP;i++) pr[i]=prog(i);
    for(int u=0;u<8;u++){ glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_2D,tex[u]); glBindSampler(u,smp[0]); }   // as glbench
    float vtx[64*3*5]; for(int i=0;i<64*3*5;i++) vtx[i]=(float)((i*37)%17)/17.0f-0.5f; unsigned short idx[64*3]; for(int i=0;i<64*3;i++) idx[i]=(unsigned short)i;
    GLuint vao,vb,ib,ub; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY_BUFFER,vb); glBufferData(GL_ARRAY_BUFFER,sizeof vtx,vtx,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,0); glEnableVertexAttribArray(0); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12); glEnableVertexAttribArray(1);
    glGenBuffers(1,&ib); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ib); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
    const int UBSZ=4<<20; glGenBuffers(1,&ub); glBindBuffer(GL_UNIFORM_BUFFER,ub); glBufferData(GL_UNIFORM_BUFFER,UBSZ,0,GL_DYNAMIC_DRAW);
    int upo=0; uint32_t fdone=0; double t0=0, sum=0; uint8_t cb[256];
    atomic_store(&H->ready,1);
    for(;;){
        if(!t0){ t0=now_us(); glBindFramebuffer(GL_FRAMEBUFFER,fbo); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST); }
        uint32_t op; get(&op,4);
        if(op==OP_PROG){ uint32_t i; get(&i,4); glUseProgram(pr[i%NP]); }
        else if(op==OP_TEX){ uint32_t a[3]; get(a,12); glActiveTexture(GL_TEXTURE0+a[0]); glBindTexture(GL_TEXTURE_2D,tex[a[1]%NT]); glBindSampler(a[0],smp[a[2]%NS]); }
        else if(op==OP_UBO){ uint32_t k; get(&k,4); get(cb,256); if(upo+256>UBSZ) upo=0;
            void*m=glMapBufferRange(GL_UNIFORM_BUFFER,upo,256,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
            if(m){ memcpy(m,cb,256); glUnmapBuffer(GL_UNIFORM_BUFFER); } glBindBufferRange(GL_UNIFORM_BUFFER,k?k+1:0,ub,upo,256); upo+=256; }
        else if(op==OP_STATE){ uint32_t a[2]; get(a,8); if(a[0]) glEnable(GL_BLEND); else glDisable(GL_BLEND); glDepthMask(a[1]); }
        else if(op==OP_DRAW){ uint32_t c; get(&c,4); glDrawElements(GL_TRIANGLES,(GLsizei)c,GL_UNSIGNED_SHORT,0); }
        else if(op==OP_FRAME){ glFlush(); double cpu=now_us()-t0; if(fdone>=5) sum+=cpu; t0=0; fdone++; H->cons_us=sum; atomic_store_explicit(&H->frames_done,fdone,memory_order_release); }
        else if(op==OP_END){ glFinish(); break; }
        else { fprintf(stderr,"bad op %u at %llu\n",op,(unsigned long long)rpos); return 1; }
    }
    while(!atomic_load(&H->quit)) usleep(1000);
    shm_unlink(name); return 0; }

int main(int argc,char**argv){
    if(argc>=3 && !strcmp(argv[1],"consume")) return consume(argv[2]);
    if(argc>=3 && !strcmp(argv[1],"produce")) return produce(argv[2],argc>3?atoi(argv[3]):3000,argc>4?atoi(argv[4]):60,argc>5?atoi(argv[5]):6,argc>6?atoi(argv[6]):2);
    fprintf(stderr,"usage: glring consume <shm> | glring produce <shm> [draws] [frames] [tpd] [cbpd]\n"); return 2; }
