// glbench: what Apple's OpenGL driver costs per draw on the CPU, built for x86_64 (Rosetta, like the loader) and for
// arm64 (native, like a GL helper process would be). The draw loop mimics the loader's per-draw GL calls in a big
// Halo 3 game (shim/docs/PERF-PLAN.md): texture + sampler binds on several units, a uniform-buffer range bind, a
// program change every few draws, depth/blend toggles, an indexed draw of a small mesh into a 1920x1080 FBO.
//     bash shim/tools/glbench.sh                 # builds both, runs both, prints us/draw
//     glbench [draws/frame] [frames] [textures per draw] [constant buffers per draw]
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now_us(void){ static mach_timebase_info_data_t tb; if(!tb.denom) mach_timebase_info(&tb); return mach_absolute_time()*(double)tb.numer/tb.denom/1000.0; }
static GLuint sh(GLenum t,const char*src){ GLuint s=glCreateShader(t); glShaderSource(s,1,&src,0); glCompileShader(s); GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[2000]; glGetShaderInfoLog(s,sizeof log,0,log); fprintf(stderr,"shader: %s\n",log); exit(1); } return s; }
static GLuint prog(int variant){
    char fs[2000]; snprintf(fs,sizeof fs,
        "#version 410 core\nuniform sampler2D t0,t1,t2,t3,t4,t5,t6,t7;\nlayout(std140) uniform CB{ vec4 c[16]; };\nin vec2 uv; out vec4 o;\n"
        "void main(){ o=texture(t0,uv)+texture(t1,uv)*c[0]+texture(t2,uv)*c[1]+texture(t3,uv)*c[2]+texture(t4,uv)+texture(t5,uv)+texture(t6,uv)+texture(t7,uv)*%d.0; }\n",variant+1);
    const char*vs="#version 410 core\nlayout(location=0) in vec3 p; layout(location=1) in vec2 t; layout(std140) uniform CB{ vec4 c[16]; }; out vec2 uv;\n"
        "void main(){ uv=t; gl_Position=vec4(p*0.1+c[3].xyz*0.0,1.0); }\n";
    GLuint pr=glCreateProgram(); glAttachShader(pr,sh(GL_VERTEX_SHADER,vs)); glAttachShader(pr,sh(GL_FRAGMENT_SHADER,fs)); glLinkProgram(pr);
    GLint ok=0; glGetProgramiv(pr,GL_LINK_STATUS,&ok); if(!ok){ fprintf(stderr,"link failed\n"); exit(1); }
    glUseProgram(pr); for(int i=0;i<8;i++){ char n[4]={'t',(char)('0'+i),0}; glUniform1i(glGetUniformLocation(pr,n),i); }
    glUniformBlockBinding(pr,glGetUniformBlockIndex(pr,"CB"),0); return pr; }

int main(int argc,char**argv){
    int dpf=argc>1?atoi(argv[1]):3000, frames=argc>2?atoi(argv[2]):60, tpd=argc>3?atoi(argv[3]):6, cbpd=argc>4?atoi(argv[4]):2; if(tpd>8) tpd=8; if(cbpd<1) cbpd=1; if(cbpd>4) cbpd=4;
    CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,kCGLPFAColorSize,24,kCGLPFADepthSize,24,0};
    CGLPixelFormatObj pf; GLint np=0; CGLChoosePixelFormat(at,&pf,&np); CGLContextObj ctx; if(CGLCreateContext(pf,0,&ctx)){ fprintf(stderr,"no GL context\n"); return 1; }
    CGLSetCurrentContext(ctx);
    // render target: 1920x1080 RGBA8 + depth
    GLuint fbo, col, dep; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenTextures(1,&col); glBindTexture(GL_TEXTURE_2D,col); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1920,1080,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,col,0);
    glGenTextures(1,&dep); glBindTexture(GL_TEXTURE_2D,dep); glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,1920,1080,0,GL_DEPTH_COMPONENT,GL_FLOAT,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,dep,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){ fprintf(stderr,"fbo incomplete\n"); return 1; }
    glViewport(0,0,1920,1080);
    // 64 textures, 8 samplers, 8 programs, a mesh of 64 triangles, a 4 MB uniform ring
    enum { NT=64, NS=8, NP=8 }; GLuint tex[NT], smp[NS], pr[NP]; unsigned char*px=malloc(256*256*4); for(int i=0;i<256*256*4;i++) px[i]=(unsigned char)(i*7);
    glGenTextures(NT,tex); for(int i=0;i<NT;i++){ glBindTexture(GL_TEXTURE_2D,tex[i]); glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,px); glGenerateMipmap(GL_TEXTURE_2D); }
    glGenSamplers(NS,smp); for(int i=0;i<NS;i++){ glSamplerParameteri(smp[i],GL_TEXTURE_MIN_FILTER,i&1?GL_LINEAR_MIPMAP_LINEAR:GL_LINEAR); glSamplerParameteri(smp[i],GL_TEXTURE_WRAP_S,i&2?GL_CLAMP_TO_EDGE:GL_REPEAT); }
    for(int i=0;i<NP;i++) pr[i]=prog(i);
    for(int u=0;u<8;u++){ glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_2D,tex[u]); glBindSampler(u,smp[0]); }
    float vtx[64*3*5]; for(int i=0;i<64*3*5;i++) vtx[i]=(float)((i*37)%17)/17.0f-0.5f; unsigned short idx[64*3]; for(int i=0;i<64*3;i++) idx[i]=(unsigned short)i;
    GLuint vao, vb, ib, ub; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vb); glBindBuffer(GL_ARRAY_BUFFER,vb); glBufferData(GL_ARRAY_BUFFER,sizeof vtx,vtx,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,0); glEnableVertexAttribArray(0); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12); glEnableVertexAttribArray(1);
    glGenBuffers(1,&ib); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ib); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof idx,idx,GL_STATIC_DRAW);
    const int UBSZ=4<<20; glGenBuffers(1,&ub); glBindBuffer(GL_UNIFORM_BUFFER,ub); glBufferData(GL_UNIFORM_BUFFER,UBSZ,0,GL_DYNAMIC_DRAW);
    float cb[64]; for(int i=0;i<64;i++) cb[i]=0.01f*i;
    double best=1e30, sum=0; int n=0; unsigned seed=1; int upo=0;
    for(int f=0;f<frames+5;f++){
        double t0=now_us();
        glBindFramebuffer(GL_FRAMEBUFFER,fbo); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
        for(int d=0; d<dpf; d++){
            seed=seed*1103515245u+12345u;
            if(d%4==0) glUseProgram(pr[(seed>>8)%NP]);                                   // a program change every few draws
            for(int u=0;u<tpd;u++){ glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_2D,tex[((seed>>(u*3))+d)%NT]); glBindSampler(u,smp[(seed>>(u+5))%NS]); }
            // constants: write 256 bytes into the ring, bind that range (what the loader's cb ring does per draw)
            // the loader's cb ring: map the next range unsynchronized, copy, unmap, bind that range (d3dgl_ring_put)
            for(int k=0;k<cbpd;k++){ if(upo+256>UBSZ) upo=0;
                void*m=glMapBufferRange(GL_UNIFORM_BUFFER,upo,256,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
                if(m){ memcpy(m,cb,256); glUnmapBuffer(GL_UNIFORM_BUFFER); } glBindBufferRange(GL_UNIFORM_BUFFER,k?k+1:0,ub,upo,256); upo+=256; }
            if(d%16==0){ if((seed>>12)&1) glEnable(GL_BLEND); else glDisable(GL_BLEND); glDepthMask((seed>>13)&1); }
            glDrawElements(GL_TRIANGLES,(GLsizei)(((seed>>16)%64+1)*3),GL_UNSIGNED_SHORT,0);
        }
        glFlush();
        double cpu=now_us()-t0;
        glFinish();
        if(f>=5){ sum+=cpu; n++; if(cpu<best) best=cpu; }
    }
    GLenum e=glGetError();
    printf("%-7s %d draws/frame, %d textures + %d cbs/draw: CPU %.2f us/draw (mean), %.2f us/draw (best frame), %.1f ms/frame%s\n",
#if defined(__x86_64__)
        "x86_64",
#else
        "arm64",
#endif
        dpf,tpd,cbpd,sum/n/dpf,best/dpf,sum/n/1000,e?" [GL error]":"");
    return 0; }
