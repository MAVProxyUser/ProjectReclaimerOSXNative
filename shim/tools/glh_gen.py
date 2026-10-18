#!/usr/bin/env python3
"""Generates the native GL helper's call encoders and decoders (shim/docs/PERF-PLAN.md, option 1).

    python3 shim/tools/glh_gen.py        # writes shim/src/glh_gen.inc (loader side) and shim/src/glh_dec.inc (helper side)

The loader resolves every GL entry point through dlsym; in helper mode (PELOAD_GL_HELPER=1) each name in FUNCS resolves
to a generated router that encodes the call into the shared-memory ring when the render context is current (and calls
the real function otherwise). The helper (shim/src/glhelper.c, native arm64) decodes and runs them.

Wire format (8-byte aligned): header {u16 op, u16 payload mask, u32 total bytes}, one u64 slot per argument, then the
payloads in argument order. A pointer argument whose bit is set in the mask carries its byte length in its slot and its
bytes as a payload; otherwise its slot holds the pointer value itself (a buffer offset, or NULL).
Calls with results (return values or output pointers) are synchronous: the loader waits until the helper has run them;
the return value and the outputs come back through the reply area.
Hand-written (SPECIAL below): names (Gen*/Create*), strings, buffer mapping, fences, queries, GetError/GetString,
CheckFramebufferStatus, Finish, the pixel-store / buffer-binding tracking that payload sizes need."""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
GL3 = os.path.join(SDK, 'System/Library/Frameworks/OpenGL.framework/Headers/gl3.h')

FUNCS = """glActiveTexture glAttachShader glBeginQuery glBindBuffer glBindBufferRange glBindFramebuffer glBindProgramPipeline
glBindRenderbuffer glBindSampler glBindTexture glBindVertexArray glBlendColor glBlendEquationSeparate glBlendEquationSeparatei
glBlendFunc glBlendFuncSeparate glBlendFuncSeparatei glBlitFramebuffer glBufferData glBufferSubData glCheckFramebufferStatus
glClear glClearColor glClearDepth glClearStencil glColorMask glColorMaski glCompileShader glCompressedTexImage2D glCreateProgram
glCreateShader glCullFace glDeleteBuffers glDeleteFramebuffers glDeleteQueries glDeleteSamplers glDeleteShader glDeleteSync
glDeleteTextures glDepthFunc glDepthMask glDepthRange glDetachShader glDisable glDisableVertexAttribArray glDisablei glDrawArrays
glDrawArraysInstanced glDrawBuffer glDrawBuffers glDrawElements glDrawElementsBaseVertex glDrawElementsInstanced
glDrawElementsInstancedBaseVertex glEnable glEnableVertexAttribArray glEnablei glEndQuery glFenceSync glFinish glFlush
glFramebufferRenderbuffer glFramebufferTexture2D glFrontFace glGenBuffers glGenFramebuffers glGenProgramPipelines glGenQueries
glGenRenderbuffers glGenSamplers glGenTextures glGenVertexArrays glGenerateMipmap glGetBooleanv glGetBufferSubData glGetError
glGetFloatv glGetIntegerv glGetProgramInfoLog glGetProgramiv glGetQueryObjectiv glGetQueryObjectui64v glGetShaderInfoLog
glGetShaderSource glGetShaderiv glGetString glGetTexImage glGetTexLevelParameteriv glGetUniformBlockIndex glGetUniformLocation
glGetUniformfv glIsEnabled glLinkProgram glMapBufferRange glPixelStorei glPolygonOffset glPrimitiveRestartIndex
glProgramParameteri glProgramUniform1i glProgramUniform4fv glReadBuffer glReadPixels glRenderbufferStorage
glRenderbufferStorageMultisample glSamplerParameterf glSamplerParameterfv glSamplerParameteri glScissor glShaderSource
glStencilFuncSeparate glStencilMask glStencilOpSeparate glTexBuffer glTexImage2D glTexImage2DMultisample glTexImage3D
glTexParameterf glTexParameteri glTexSubImage2D glUniformBlockBinding glUnmapBuffer glUseProgram glUseProgramStages
glVertexAttribDivisor glVertexAttribIPointer glVertexAttribPointer glViewport glWaitSync glDeleteVertexArrays
glDeleteRenderbuffers glDeleteProgram glDeleteProgramPipelines glTexSubImage3D glCopyTexSubImage2D glPolygonMode
glUniform1i glUniform4fv glGetActiveUniformBlockiv glValidateProgram glBindAttribLocation glBindFragDataLocation
glBeginTransformFeedback glEndTransformFeedback glCopyBufferSubData glBindBufferBase glTransformFeedbackVaryings""".split()

# loader side hand-written (shim/src/glh_client.c): no router generated (an encoder e_<name> still is, unless HELPER_SPECIAL)
CLIENT_SPECIAL = set("""glGenBuffers glGenFramebuffers glGenProgramPipelines glGenQueries glGenRenderbuffers glGenSamplers glGenTextures
glGenVertexArrays glCreateProgram glCreateShader glShaderSource glMapBufferRange glUnmapBuffer glFenceSync glWaitSync glDeleteSync
glGetError glGetString glCheckFramebufferStatus glFinish glGetQueryObjectiv glGetQueryObjectui64v glBeginQuery glEndQuery
glPixelStorei glBindBuffer glGetTexImage glReadPixels glTransformFeedbackVaryings""".split())
# wire format differs: neither encoder nor decoder generated (shim/src/glh_client.c + shim/src/glhelper.c)
HELPER_SPECIAL = set("""glGenBuffers glGenFramebuffers glGenProgramPipelines glGenQueries glGenRenderbuffers glGenSamplers glGenTextures
glGenVertexArrays glCreateProgram glCreateShader glShaderSource glMapBufferRange glUnmapBuffer glFenceSync glWaitSync glDeleteSync
glGetError glGetString glCheckFramebufferStatus glFinish glGetQueryObjectiv glGetQueryObjectui64v glBeginQuery glEndQuery
glGetTexImage glTransformFeedbackVaryings""".split())
SPECIAL = CLIENT_SPECIAL

# input pointers carried as payloads: byte-size expressions over the C argument names
PAYLOAD = {
    'glBufferData': {'data': 'size'},
    'glBufferSubData': {'data': 'size'},
    'glTexImage2D': {'pixels': 'glh_img_bytes(width,height,1,format,type,0)'},
    'glTexImage3D': {'pixels': 'glh_img_bytes(width,height,depth,format,type,0)'},
    'glTexSubImage2D': {'pixels': 'glh_img_bytes(width,height,1,format,type,0)'},
    'glTexSubImage3D': {'pixels': 'glh_img_bytes(width,height,depth,format,type,0)'},
    'glCompressedTexImage2D': {'data': 'imageSize'},
    'glProgramUniform4fv': {'value': 'count*16'},
    'glUniform4fv': {'value': 'count*16'},
    'glSamplerParameterfv': {'param': 'glh_pname_n(pname)*4'},
    'glDrawBuffers': {'bufs': 'n*4'},
    'glDeleteTextures': {'textures': 'n*4'}, 'glDeleteBuffers': {'buffers': 'n*4'},
    'glDeleteFramebuffers': {'framebuffers': 'n*4'}, 'glDeleteSamplers': {'samplers': 'count*4'},
    'glDeleteQueries': {'ids': 'n*4'}, 'glDeleteVertexArrays': {'arrays': 'n*4'},
    'glDeleteRenderbuffers': {'renderbuffers': 'n*4'}, 'glDeleteProgramPipelines': {'pipelines': 'n*4'},
    'glGetUniformLocation': {'name': 'glh_strbytes(name)'},
    'glGetUniformBlockIndex': {'uniformBlockName': 'glh_strbytes(uniformBlockName)'},
    'glBindAttribLocation': {'name': 'glh_strbytes(name)'},
    'glBindFragDataLocation': {'name': 'glh_strbytes(name)'},
}
# pixel pointers that are buffer offsets while a PIXEL_UNPACK_BUFFER is bound
UNPACK = {'glTexImage2D': 'pixels', 'glTexImage3D': 'pixels', 'glTexSubImage2D': 'pixels', 'glTexSubImage3D': 'pixels',
          'glCompressedTexImage2D': 'data'}
# output pointers of synchronous calls: capacity expressions
OUTPUT = {
    'glGetIntegerv': {'params': 'glh_pname_n(pname)*4'}, 'glGetFloatv': {'params': 'glh_pname_n(pname)*4'},
    'glGetBooleanv': {'params': 'glh_pname_n(pname)'},
    'glGetShaderiv': {'params': '4'}, 'glGetProgramiv': {'params': '4'}, 'glGetTexLevelParameteriv': {'params': '4'},
    'glGetShaderInfoLog': {'length': '4', 'infoLog': 'bufSize'}, 'glGetProgramInfoLog': {'length': '4', 'infoLog': 'bufSize'},
    'glGetShaderSource': {'length': '4', 'source': 'bufSize'}, 'glGetBufferSubData': {'data': 'size'},
    'glGetUniformfv': {'params': '16'}, 'glGetActiveUniformBlockiv': {'params': '4'},
}

def parse_gl3():
    src = open(GL3).read()
    protos = {}
    for m in re.finditer(r'GLAPI\s+(.+?)\s*APIENTRY\s+(gl\w+)\s*\(([^)]*)\)[^;]*;', src):
        ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
        al = []
        if args and args != 'void':
            for a in args.split(','):
                a = a.strip()
                mm = re.match(r'(.*?)(\w+)$', a)
                ty, nm = mm.group(1).strip(), mm.group(2)
                al.append((ty, nm))
        protos[name] = (ret, al)
    return protos

def is_ptr(ty): return '*' in ty or ty in ('GLsync',)
def slot_kind(ty):
    t = ty.replace('const', '').strip()
    if '*' in t or t == 'GLsync': return 'p'
    if t in ('GLfloat', 'GLclampf'): return 'f'
    if t in ('GLdouble', 'GLclampd'): return 'd'
    if t in ('GLintptr', 'GLsizeiptr', 'GLint64'): return 'l'
    if t in ('GLuint64',): return 'u'
    if t in ('GLint', 'GLsizei'): return 'i'
    return 'w'   # GLenum GLuint GLbitfield GLboolean GLubyte ...

def main():
    protos = parse_gl3()
    names = [f for f in FUNCS if f in protos]
    missing = [f for f in FUNCS if f not in protos]
    if missing: print('not in gl3.h (skipped):', missing, file=sys.stderr)
    # every pointer argument needs a rule: a payload, an output, or a buffer offset (else it would reach the helper as a
    # loader address -- the first helper run crashed in glSamplerParameterfv on exactly that)
    OFFSETS = {('glVertexAttribPointer', 'pointer'), ('glVertexAttribIPointer', 'pointer'), ('glDrawElements', 'indices'),
               ('glDrawElementsBaseVertex', 'indices'), ('glDrawElementsInstanced', 'indices'),
               ('glDrawElementsInstancedBaseVertex', 'indices'), ('glReadPixels', 'pixels')}
    bad = []
    for n in names:
        if n in HELPER_SPECIAL: continue
        for t, a in protos[n][1]:
            if slot_kind(t) == 'p' and a not in PAYLOAD.get(n, {}) and a not in OUTPUT.get(n, {}) and (n, a) not in OFFSETS:
                bad.append('%s(%s %s)' % (n, t, a))
    if bad: sys.exit('glh_gen: pointer arguments without a rule:\n  ' + '\n  '.join(bad))
    cl = ['// GENERATED by shim/tools/glh_gen.py -- do not edit. Loader side of the native GL helper (glh_client.c).']
    de = ['// GENERATED by shim/tools/glh_gen.py -- do not edit. Helper side (glhelper.c): decode one call and run it.']
    cl.append('enum {'); cl.append('    GLH_OP_NONE=0,')
    for i, n in enumerate(names): cl.append('    GLH_OP_%s=%d,' % (n, i + 1))
    cl.append('    GLH_OP_GEN_END=%d };' % (len(names) + 1))
    # real entry points
    cl.append('static struct {')
    for n in names:
        ret, al = protos[n]
        cl.append('    %s(*%s)(%s);' % (ret, n, ', '.join(t for t, _ in al) or 'void'))
    cl.append('} GLR;')
    cl.append('static void glh_load_real(void){')
    for n in names: cl.append('    GLR.%s=glh_real_dlsym("%s");' % (n, n))
    cl.append('}')
    # routers
    for n in names:
        if n in HELPER_SPECIAL: continue
        ret, al = protos[n]
        pay, out = PAYLOAD.get(n, {}), OUTPUT.get(n, {})
        sync = ret != 'void' or bool(out)
        params = ', '.join('%s %s' % (t, a) for t, a in al) or 'void'
        argl = ', '.join(a for _, a in al)
        if n not in CLIENT_SPECIAL:
            cl.append('static %s e_%s(%s);' % (ret, n, params))
            cl.append('static %s r_%s(%s){ if(!glh_active()) %sGLR.%s(%s); %se_%s(%s); }' % (ret, n, params, '' if ret == 'void' else 'return ', n, argl,
                      'else ' if ret == 'void' else 'return ', n, argl))
        cl.append('static %s e_%s(%s){' % (ret, n, params))
        cl.append('    {')
        # payload sizes
        mask_bits, sizes = [], []
        for i, (t, a) in enumerate(al):
            if a in pay:
                if n in UNPACK and UNPACK[n] == a:
                    cl.append('    uint64_t sz_%s=(%s && !glh_unpack_bound())?(uint64_t)(%s):0;' % (a, a, pay[a]))
                else:
                    cl.append('    uint64_t sz_%s=%s?(uint64_t)(%s):0;' % (a, a, pay[a]))
                mask_bits.append((i, a))
        tot = ' + '.join(['%d' % (8 + 8 * len(al))] + ['GLH_AL8(sz_%s)' % a for _, a in mask_bits])
        cl.append('    uint8_t*c=glh_begin(GLH_OP_%s,%s); if(!c) %s;' % (n, tot, 'return' if ret == 'void' else 'return (%s)0' % ret))
        cl.append('    uint16_t m=0; uint64_t*s=(uint64_t*)(c+8);')
        for i, (t, a) in enumerate(al):
            k = slot_kind(t)
            if a in pay:
                cl.append('    if(sz_%s){ m|=%d; s[%d]=sz_%s; } else s[%d]=(uint64_t)(uintptr_t)%s;' % (a, 1 << i, i, a, i, a))
            elif k == 'p': cl.append('    s[%d]=(uint64_t)(uintptr_t)%s;' % (i, a))
            elif k == 'f': cl.append('    { float v=(float)%s; memcpy(&s[%d],&v,4); }' % (a, i))
            elif k == 'd': cl.append('    { double v=(double)%s; memcpy(&s[%d],&v,8); }' % (a, i))
            elif k in ('l', 'i'): cl.append('    s[%d]=(uint64_t)(int64_t)%s;' % (i, a))
            else: cl.append('    s[%d]=(uint64_t)%s;' % (i, a))
        cl.append('    uint8_t*p=(uint8_t*)(s+%d);' % len(al))
        for i, a in mask_bits:
            cl.append('    if(sz_%s){ memcpy(p,(const void*)%s,(size_t)sz_%s); p+=GLH_AL8(sz_%s); }' % (a, a, a, a))
        cl.append('    ((uint16_t*)c)[1]=m;')
        if not sync:
            cl.append('    glh_end(c); }')
            cl.append('}')
        else:
            outs = [(i, a) for i, (t, a) in enumerate(al) if a in out]
            cl.append('    uint64_t cap[%d]={0};' % max(1, len(outs)))
            for j, (i, a) in enumerate(outs):
                cl.append('    cap[%d]=%s?(uint64_t)(%s):0; s[%d]=cap[%d];' % (j, a, out[a], i, j))
            cl.append('    uint64_t rv=0; const uint8_t*r=glh_end_sync(c,&rv);')
            for j, (i, a) in enumerate(outs):
                cl.append('    if(cap[%d]&&r){ memcpy((void*)%s,r,(size_t)cap[%d]); } r=r?r+GLH_AL8(cap[%d]):r;' % (j, a, j, j))
            cl.append('    glh_sync_release();')
            if ret == 'void': cl.append('    (void)rv; }'); cl.append('}')
            else:
                cl.append('    return (%s)rv; }' % ret)
                cl.append('}')
    # name -> router
    for n in names:   # the hand-written routers (glh_client.c), defined after this file
        if n in CLIENT_SPECIAL:
            ret, al = protos[n]
            cl.append('static %s s_%s(%s);' % (ret, n, ', '.join('%s %s' % (t, a) for t, a in al) or 'void'))
    cl.append('static void* glh_router_for(const char*n){')
    for n in names:
        fn = ('r_' + n) if n not in SPECIAL else ('s_' + n)
        cl.append('    if(!strcmp(n,"%s")) return (void*)%s;' % (n, fn))
    cl.append('    return 0; }')

    # decoder
    de.append('enum {'); de.append('    GLH_OP_NONE=0,')
    for i, n in enumerate(names): de.append('    GLH_OP_%s=%d,' % (n, i + 1))
    de.append('    GLH_OP_GEN_END=%d };' % (len(names) + 1))
    de.append('static int glh_exec_gen(uint16_t op,uint16_t m,const uint64_t*s,const uint8_t*p,uint8_t*reply,uint64_t*rv){')
    de.append('    (void)m;(void)p;(void)reply;(void)rv; switch(op){')
    for n in names:
        if n in HELPER_SPECIAL: continue
        ret, al = protos[n]
        pay, out = PAYLOAD.get(n, {}), OUTPUT.get(n, {})
        de.append('    case GLH_OP_%s: {' % n)
        if pay: de.append('        p=(const uint8_t*)(s+%d);' % len(al))
        args = []
        outs = [a for _, a in al if a in out]
        for i, (t, a) in enumerate(al):
            k = slot_kind(t)
            tt = t
            if a in pay:
                de.append('        %s a%d=(m&%d)?(%s)p:(%s)(uintptr_t)s[%d]; if(m&%d) p+=GLH_AL8(s[%d]);' % (tt, i, 1 << i, tt, tt, i, 1 << i, i))
            elif a in out:
                de.append('        %s a%d=s[%d]?(%s)reply:(%s)0; reply+=GLH_AL8(s[%d]);' % (tt, i, i, tt, tt, i))
            elif k == 'p': de.append('        %s a%d=(%s)(uintptr_t)s[%d];' % (tt, i, tt, i))
            elif k == 'f': de.append('        float a%d; memcpy(&a%d,&s[%d],4);' % (i, i, i))
            elif k == 'd': de.append('        double a%d; memcpy(&a%d,&s[%d],8);' % (i, i, i))
            elif k in ('l', 'i'): de.append('        %s a%d=(%s)(int64_t)s[%d];' % (tt, i, tt, i))
            else: de.append('        %s a%d=(%s)s[%d];' % (tt, i, tt, i))
            args.append('a%d' % i)
        call = '%s(%s)' % (n, ', '.join(args))
        if ret == 'void': de.append('        %s; return %d; }' % (call, 1 if (out) else 0))
        else: de.append('        *rv=(uint64_t)%s; return 1; }' % call)
    de.append('    default: return -1; } }')
    open(os.path.join(ROOT, 'src/glh_gen.inc'), 'w').write('\n'.join(cl) + '\n')
    open(os.path.join(ROOT, 'src/glh_dec.inc'), 'w').write('\n'.join(de) + '\n')
    print('glh_gen: %d functions (%d hand-written)' % (len(names), len([n for n in names if n in SPECIAL])))

main()
