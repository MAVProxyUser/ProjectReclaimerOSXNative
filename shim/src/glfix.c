// glfix.dylib — DYLD interpose that rewrites GLSL "#version <150" to 150 in
// shader sources, so Reclaimer's #version 130 shaders compile on macOS Core
// profile (which rejects <150). Injected via DYLD_INSERT_LIBRARIES; works no
// matter how the engine obtained the GL function pointers, because it replaces
// the framework's own glShaderSource / glCreateShaderProgramv.
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

typedef unsigned GLuint; typedef int GLsizei; typedef unsigned GLenum;

static void bump_version(char *s){
    char *v=strstr(s,"#version "); if(!v) return; char *d=v+9; while(*d==' ')d++;
    if(d[0]>='1'&&d[0]<='9'&&d[1]>='0'&&d[1]<='9'&&d[2]>='0'&&d[2]<='9'){
        int ver=(d[0]-'0')*100+(d[1]-'0')*10+(d[2]-'0');
        if(ver<150){ d[0]='1';d[1]='5';d[2]='0'; } }
}
// build modified copies of the source strings (version bumped)
static char **dup_fix(GLsizei count, const char*const*strings, const int*lengths, char ***outbufs){
    char **ns=malloc(sizeof(char*)*count); char **bufs=malloc(sizeof(char*)*count); *outbufs=bufs;
    for(int i=0;i<count;i++){ const char*s=strings[i]; int L=lengths?(lengths[i]>=0?lengths[i]:(int)strlen(s)):(int)strlen(s);
        char*b=malloc(L+1); memcpy(b,s,L); b[L]=0; bump_version(b); ns[i]=b; bufs[i]=b; }
    return ns;
}

static void my_glShaderSource(GLuint sh, GLsizei count, const char*const* strings, const int* lengths){
    void(*real)(GLuint,GLsizei,const char*const*,const int*)=dlsym(RTLD_NEXT,"glShaderSource");
    if(!real||count<=0){ if(real) real(sh,count,strings,lengths); return; }
    char **bufs; char **ns=dup_fix(count,strings,lengths,&bufs);
    real(sh,count,(const char*const*)ns,0);   // NUL-terminated copies
    (void)!write(2,"  [glfix] glShaderSource version-bumped\n",40);
    for(int i=0;i<count;i++) free(bufs[i]); free(ns); free(bufs);
}
static GLuint my_glCreateShaderProgramv(GLenum type, GLsizei count, const char*const* strings){
    GLuint(*real)(GLenum,GLsizei,const char*const*)=dlsym(RTLD_NEXT,"glCreateShaderProgramv");
    if(!real) return 0; if(count<=0) return real(type,count,strings);
    char **bufs; char **ns=dup_fix(count,strings,0,&bufs);
    GLuint r=real(type,count,(const char*const*)ns);
    (void)!write(2,"  [glfix] glCreateShaderProgramv version-bumped\n",48);
    for(int i=0;i<count;i++) free(bufs[i]); free(ns); free(bufs);
    return r;
}

extern void glShaderSource(GLuint,GLsizei,const char*const*,const int*);
extern GLuint glCreateShaderProgramv(GLenum,GLsizei,const char*const*);
__attribute__((used)) static struct{ const void*repl,*orig; }
_ip_ss __attribute__((section("__DATA,__interpose"))) = {(const void*)my_glShaderSource,(const void*)glShaderSource},
_ip_csp __attribute__((section("__DATA,__interpose"))) = {(const void*)my_glCreateShaderProgramv,(const void*)glCreateShaderProgramv};
