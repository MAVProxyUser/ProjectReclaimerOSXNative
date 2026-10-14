#!/usr/bin/env python3
# DXBC (Shader Model 4/5) -> GLSL 410 core translator for the Reclaimer D3D11->GL path.
# Scope: enough of SM4/5 to translate Halo 3 / MCC shaders (growing). Parses the DXBC container
# (ISGN/OSGN/RDEF/SHEX), decodes the tokenized instruction+operand stream, and emits GLSL.
# Usage: dxbc2glsl.py <shader.dxbc>   (prints GLSL; use --dump for the decoded listing)
import sys, struct

def u32(d,o): return struct.unpack_from('<I',d,o)[0]
def f32(v):  return struct.unpack('<f',struct.pack('<I',v))[0]
import math
def fmtf(bits):
    # Format a 32-bit float immediate for GLSL. NaN/Inf have no GLSL literal, so emit the
    # exact bit pattern via uintBitsToFloat (preserves the value the shader intended).
    v=f32(bits)
    if math.isnan(v) or math.isinf(v): return 'uintBitsToFloat(%uu)'%(bits & 0xffffffff)
    return '%.9g'%v

# ---- container ----
def parse_chunks(d):
    assert d[:4]==b'DXBC', "not DXBC"
    n=u32(d,28); ch={}
    for i in range(n):
        o=u32(d,32+4*i); fc=d[o:o+4].decode('latin1'); sz=u32(d,o+4); ch[fc]=(o+8,sz)
    return ch

def parse_sig(d,base):
    # ISGN/OSGN: u32 count, u32 _, then count * {nameOff,idx,sysval,compType,reg,mask/rwMask(byte)+...}
    n=u32(d,base); out=[]
    for i in range(n):
        e=base+8+i*24
        noff=u32(d,e); idx=u32(d,e+4); sysval=u32(d,e+8); ctype=u32(d,e+12); reg=u32(d,e+16)
        mask=d[e+20]; rw=d[e+21]
        nm=d[base+noff:d.index(b'\0',base+noff)].decode('latin1')
        out.append(dict(name=nm,idx=idx,sysval=sysval,ctype=ctype,reg=reg,mask=mask,rw=rw))
    return out

_SYSIN={}   # input register -> GLSL builtin (SV_VertexID/SV_InstanceID), set per-shader in emit()
# ---- SM4/5 operand decode ----
# operand types
T_TEMP=0; T_INPUT=1; T_OUTPUT=2; T_INDEXABLE_TEMP=3; T_IMM32=4; T_IMM64=5
T_SAMPLER=6; T_RESOURCE=7; T_CB=8; T_ICB=9; T_LABEL=10; T_INPUT_PRIM=11
T_OUTPUT_DEPTH=12; T_NULL=13; T_CONST=14; T_INPUT_GSV=25
COMP='xyzw'

def decode_operand(d,o):
    tok=u32(d,o); start=o; o+=4
    numc=tok&3                       # 0->0comp,1->1comp,2->4comp,3->N
    selmode=(tok>>2)&3
    swizzle=None; mask=0xf; sel=0
    if numc==2:
        if selmode==0: mask=(tok>>4)&0xf
        elif selmode==1: swizzle=[(tok>>(4+2*i))&3 for i in range(4)]
        elif selmode==2: sel=(tok>>4)&3
    optype=(tok>>12)&0xff
    numidx=(tok>>20)&3
    idxrep=[(tok>>(22+3*i))&7 for i in range(numidx)]
    ext=(tok>>31)&1
    if ext:
        e=u32(d,o); o+=4   # extended operand token (modifiers: neg/abs) — capture
        emod=(e>>6)&0xff
    else: emod=0
    idxs=[]
    # D3D10_SB_OPERAND_INDEX_REPRESENTATION: 0=IMM32 1=IMM64 2=RELATIVE
    #   3=IMM32_PLUS_RELATIVE 4=IMM64_PLUS_RELATIVE
    for r in idxrep:
        if r==0:            # imm32
            idxs.append(('imm',u32(d,o))); o+=4
        elif r==1:          # imm64
            idxs.append(('imm',u32(d,o))); o+=8
        elif r==2:          # relative (operand follows)
            rel,o=decode_operand(d,o); idxs.append(('rel',0,rel))
        elif r==3:          # imm32 + relative
            base_imm=u32(d,o); o+=4; rel,o=decode_operand(d,o); idxs.append(('rel',base_imm,rel))
        elif r==4:          # imm64 + relative
            base_imm=u32(d,o); o+=8; rel,o=decode_operand(d,o); idxs.append(('rel',base_imm,rel))
        else:
            idxs.append(('imm',u32(d,o))); o+=4
    imm=None
    if optype==T_IMM32:
        cnt=4 if numc==2 else 1
        imm=[u32(d,o+4*k) for k in range(cnt)]; o+=4*cnt
    return dict(type=optype,mask=mask,swz=swizzle,sel=sel,numc=numc,idx=idxs,imm=imm,emod=emod,selmode=selmode),o

# opcode table — AUTHORITATIVE D3D10/11 SB opcode numbers. (The earlier table was missing CUT=9,
# shifting every opcode >=9 by one, so dp4(17) read as else(18-1), mov(54) as movc, etc. -> the
# matrix-transform dp4s were dropped and gl_Position stayed 0 -> geometry collapsed -> black frame.)
OPS={0:'add',1:'and',2:'break',3:'breakc',4:'call',5:'callc',6:'case',7:'continue',8:'continuec',
 9:'cut',10:'default',11:'deriv_rtx',12:'deriv_rty',13:'discard',14:'div',15:'dp2',16:'dp3',17:'dp4',
 18:'else',19:'emit',20:'emitthencut',21:'endif',22:'endloop',23:'endswitch',
 24:'eq',25:'exp',26:'frc',27:'ftoi',28:'ftou',29:'ge',30:'iadd',31:'if',32:'ieq',33:'ige',
 34:'ilt',35:'imad',36:'imax',37:'imin',38:'imul',39:'ine',40:'ineg',41:'ishl',42:'ishr',43:'itof',
 44:'label',45:'ld',46:'ld_ms',47:'log',48:'loop',49:'lt',50:'mad',51:'min',52:'max',53:'customdata',
 54:'mov',55:'movc',56:'mul',57:'ne',58:'nop',59:'not',60:'or',61:'resinfo',62:'ret',63:'retc',
 64:'round_ne',65:'round_ni',66:'round_pi',67:'round_z',68:'rsq',69:'sample',70:'sample_c',
 71:'sample_c_lz',72:'sample_l',73:'sample_d',74:'sample_b',75:'sqrt',76:'switch',77:'sincos',
 78:'udiv',79:'ult',80:'uge',81:'umul',82:'umad',83:'umax',84:'umin',85:'ushr',86:'utof',87:'xor',
 88:'dcl_resource',89:'dcl_cb',90:'dcl_sampler',91:'dcl_index_range',94:'dcl_maxout',95:'dcl_input',
 96:'dcl_input_sgv',97:'dcl_input_siv',98:'dcl_input_ps',99:'dcl_input_ps_sgv',100:'dcl_input_ps_siv',
 101:'dcl_output',102:'dcl_output_sgv',103:'dcl_output_siv',104:'dcl_temps',
 105:'dcl_indexable_temp',106:'dcl_globalflags'}

def decode_program(d,base,size):
    ver=u32(d,base); length=u32(d,base+4)
    ptype=(ver>>16)&0xffff   # 0xffff..: 0=PS,1=VS,2=GS,3=HS,4=DS,5=CS (high nibble)
    i=base+8; end=min(base+length*4, base+size); ins=[]
    while i+4<=end:
        tok=u32(d,i); op=tok&0x7ff
        # opcode 52 = CUSTOMDATA (e.g. the immediate constant buffer). Its length is in the NEXT dword,
        # NOT the opcode token's length field; using the token length mis-advances and DESYNCS the
        # whole rest of the stream (later dp4/mad become garbage -> gl_Position never computed -> black).
        if op==53:  # CUSTOMDATA: length in the next dword, not the opcode token
            clen=u32(d,i+4)
            if clen<2 or i+clen*4>base+size+4: break
            ins.append(dict(op=53,name='customdata',operands=[],test=0,icb=d[i+8:i+clen*4]))
            i+=clen*4; continue
        ln=(tok>>24)&0x7f
        ext=(tok>>31)&1
        test=(tok>>18)&1   # D3D10_SB_INSTRUCTION_TEST_BOOLEAN (if/breakc/etc: 0=z,1=nz)
        if ln==0: break
        name=OPS.get(op,'op%d'%op)
        o=i+4
        if ext:  # skip extended opcode tokens (e.g. sample resource return type / offsets)
            while u32(d,o)>>31: o+=4
            o+=4
        # operands: decode until we've consumed ln dwords
        operands=[]
        stop=i+ln*4
        if op not in (62,58,18,21,22,48,2,7,9,10,23,19):  # ops w/o operands (ret/nop/else/endif/endloop/loop/break/continue/cut/default/endswitch/emit)
            while o<stop:
                try: od,o=decode_operand(d,o)
                except Exception: break
                operands.append(od)
        ins.append(dict(op=op,name=name,operands=operands,test=test))
        i+=ln*4
    return ptype,ins

PTYPE={0:'ps',1:'vs',2:'gs',3:'hs',4:'ds',5:'cs'}

# ---- GLSL emitter ----
def swz(op):
    # Always return a 4-char swizzle so every SOURCE evaluates to a vec4 (scalars broadcast by
    # repeating the component). Avoids GLSL 'swizzle of non-vector float' when a dest mask is wider.
    if op['swz'] is not None: s=''.join(COMP[c] for c in op['swz'])
    elif op['numc']==2 and op['selmode']==0: s=''.join(COMP[i] for i in range(4) if op['mask']&(1<<i)) or 'x'
    elif op['selmode']==2: s=COMP[op['sel']]
    else: s='xyzw'
    return (s + s[-1]*4)[:4] if s else 'xyzw'
def destmask(op):
    return ''.join(COMP[i] for i in range(4) if op['mask']&(1<<i)) or 'xyzw'
def src(op):
    t=op['type']
    if t==T_IMM32:
        b=op['imm']
        if len(b)==1: return 'vec4(%s)'%fmtf(b[0])  # always vec4 so .xyzw swizzles are valid
        return 'vec4(%s)'%','.join(fmtf(x) for x in b)
    ix=lambda k: (op['idx'][k][1] if len(op['idx'])>k else 0)
    if t==T_TEMP:   base='r%d'%ix(0)
    elif t==T_INPUT: base=_SYSIN.get(ix(0),'v%d'%ix(0))
    elif t==T_OUTPUT: base='o%d'%ix(0)
    elif t==T_CB:
        # cb[b][reg].swz : idx[0]=buffer, idx[1]=register
        base='cb%d[%d]'%(ix(0),ix(1))
    elif t==T_CONST: base='r%d'%ix(0)
    elif t==T_INPUT_GSV or t==11: base='gl_FragCoord' # rough
    else: base='vec4(0.0)'
    s=swz(op); e='%s.%s'%(base,s) if s!='xyzw' or True else base
    neg=op['emod']&0x1; ab=op['emod']&0x2
    if ab: e='abs(%s)'%e
    if neg: e='(-%s)'%e
    return e
def ncomp(op): # number of components a src/dest references
    if op['swz'] is not None: return 4
    if op['selmode']==2: return 1
    return bin(op['mask']).count('1') if op['numc']==2 else 1

def emit(stage,isgn,osgn,ins):
    L=['#version 410 core','// stage=%s'%stage]
    # declarations gathered from instructions
    ntemps=0; cbs={}; samplers=set(); resources=set()
    for x in ins:
        if x['name']=='dcl_temps' and x['operands']==[] : pass
    # dcl_temps encodes count in its single imm operand length token; re-scan raw not available here,
    # so infer temp count from max temp index used.
    maxt=-1
    for x in ins:
        for op in x['operands']:
            if op['type']==T_TEMP and op['idx']: maxt=max(maxt,op['idx'][0][1])
            if op['type']==T_CB and op['idx']:
                b=op['idx'][0][1]; need=64
                if len(op['idx'])>1:
                    ri=op['idx'][1]                       # register index: ('imm',v) or ('rel',base,relop)
                    if ri[0]=='imm': need=ri[1]+1
                    else: need=max(ri[1]+4096,4096)       # relative index -> size generously
                cbs[b]=max(cbs.get(b,0),need,64)
            if op['type']==T_SAMPLER and op['idx']: samplers.add(op['idx'][0][1])
            if op['type']==T_RESOURCE and op['idx']: resources.add(op['idx'][0][1])
    ntemps=maxt+1
    # inputs (dedupe by register — a signature can map several semantics to one reg). System-value
    # inputs (SV_VertexID/SV_InstanceID) become GLSL builtins, not vertex attributes.
    global _SYSIN; _SYSIN={}
    seen_in=set()
    for s in isgn:
        if stage=='ps' and s['name']=='SV_Position': continue  # use gl_FragCoord
        if s['name']=='SV_VertexID': _SYSIN[s['reg']]='vec4(float(gl_VertexID))'; continue
        if s['name']=='SV_InstanceID': _SYSIN[s['reg']]='vec4(float(gl_InstanceID))'; continue
        if s['reg'] in seen_in: continue
        seen_in.add(s['reg']); L.append('in vec4 v%d; // %s%d'%(s['reg'],s['name'],s['idx']))
    # outputs (dedupe by register)
    seen_out=set()
    for s in osgn:
        if s['name']=='SV_Position': continue
        # depth outputs are gl_FragDepth (handled via T_OUTPUT_DEPTH), not a color register.
        # They (and any system-generated output) carry reg=0xFFFFFFFF -> never declare them.
        if s['name'].startswith('SV_Depth') or s['reg']>=0xffffffff: continue
        if s['reg'] in seen_out: continue
        seen_out.add(s['reg'])
        L.append('out vec4 o%d; // %s%d'%(s['reg'],s['name'],s['idx']))
    for b in sorted(cbs): L.append('uniform vec4 cb%d[%d];'%(b,cbs[b]))
    for r in sorted(resources): L.append('uniform sampler2D t%d;'%r)
    L.append('void main(){')
    for i in range(ntemps): L.append('  vec4 r%d=vec4(0.0);'%i)
    out_is_pos={s['reg']:(s['name']=='SV_Position') for s in osgn}
    cf_stack=[]   # control-flow nesting: 'loop'|'if'|['switch',selvar,first]
    def dst(op):
        t=op['type']; ix=(op['idx'][0][1] if op['idx'] else 0)
        if t==T_OUTPUT:
            if ix>=0xffffffff: return 'gl_FragDepth'   # depth register encoded as plain output
            if out_is_pos.get(ix): return 'gl_Position'
            return 'o%d'%ix
        if t==T_OUTPUT_DEPTH: return 'gl_FragDepth'
        return 'r%d'%ix
    def asg(op,expr,n):
        d=dst(op)
        if d=='gl_FragDepth': return '  gl_FragDepth = (%s).x;'%expr   # scalar builtin: no swizzle
        m=destmask(op); return '  %s.%s = (%s).%s;'%(d,m,expr,('xyzw'[:len(m)]))
    for x in ins:
        n=x['name']; o=x['operands']
        if n in ('dcl_cb','dcl_sampler','dcl_resource','dcl_input','dcl_input_ps','dcl_input_sgv',
                 'dcl_input_ps_sgv','dcl_input_ps_siv','dcl_input_siv','dcl_output','dcl_output_sgv',
                 'dcl_output_siv','dcl_temps','dcl_globalflags','dcl_indexable_temp','customdata','nop') or n.startswith('op'):
            continue
        S=lambda k: src(o[k]) if len(o)>k else 'vec4(0.0)'
        cmp='!=' if x.get('test') else '=='
        # ---- control flow. cf_stack entries: 'loop' | 'if' | ['switch',selvar,first_case_seen].
        # DXBC switch/case -> GLSL if/else-if chain (GLSL switch is too strict: no stmts before a
        # case + fall-through); break inside a switch is a no-op, inside a loop it's break;.
        if n=='ret': L.append('  return;'); continue
        if n=='retc': L.append('  if((%s).x %s 0.0) return;'%(S(0),cmp)); continue
        if n=='if': L.append('  if((%s).x %s 0.0){'%(S(0),cmp)); cf_stack.append('if'); continue
        if n=='else':
            if cf_stack and cf_stack[-1]=='if': cf_stack[-1]='else'; L.append('  }else{')  # mark else consumed
            continue
        if n=='endif':
            if cf_stack and cf_stack[-1] in ('if','else'): cf_stack.pop(); L.append('  }')
            continue
        if n=='loop': L.append('  for(int _li=0;_li<255;_li++){'); cf_stack.append('loop'); continue  # safety cap (avoid GPU hangs on a mistranslated break)
        if n=='endloop':
            if cf_stack and cf_stack[-1]=='loop': cf_stack.pop(); L.append('  }')
            continue
        if n=='continue':
            if any(e=='loop' for e in cf_stack): L.append('  continue;')
            continue
        if n=='breakc':
            inloop=any(e=='loop' for e in cf_stack)
            if inloop: L.append('  if((%s).x %s 0.0) break;'%(S(0),cmp))
            else: L.append('  // breakc (switch) omitted')
            continue
        if n=='break':
            inner=next((e for e in reversed(cf_stack) if e=='loop' or (isinstance(e,list) and e[0]=='switch')),None)
            if inner=='loop': L.append('  break;')
            else: L.append('  // break (switch) -> if-chain')
            continue
        if n=='switch':
            sv='_sw%d'%len(cf_stack); L.append('  { int %s=floatBitsToInt((%s).x);'%(sv,S(0))); cf_stack.append(['switch',sv,False]); continue
        if n=='case':
            cv=(o[0]['imm'][0] if (o and o[0]['imm']) else 0)
            st=cf_stack[-1] if (cf_stack and isinstance(cf_stack[-1],list)) else None
            if st: L.append('  %sif(%s==%d){'%('' if not st[2] else '}else ',st[1],cv)); st[2]=True
            continue
        if n=='default':
            st=cf_stack[-1] if (cf_stack and isinstance(cf_stack[-1],list)) else None
            if st: L.append('  %s{'%('}else ' if st[2] else '')); st[2]=True
            continue
        if n=='endswitch':
            if cf_stack and isinstance(cf_stack[-1],list): cf_stack.pop(); L.append('  } }')
            continue
        if not o: L.append('  // %s (no operands)'%n); continue   # arithmetic ops below all need a dest
        # integer ops operate on raw bits; reinterpret float<->int. All operand access via S(k)/TX(k)
        # (guarded) so variable operand counts never crash the emit.
        def si(k): return 'floatBitsToInt(%s)'%S(k)
        def asg_i(op,expr):
            d=dst(op)
            if d=='gl_FragDepth': return '  gl_FragDepth = intBitsToFloat((%s)).x;'%expr
            m=destmask(op); return '  %s.%s = intBitsToFloat((%s)).%s;'%(d,m,expr,'xyzw'[:len(m)])
        def TX(k): return (o[k]['idx'][0][1] if len(o)>k and o[k]['idx'] else 0)
        try:
            if n=='iadd': L.append(asg_i(o[0],'%s + %s'%(si(1),si(2)))); continue
            if n=='imul': L.append(asg_i(o[0],'%s * %s'%(si(2),si(3)) if len(o)>3 else '%s*%s'%(si(1),si(2)))); continue
            if n=='imin': L.append(asg_i(o[0],'min(%s,%s)'%(si(1),si(2)))); continue
            if n=='imax': L.append(asg_i(o[0],'max(%s,%s)'%(si(1),si(2)))); continue
            if n=='imad': L.append(asg_i(o[0],'%s*%s+%s'%(si(1),si(2),si(3)))); continue
            if n=='and': L.append(asg_i(o[0],'%s & %s'%(si(1),si(2)))); continue
            if n=='or':  L.append(asg_i(o[0],'%s | %s'%(si(1),si(2)))); continue
            if n=='xor': L.append(asg_i(o[0],'%s ^ %s'%(si(1),si(2)))); continue
            if n=='not': L.append(asg_i(o[0],'~%s'%si(1))); continue
            if n=='ishl': L.append(asg_i(o[0],'%s << %s'%(si(1),si(2)))); continue
            if n=='ishr': L.append(asg_i(o[0],'%s >> %s'%(si(1),si(2)))); continue
            if n=='ushr': L.append(asg_i(o[0],'ivec4(uvec4(%s) >> uvec4(%s))'%(si(1),si(2)))); continue
            if n=='ftoi': L.append(asg_i(o[0],'ivec4(%s)'%S(1))); continue
            if n=='ftou': L.append(asg_i(o[0],'ivec4(uvec4(%s))'%S(1))); continue
            if n=='itof': L.append(asg(o[0],'vec4(%s)'%si(1),0)); continue
            if n=='utof': L.append(asg(o[0],'vec4(uvec4(%s))'%si(1),0)); continue
            if n in ('ld_ms','ld'): L.append(asg(o[0],'texelFetch(t%d,ivec2((%s).xy),0)'%(TX(2),S(1)),0)); continue
            if n=='sample_d': L.append(asg(o[0],'textureGrad(t%d,(%s).xy,(%s).xy,(%s).xy)'%(TX(2),S(1),S(3),S(4)),0)); continue
            if n=='resinfo': L.append(asg(o[0],'vec4(textureSize(t%d,0),0,0)'%TX(2),0)); continue
            if n=='mov': L.append(asg(o[0],S(1),0))
            elif n=='add': L.append(asg(o[0],'%s + %s'%(S(1),S(2)),0))
            elif n=='mul': L.append(asg(o[0],'%s * %s'%(S(1),S(2)),0))
            elif n=='div': L.append(asg(o[0],'%s / %s'%(S(1),S(2)),0))
            elif n=='mad': L.append(asg(o[0],'%s * %s + %s'%(S(1),S(2),S(3)),0))
            elif n=='min': L.append(asg(o[0],'min(%s,%s)'%(S(1),S(2)),0))
            elif n=='max': L.append(asg(o[0],'max(%s,%s)'%(S(1),S(2)),0))
            elif n=='frc': L.append(asg(o[0],'fract(%s)'%S(1),0))
            elif n=='exp': L.append(asg(o[0],'exp2(%s)'%S(1),0))
            elif n=='log': L.append(asg(o[0],'log2(%s)'%S(1),0))
            elif n=='sqrt': L.append(asg(o[0],'sqrt(%s)'%S(1),0))
            elif n=='rsq': L.append(asg(o[0],'inversesqrt(%s)'%S(1),0))
            elif n=='round_z': L.append(asg(o[0],'trunc(%s)'%S(1),0))
            elif n=='sincos': L.append('  // sincos (approx)'); L.append(asg(o[0] if o[0]['type']!=T_NULL else o[1],'sin(%s)'%S(2),0))
            elif n=='dp2': L.append(asg(o[0],'vec4(dot((%s).xy,(%s).xy))'%(S(1),S(2)),0))
            elif n=='dp3': L.append(asg(o[0],'vec4(dot((%s).xyz,(%s).xyz))'%(S(1),S(2)),0))
            elif n=='dp4': L.append(asg(o[0],'vec4(dot(%s,%s))'%(S(1),S(2)),0))
            elif n=='movc': L.append(asg(o[0],'mix(vec4(%s),vec4(%s),notEqual(vec4(%s),vec4(0.0)))'%(S(3),S(2),S(1)),0) if len(o)>=4 else asg(o[0],S(1),0))
            elif n in ('ne','ine'): L.append(asg(o[0],'vec4(notEqual(vec4(%s),vec4(%s)))'%(S(1),S(2)),0))
            elif n in ('eq','ieq'): L.append(asg(o[0],'vec4(equal(vec4(%s),vec4(%s)))'%(S(1),S(2)),0))
            elif n in ('lt','ilt'): L.append(asg(o[0],'vec4(lessThan(vec4(%s),vec4(%s)))'%(S(1),S(2)),0))
            elif n in ('ge','ige'): L.append(asg(o[0],'vec4(greaterThanEqual(vec4(%s),vec4(%s)))'%(S(1),S(2)),0))
            elif n=='sample': L.append(asg(o[0],'texture(t%d,(%s).xy)'%(TX(2),S(1)),0))
            elif n=='sample_l': L.append(asg(o[0],'textureLod(t%d,(%s).xy,(%s).x)'%(TX(2),S(1),S(4)),0))
            elif n in ('sample_c','sample_c_lz'): L.append(asg(o[0],'vec4(texture(t%d,(%s).xy).r >= (%s).x ?1.0:0.0)'%(TX(2),S(1),S(4) if len(o)>4 else 'vec4(0.5)'),0))
            else: L.append('  // UNTRANSLATED: %s'%n)
        except Exception as e:
            L.append('  // ERR %s: %s'%(n,e))
    # Declare any identifier referenced in the body but not yet declared (textures t#, inputs v#,
    # extra outputs o#, cbuffers cb#). The DXBC resource/signature scan misses some (empty index,
    # regs absent from the signature), which otherwise causes 'undeclared identifier'.
    body='\n'.join(L); import re as _re
    decl_t=set(_re.findall(r'uniform sampler2D t(\d+)',body)); decl_cb=set(_re.findall(r'uniform vec4 cb(\d+)',body))
    decl_v=set(_re.findall(r'in vec4 v(\d+)',body)); decl_o=set(_re.findall(r'out vec4 o(\d+)',body))
    ins_hdr=[]
    for m in sorted(set(_re.findall(r'\bt(\d+)\b',body))-decl_t,key=int): ins_hdr.append('uniform sampler2D t%s;'%m)
    for m in sorted(set(_re.findall(r'\bcb(\d+)\b',body))-decl_cb,key=int): ins_hdr.append('uniform vec4 cb%s[4096];'%m)
    for m in sorted(set(_re.findall(r'\bv(\d+)\b',body))-decl_v,key=int): ins_hdr.append('in vec4 v%s;'%m)
    for m in sorted(set(_re.findall(r'\bo(\d+)\b',body))-decl_o,key=int):
        if m!='0' or 'gl_Position' not in body: ins_hdr.append('out vec4 o%s;'%m)
    if ins_hdr:
        gi=next(i for i,l in enumerate(L) if l.startswith('void main')); L[gi:gi]=ins_hdr; body='\n'.join(L)
    # declare any temp r# referenced but not declared (insert right after 'void main(){')
    decl_r=set(_re.findall(r'vec4 r(\d+)=vec4',body)); miss_r=sorted(set(_re.findall(r'\br(\d+)\b',body))-decl_r,key=int)
    if miss_r:
        gi=next(i for i,l in enumerate(L) if l.startswith('void main'))
        L[gi+1:gi+1]=['  vec4 r%s=vec4(0.0);'%m for m in miss_r]; body='\n'.join(L)
    # auto-balance braces (nested switch/case without endswitch, early ret) so GLSL compiles.
    depth=body.count('{')-body.count('}')  # includes main's own '{'
    if depth>1: L.append('  '+'}'*(depth-1)+' // auto-closed %d block(s)'%(depth-1))
    L.append('}')   # closes main
    return '\n'.join(L)

def main():
    args=[a for a in sys.argv[1:] if not a.startswith('--')]
    dump='--dump' in sys.argv
    d=open(args[0],'rb').read()
    ch=parse_chunks(d)
    isgn=parse_sig(d,ch['ISGN'][0]) if 'ISGN' in ch else []
    osgn=parse_sig(d,ch['OSGN'][0]) if 'OSGN' in ch else []
    sk='SHEX' if 'SHEX' in ch else 'SHDR'
    ptype,ins=decode_program(d,ch[sk][0],ch[sk][1])
    stage=PTYPE.get(ptype>>12 if ptype>0xfff else ptype,'ps')
    if dump:
        print("// stage=%s"%stage); print("// IN :",[(s['name']+str(s['idx']),'r%d'%s['reg'],'m%x'%s['mask']) for s in isgn])
        print("// OUT:",[(s['name']+str(s['idx']),'r%d'%s['reg']) for s in osgn])
        for n,x in enumerate(ins):
            print(" %3d %-14s %s"%(n,x['name'],
                ' '.join('t%d.n%d.m%x%s'%(op['type'],op['numc'],op['mask'],('='+str([f32(v) for v in op['imm']]) if op['imm'] else '')) for op in x['operands'])))
        return
    print(emit(stage,isgn,osgn,ins))

if __name__=='__main__': main()
