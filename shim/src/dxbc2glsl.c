// dxbc2glsl.c — in-process DXBC (Shader Model 4/5) -> GLSL 410 core translator.
// A faithful C port of re-tools/dxbc2glsl.py so the shim translates halo3's shaders ITSELF at
// runtime (no Python subprocess, no offline pre-pass). The Python stays as the test oracle:
// build this standalone with  clang -DDXBC2GLSL_MAIN -o /tmp/dxbc2glsl_c shim/src/dxbc2glsl.c
// and diff its output against dxbc2glsl.py over the dumped-shader corpus.
//
// peload.c does:  #include "dxbc2glsl.c"  and calls dg_translate(bytes,len) -> malloc'd GLSL (or NULL).
// All symbols are dg_-prefixed to avoid colliding with peload.c's own helpers.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

static uint32_t dg_u32(const uint8_t*d,size_t len,size_t o){ if(o+4>len) return 0; uint32_t v; memcpy(&v,d+o,4); return v; }
static float dg_f32(uint32_t b){ float f; memcpy(&f,&b,4); return f; }
// Format a 32-bit float immediate for GLSL (NaN/Inf -> uintBitsToFloat; else %.9g), matching Python fmtf.
static void dg_fmtf(uint32_t bits,char*out,size_t n){
    float v=dg_f32(bits);
    // NaN/Inf AND denormals: DXBC registers are untyped, so an integer immediate (l(1), l(0x10000), the
    // int address of an ld) is a denormal float bit-pattern. Spelled as a decimal float literal, the GPU
    // compiler may flush it to 0 -> emit the exact bits instead.
    if(isnan(v)||isinf(v)||(((bits>>23)&0xff)==0 && (bits&0x7fffff))){ snprintf(out,n,"uintBitsToFloat(%uu)",(unsigned)bits); return; }
    snprintf(out,n,"%.9g",(double)v);
}

// ---- operand model ----
enum { T_TEMP=0,T_INPUT=1,T_OUTPUT=2,T_INDEXABLE_TEMP=3,T_IMM32=4,T_IMM64=5,
       T_SAMPLER=6,T_RESOURCE=7,T_CB=8,T_ICB=9,T_LABEL=10,T_INPUT_PRIM=11,
       T_OUTPUT_DEPTH=12,T_NULL=13,T_CONST=14,T_INPUT_GSV=25 };
static const char COMP[4]={'x','y','z','w'};

typedef struct { int kind; uint32_t val; int rtype; uint32_t rreg; int rcomp; } DgIdx; // kind 0=imm, 1=rel: val + (register rtype#rreg).rcomp
typedef struct {
    int type, mask, sel, numc, selmode, emod;
    int has_swz; int swz[4];
    int nidx; DgIdx idx[3];
    int has_imm; int immn; uint32_t imm[4];
} DgOp;

typedef struct { int op; const char*name; int test; int nop; DgOp ops[8]; uint32_t tok; int has_aoff; int aoff[3]; uint32_t raw[4]; } DgIns;
// Constant buffers as std140 uniform BLOCKS ("layout(std140) uniform cbN_blk { vec4 cbN[size]; }", size from
// dcl_constantbuffer) instead of plain uniform arrays: plain arrays hit GL's 4096-component limit for
// dynamically indexed palettes (skinned meshes failed to link) and cost a full copy per draw. 0 = old arrays.
static int g_dg_ubo = 1;
static int g_dg_nodclsz = 0;
static int g_dg_nocmp = 0;     // A/B: old manual compare for sample_c   // A/B: ignore dcl_constantbuffer sizes (old max-index heuristic)
// immediate constant buffer (dcl_immediateConstantBuffer customdata), per translated shader
static uint32_t g_icb[4096*4]; static int g_icbn;

// decode one operand starting at byte offset o; fill *out (may be NULL to just advance). Returns new o, or -1.
static long dg_decode_operand(const uint8_t*d,size_t len,size_t o,DgOp*out){
    if(o+4>len) return -1;
    uint32_t tok=dg_u32(d,len,o); o+=4;
    DgOp tmp; if(!out) out=&tmp; memset(out,0,sizeof *out);
    int numc=tok&3; int selmode=(tok>>2)&3;
    out->numc=numc; out->selmode=selmode; out->mask=0xf; out->sel=0; out->has_swz=0;
    if(numc==2){
        if(selmode==0) out->mask=(tok>>4)&0xf;
        else if(selmode==1){ out->has_swz=1; for(int i=0;i<4;i++) out->swz[i]=(tok>>(4+2*i))&3; }
        else if(selmode==2) out->sel=(tok>>4)&3;
    }
    int optype=(tok>>12)&0xff; out->type=optype;
    int numidx=(tok>>20)&3;
    int idxrep[3]; for(int i=0;i<numidx;i++) idxrep[i]=(tok>>(22+3*i))&7;
    int ext=(tok>>31)&1;
    if(ext){ uint32_t e=dg_u32(d,len,o); o+=4; out->emod=(e>>6)&0xff; } else out->emod=0;
    out->nidx=numidx;
    for(int k=0;k<numidx;k++){
        int r=idxrep[k];
        if(r==0){ out->idx[k].kind=0; out->idx[k].val=dg_u32(d,len,o); o+=4; }
        else if(r==1){ out->idx[k].kind=0; out->idx[k].val=dg_u32(d,len,o); o+=8; }
        else if(r>=2&&r<=4){ out->idx[k].kind=1; out->idx[k].val=0;
            if(r==3){ out->idx[k].val=dg_u32(d,len,o); o+=4; } else if(r==4){ out->idx[k].val=dg_u32(d,len,o); o+=8; }
            // relative part: a register component (r#.c / x#[i].c / v#.c) holding an int
            DgOp rel; long no=dg_decode_operand(d,len,o,&rel); if(no<0)return -1; o=(size_t)no;
            out->idx[k].rtype=rel.type; out->idx[k].rreg=rel.nidx?rel.idx[0].val:0;
            out->idx[k].rcomp = rel.selmode==2?rel.sel : (rel.has_swz?rel.swz[0]:0);
            if(rel.type==T_INDEXABLE_TEMP && rel.nidx>1) out->idx[k].rreg |= (rel.idx[1].val<<16); }
        else { out->idx[k].kind=0; out->idx[k].val=dg_u32(d,len,o); o+=4; }
    }
    if(optype==T_IMM32){ int cnt=(numc==2)?4:1; out->has_imm=1; out->immn=cnt; for(int k=0;k<cnt;k++) out->imm[k]=dg_u32(d,len,o+4*k); o+=4*cnt; }
    return (long)o;
}

// AUTHORITATIVE D3D10/11 SB opcode -> mnemonic (indices match dxbc2glsl.py OPS).
static const char* dg_ops(int op){
    static const char* T[]={
     "add","and","break","breakc","call","callc","case","continue","continuec","cut","default",
     "deriv_rtx","deriv_rty","discard","div","dp2","dp3","dp4","else","emit","emitthencut","endif",
     "endloop","endswitch","eq","exp","frc","ftoi","ftou","ge","iadd","if","ieq","ige","ilt","imad",
     "imax","imin","imul","ine","ineg","ishl","ishr","itof","label","ld","ld_ms","log","loop","lt",
     "mad","min","max","customdata","mov","movc","mul","ne","nop","not","or","resinfo","ret","retc",
     "round_ne","round_ni","round_pi","round_z","rsq","sample","sample_c","sample_c_lz","sample_l",
     "sample_d","sample_b","sqrt","switch","sincos","udiv","ult","uge","umul","umad","umax","umin",
     "ushr","utof","xor","dcl_resource","dcl_cb","dcl_sampler","dcl_index_range",NULL,NULL,"dcl_maxout",
     "dcl_input","dcl_input_sgv","dcl_input_siv","dcl_input_ps","dcl_input_ps_sgv","dcl_input_ps_siv",
     "dcl_output","dcl_output_sgv","dcl_output_siv","dcl_temps","dcl_indexable_temp","dcl_globalflags"};
    if(op>=0 && op<(int)(sizeof T/sizeof T[0]) && T[op]) return T[op];
    switch(op){   // SM4.1/SM5 opcodes the emitter handles (others stay NULL -> skipped)
        case 109: return "gather4"; case 122: return "deriv_rtx_coarse"; case 123: return "deriv_rtx_fine";
        case 124: return "deriv_rty_coarse"; case 125: return "deriv_rty_fine"; case 129: return "rcp";
        case 130: return "f32tof16"; case 131: return "f16tof32"; case 138: return "ubfe"; case 139: return "ibfe";
        case 140: return "bfi"; case 141: return "bfrev"; case 134: return "countbits";
        case 165: return "ld_raw"; case 167: return "ld_structured"; case 166: return "store_raw"; case 168: return "store_structured";
        case 161: return "dcl_resource_raw"; case 162: return "dcl_resource_structured"; }
    return NULL;
}
static int dg_noperand_op(int op){ // ret,nop,else,endif,endloop,loop,break,continue,cut,default,endswitch,emit
    switch(op){ case 62: case 58: case 18: case 21: case 22: case 48: case 2: case 7: case 9: case 10: case 23: case 19: return 1; } return 0;
}

// decode the SHEX/SHDR program. Returns ptype; fills ins[] (cap limited), *nins.
static int dg_decode_program(const uint8_t*d,size_t len,size_t base,size_t size,DgIns*ins,int*nins,int cap){
    uint32_t ver=dg_u32(d,len,base); uint32_t length=dg_u32(d,len,base+4);
    int ptype=(ver>>16)&0xffff;
    size_t i=base+8; size_t end=base+(size_t)length*4; if(end>base+size) end=base+size;
    int n=0;
    while(i+4<=end && n<cap){
        uint32_t tok=dg_u32(d,len,i); int op=tok&0x7ff;
        if(op==53){ uint32_t clen=dg_u32(d,len,i+4); if(clen<2 || i+(size_t)clen*4>base+size+4) break;
            if((tok>>11)==3){ g_icbn=0; for(uint32_t k=2;k<clen && g_icbn<4096*4;k++) g_icb[g_icbn++]=dg_u32(d,len,i+4*k); }
            ins[n].op=53; ins[n].name="customdata"; ins[n].test=0; ins[n].nop=0; n++; i+=(size_t)clen*4; continue; }
        int ln=(tok>>24)&0x7f; int ext=(tok>>31)&1; int test=(tok>>18)&1;
        if(ln==0) break;
        const char*nm=dg_ops(op);
        size_t o=i+4;
        // extended opcode tokens: type 1 = SAMPLE_CONTROLS -> immediate texel offsets (aoffimmi(u,v,w), 4-bit
        // two's complement at bits 9/13/17). FXAA/bloom/blur taps use them; dropping them collapses the taps.
        if(ext){ for(;;){ uint32_t et=dg_u32(d,len,o); o+=4;
                if((et&0x3f)==1){ ins[n].has_aoff=1; for(int c=0;c<3;c++){ int v=(int)((et>>(9+4*c))&0xf); if(v&8) v-=16; ins[n].aoff[c]=v; } }
                if(!(et>>31) || o>=i+(size_t)ln*4) break; } }
        ins[n].op=op; ins[n].name=nm; ins[n].test=test; ins[n].nop=0; ins[n].tok=tok; ins[n].has_aoff=0;
        for(int k=0;k<4;k++) ins[n].raw[k]=dg_u32(d,len,i+4+4*k);   // raw operand dwords (dcl_indexable_temp)
        size_t stop=i+(size_t)ln*4;
        if(!dg_noperand_op(op)){
            while(o<stop && ins[n].nop<8){ long no=dg_decode_operand(d,len,o,&ins[n].ops[ins[n].nop]); if(no<0) break; o=(size_t)no; ins[n].nop++; }
        }
        n++; i+=(size_t)ln*4;
    }
    *nins=n; return ptype;
}

// ---- signatures ----
typedef struct { char name[64]; uint32_t idx,sysval,reg,mask; } DgSig;
static int dg_parse_sig(const uint8_t*d,size_t len,size_t base,DgSig*out,int cap){
    uint32_t n=dg_u32(d,len,base); int m=0;
    for(uint32_t i=0;i<n && (int)i<cap;i++){
        size_t e=base+8+(size_t)i*24;
        uint32_t noff=dg_u32(d,len,e), idx=dg_u32(d,len,e+4), sysval=dg_u32(d,len,e+8);
        uint32_t reg=dg_u32(d,len,e+16); uint8_t mask=(e+20<len)?d[e+20]:0;
        size_t np=base+noff; out[m].name[0]=0;
        if(np<len){ size_t k=0; while(np+k<len && d[np+k] && k<63){ out[m].name[k]=(char)d[np+k]; k++; } out[m].name[k]=0; }
        out[m].idx=idx; out[m].sysval=sysval; out[m].reg=reg; out[m].mask=mask; m++;
    }
    return m;
}

// ---- line buffer ----
typedef struct { char**v; int n,cap; } DgLines;
static void dg_lpush(DgLines*L,char*s){ if(L->n>=L->cap){ L->cap=L->cap?L->cap*2:64; L->v=realloc(L->v,L->cap*sizeof(char*)); } L->v[L->n++]=s; }
static void dg_lprintf(DgLines*L,const char*fmt,...){ va_list ap; va_start(ap,fmt); va_list ap2; va_copy(ap2,ap);
    int need=vsnprintf(NULL,0,fmt,ap2); va_end(ap2); char*s=malloc(need+1); vsnprintf(s,need+1,fmt,ap); va_end(ap); dg_lpush(L,s); }
static void dg_linsert(DgLines*L,int at,char*s){ if(L->n>=L->cap){ L->cap=L->cap?L->cap*2:64; L->v=realloc(L->v,L->cap*sizeof(char*)); }
    for(int i=L->n;i>at;i--) L->v[i]=L->v[i-1]; L->v[at]=s; L->n++; }

// rotating scratch pool so several src()/si() results coexist inside one printf
static char* dg_pool(void){ static char buf[16][256]; static int pi; char*p=buf[pi++ & 15]; p[0]=0; return p; }

static const char* g_sysin[64];   // input reg -> GLSL builtin (per-shader)

static void dg_swz(const DgOp*op,char out[5]){
    char s[8]; int sl=0;
    if(op->has_swz){ for(int i=0;i<4;i++) s[sl++]=COMP[op->swz[i]]; }
    else if(op->numc==2 && op->selmode==0){ for(int i=0;i<4;i++) if(op->mask&(1<<i)) s[sl++]=COMP[i]; if(sl==0){ s[0]='x'; sl=1; } }
    else if(op->selmode==2){ s[0]=COMP[op->sel]; sl=1; }
    else { memcpy(s,"xyzw",4); sl=4; }
    if(sl==0){ memcpy(out,"xyzw",4); out[4]=0; return; }
    char last=s[sl-1]; while(sl<4) s[sl++]=last; memcpy(out,s,4); out[4]=0;
}
static void dg_destmask(const DgOp*op,char out[5]){ int k=0; for(int i=0;i<4;i++) if(op->mask&(1<<i)) out[k++]=COMP[i]; if(k==0){ memcpy(out,"xyzw",4); k=4; } out[k]=0; }
static uint32_t dg_ix(const DgOp*op,int k){ return (op->nidx>k)?op->idx[k].val:0; }

// index expression: immediate, or base + int(register component)
static void dg_idxexpr(const DgIdx*ix,char*out,size_t n){
    if(ix->kind!=1){ snprintf(out,n,"%u",ix->val); return; }
    char reg[64];
    if(ix->rtype==T_TEMP) snprintf(reg,sizeof reg,"r%u.%c",ix->rreg,COMP[ix->rcomp&3]);
    else if(ix->rtype==T_INDEXABLE_TEMP) snprintf(reg,sizeof reg,"x%u[%u].%c",ix->rreg&0xffff,ix->rreg>>16,COMP[ix->rcomp&3]);
    else if(ix->rtype==T_INPUT) snprintf(reg,sizeof reg,"v%u.%c",ix->rreg,COMP[ix->rcomp&3]);
    else snprintf(reg,sizeof reg,"vec4(0.0).x");
    if(ix->val) snprintf(out,n,"%u + floatBitsToInt(%s)",ix->val,reg); else snprintf(out,n,"floatBitsToInt(%s)",reg); }
static const char* dg_src(const DgOp*op){
    char*o=dg_pool();
    if(op->type==T_IMM32){ char f0[48];
        if(op->immn==1){ dg_fmtf(op->imm[0],f0,sizeof f0); snprintf(o,256,"vec4(%s)",f0); }
        else { char f1[48],f2[48],f3[48]; dg_fmtf(op->imm[0],f0,sizeof f0); dg_fmtf(op->imm[1],f1,sizeof f1); dg_fmtf(op->imm[2],f2,sizeof f2); dg_fmtf(op->imm[3],f3,sizeof f3);
            snprintf(o,256,"vec4(%s,%s,%s,%s)",f0,f1,f2,f3); }
        return o;
    }
    char base[96];
    switch(op->type){
        case T_TEMP:   snprintf(base,sizeof base,"r%u",dg_ix(op,0)); break;
        case T_INPUT:  { uint32_t r=dg_ix(op,0); if(r<64 && g_sysin[r]) snprintf(base,sizeof base,"%s",g_sysin[r]); else snprintf(base,sizeof base,"v%u",r); } break;
        case T_OUTPUT: snprintf(base,sizeof base,"o%u",dg_ix(op,0)); break;
        case T_CB:     { char ie[96]; if(op->nidx>1) dg_idxexpr(&op->idx[1],ie,sizeof ie); else snprintf(ie,sizeof ie,"0"); snprintf(base,sizeof base,"cb%u[%s]",dg_ix(op,0),ie); } break;
        case T_INDEXABLE_TEMP: { char ie[96]; if(op->nidx>1) dg_idxexpr(&op->idx[1],ie,sizeof ie); else snprintf(ie,sizeof ie,"0"); snprintf(base,sizeof base,"x%u[%s]",dg_ix(op,0),ie); } break;
        case T_ICB:    { char ie[96]; if(op->nidx) dg_idxexpr(&op->idx[0],ie,sizeof ie); else snprintf(ie,sizeof ie,"0"); snprintf(base,sizeof base,"dx_icb[%s]",ie); } break;
        case T_CONST:  snprintf(base,sizeof base,"r%u",dg_ix(op,0)); break;
        case T_INPUT_GSV: case 11: snprintf(base,sizeof base,"gl_FragCoord"); break;
        default: snprintf(base,sizeof base,"vec4(0.0)"); break;
    }
    char s[5]; dg_swz(op,s);
    char e[160]; snprintf(e,sizeof e,"%s.%s",base,s);
    int neg=op->emod&0x1, ab=op->emod&0x2;
    if(ab){ char t[200]; snprintf(t,sizeof t,"abs(%s)",e); memcpy(e,t,sizeof e); }
    if(neg){ char t[200]; snprintf(t,sizeof t,"(-%s)",e); memcpy(e,t,sizeof e); }
    snprintf(o,256,"%s",e); return o;
}

// ---- helpers for body emit that need the osgn (SV_Position) ----
typedef struct { const DgSig*osgn; int no; } DgCtx;
static int dg_out_is_pos(DgCtx*c,uint32_t reg){ for(int i=0;i<c->no;i++) if(c->osgn[i].reg==reg && !strcmp(c->osgn[i].name,"SV_Position")) return 1; return 0; }
static const char* dg_dst(DgCtx*c,const DgOp*op){ char*o=dg_pool(); uint32_t ix=op->nidx?op->idx[0].val:0;
    if(op->type==T_OUTPUT){ if(ix>=0xffffffffu){ snprintf(o,256,"gl_FragDepth"); return o; } if(dg_out_is_pos(c,ix)){ snprintf(o,256,"gl_Position"); return o; } snprintf(o,256,"o%u",ix); return o; }
    if(op->type==T_OUTPUT_DEPTH){ snprintf(o,256,"gl_FragDepth"); return o; }
    if(op->type==T_INDEXABLE_TEMP){ char ie[96]; if(op->nidx>1) dg_idxexpr(&op->idx[1],ie,sizeof ie); else snprintf(ie,sizeof ie,"0"); snprintf(o,256,"x%u[%s]",ix,ie); return o; }
    snprintf(o,256,"r%u",ix); return o; }
// DXBC is lane-wise: destination lane i reads lane i of the (already per-lane swizzled) 4-wide source, so
// "mov o0.zw, l(0,0,0.5,0.5)" sets z=0.5,w=0.5. Select the MASK lanes, not the first N lanes -- the old
// "(expr).xy" for a .zw mask wrote w=0 into gl_Position (geometry vanished) and scrambled every non-prefix
// write mask (.y .z .w .xz .yw .zw ...) across all shaders.
// A NULL destination (operand type 13, e.g. imul's unused hi half) has no register: it used to fall
// through to "r0.xyzw" and silently clobber r0 (seen: a light loop's counter in r0.w).
// _sat instruction modifier (opcode token bit 13): clamp the float result to [0,1]. It was ignored -> 7210
// instructions in 1852 shaders (lighting/fog/blend weights) produced out-of-range values.
static int g_dg_sat;
static char* dg_asg(DgCtx*c,const DgOp*op,const char*expr){ if(op->type==T_NULL) return strdup("  // (null dst)"); const char*d=dg_dst(c,op);
    if(!strcmp(d,"gl_FragDepth")){ char*s; asprintf(&s,g_dg_sat?"  gl_FragDepth = clamp((%s).x,0.0,1.0);":"  gl_FragDepth = (%s).x;",expr); return s; }
    char m[5]; dg_destmask(op,m);
    char*s; if(g_dg_sat) asprintf(&s,"  %s.%s = clamp(vec4(%s),0.0,1.0).%s;",d,m,expr,m); else asprintf(&s,"  %s.%s = (%s).%s;",d,m,expr,m); return s; }
static char* dg_asg_i(DgCtx*c,const DgOp*op,const char*expr){ if(op->type==T_NULL) return strdup("  // (null dst)"); const char*d=dg_dst(c,op);
    if(!strcmp(d,"gl_FragDepth")){ char*s; asprintf(&s,"  gl_FragDepth = intBitsToFloat((%s)).x;",expr); return s; }
    char m[5]; dg_destmask(op,m);
    char*s; asprintf(&s,"  %s.%s = intBitsToFloat((%s)).%s;",d,m,expr,m); return s; }

// ---- texture resource dimensions (dcl_resource) -> GLSL sampler type + coordinate shapes ----
static const char* dg_samptype(int dim){
    switch(dim){ case 100: case 101: return "usamplerBuffer"; case 5: return "sampler3D"; case 6: return "samplerCube"; case 7: case 8: case 9: return "sampler2DArray";
        case 10: return "samplerCubeArray"; default: return "sampler2D"; } }
static const char* dg_crd(int dim,const char*a){ char*o=dg_pool();      // float sample coordinate
    switch(dim){ case 5: case 6: case 8: case 9: snprintf(o,256,"(%s).xyz",a); break;
        case 7: snprintf(o,256,"vec3((%s).x,0.5,(%s).y)",a,a); break;       // 1D array = 2D array of height-1 slices
        case 10: snprintf(o,256,"(%s)",a); break;
        default: snprintf(o,256,"(%s).xy",a); } return o; }
static const char* dg_grd(int dim,const char*a){ char*o=dg_pool();      // explicit derivative (sample_d)
    switch(dim){ case 5: case 6: case 10: snprintf(o,256,"(%s).xyz",a); break;
        case 7: snprintf(o,256,"vec2((%s).x,0.0)",a); break;
        default: snprintf(o,256,"(%s).xy",a); } return o; }
static const char* dg_offs(int dim,const DgIns*in){ char*o=dg_pool();   // aoffimmi -> constant ivec offset ("" = none)
    if(!in->has_aoff || dim==6 || dim==10) return o;
    if(dim==5) snprintf(o,256,"ivec3(%d,%d,%d)",in->aoff[0],in->aoff[1],in->aoff[2]);
    else if(dim==7) snprintf(o,256,"ivec2(%d,0)",in->aoff[0]);
    else snprintf(o,256,"ivec2(%d,%d)",in->aoff[0],in->aoff[1]);
    return o; }
// ld: the address operand is INTEGER (texel coords + mip in .w) -- the register holds int bits (e.g. from
// ftoi), so it must be floatBitsToInt'd. The old ivec2(float) turned every int address into ~0 -> each ld
// read texel (0,0).
static const char* dg_fetch(int dim,unsigned t,const char*a,const char*off){ char*o=dg_pool(); int ho=off&&off[0];
    char P[200],lod[120];
    switch(dim){
        case 5: case 8: case 9: snprintf(P,sizeof P,"floatBitsToInt(%s).xyz",a); snprintf(lod,sizeof lod,"floatBitsToInt(%s).w",a); break;
        case 7: snprintf(P,sizeof P,"ivec3(floatBitsToInt(%s).x,0,floatBitsToInt(%s).y)",a,a); snprintf(lod,sizeof lod,"floatBitsToInt(%s).w",a); break;
        case 4: snprintf(P,sizeof P,"floatBitsToInt(%s).xy",a); snprintf(lod,sizeof lod,"0"); break;
        case 1: snprintf(P,sizeof P,"ivec2(floatBitsToInt(%s).x,0)",a); snprintf(lod,sizeof lod,"0"); break;
        default: snprintf(P,sizeof P,"floatBitsToInt(%s).xy",a); snprintf(lod,sizeof lod,"floatBitsToInt(%s).w",a); }
    if(ho) snprintf(o,256,"texelFetchOffset(t%u,%s,%s,%s)",t,P,lod,off); else snprintf(o,256,"texelFetch(t%u,%s,%s)",t,P,lod);
    return o; }

static int dg_is_word(int c){ return isalnum(c)||c=='_'; }
// collect ids matching prefix (e.g. "r","cb") followed by digits, with word boundaries, into seen[].
static void dg_scan_ids(const char*body,const char*prefix,int*seen,int seencap){
    size_t pl=strlen(prefix), n=strlen(body);
    for(size_t i=0;i+pl<=n;i++){
        if(i>0 && dg_is_word((unsigned char)body[i-1])) continue;
        if(strncmp(body+i,prefix,pl)) continue;
        size_t j=i+pl; if(j>=n || !isdigit((unsigned char)body[j])) continue;
        int val=0; while(j<n && isdigit((unsigned char)body[j])){ val=val*10+(body[j]-'0'); j++; }
        if(j<n && dg_is_word((unsigned char)body[j])) continue;   // boundary after digits
        if(val>=0 && val<seencap) seen[val]=1;
    }
}
static char* dg_join(DgLines*L){ size_t t=0; for(int i=0;i<L->n;i++) t+=strlen(L->v[i])+1; char*s=malloc(t+1); size_t o=0;
    for(int i=0;i<L->n;i++){ size_t l=strlen(L->v[i]); memcpy(s+o,L->v[i],l); o+=l; if(i+1<L->n) s[o++]='\n'; } s[o]=0; return s; }

char* dg_emit(const char*stage,const DgSig*isgn,int ni,const DgSig*osgn,int no,DgIns*ins,int nins){
    DgLines L={0};
    dg_lprintf(&L,"#version 410 core");
    dg_lprintf(&L,"// stage=%s",stage);
    // declaration scan
    int maxt=-1; int cbs[64]; for(int i=0;i<64;i++) cbs[i]=0; int cbused[64]={0};
    int res_[256]={0};
    // dcl_resource dimension per t# (D3D10_SB_RESOURCE_DIMENSION at opcode bits 11..15): 3=2D 5=3D 6=CUBE
    // 7=1DARRAY 8=2DARRAY 9=2DMSARRAY 10=CUBEARRAY. Declaring everything sampler2D and sampling .xy made cube
    // maps / texture arrays (env maps, lightmap pages) read the wrong texels entirely.
    int rdim[256]={0}; int dclsz[64]={0};
    for(int x=0;x<nins;x++) if(ins[x].op==89 && ins[x].nop && ins[x].ops[0].nidx>1){ uint32_t b=ins[x].ops[0].idx[0].val, n=ins[x].ops[0].idx[1].val; if(b<64 && n>0 && n<=4096) dclsz[b]=(int)n; }
    for(int x=0;x<nins;x++) if(ins[x].op==88 && ins[x].nop && ins[x].ops[0].nidx){ uint32_t r=ins[x].ops[0].idx[0].val; if(r<256) rdim[r]=(int)((ins[x].tok>>11)&0x1f); }
    // StructuredBuffer / ByteAddressBuffer SRVs (dcl_resource_structured 162 / _raw 161): GL texture buffer of R32UI
    // dwords (particle state read by the particle vertex shaders). rdim 100 = structured, 101 = raw; stride kept.
    int rstride[256]={0};
    // resources read with sample_c / sample_c_lz -> GL shadow samplers (hardware compare + PCF with the D3D
    // sampler's ComparisonFunc). Was: texture().r >= ref, ignoring the comparison function and filtering.
    int rcmp[256]={0};
    for(int x=0;x<nins;x++) if((ins[x].op==70||ins[x].op==71) && ins[x].nop>2 && ins[x].ops[2].nidx){ uint32_t r=ins[x].ops[2].idx[0].val; if(r<256) rcmp[r]=1; }
    for(int x=0;x<nins;x++) if((ins[x].op==162||ins[x].op==161) && ins[x].nop && ins[x].ops[0].type==T_RESOURCE && ins[x].ops[0].nidx){ uint32_t r=ins[x].ops[0].idx[0].val;
        if(r<256){ rdim[r]=ins[x].op==162?100:101; rstride[r]=ins[x].op==162?(int)ins[x].raw[2]:4; res_[r]=1; } }
    for(int x=0;x<nins;x++){ for(int k=0;k<ins[x].nop;k++){ DgOp*op=&ins[x].ops[k];
        if(op->type==T_TEMP && op->nidx){ if((int)op->idx[0].val>maxt) maxt=(int)op->idx[0].val; }
        if(op->type==T_CB && op->nidx){ int b=op->idx[0].val; if(b>=0&&b<64){ int need=64;
            if(op->nidx>1){ DgIdx*ri=&op->idx[1]; if(ri->kind==0) need=ri->val+1; else { need=ri->val+4096; if(need<4096) need=4096; } }
            if(need<64) need=64; if(need>cbs[b]) cbs[b]=need; cbused[b]=1; } }
        if(op->type==T_RESOURCE && op->nidx){ uint32_t r=op->idx[0].val; if(r<256) res_[r]=1; }
    } }
    int ntemps=maxt+1;
    // _SYSIN + inputs
    for(int i=0;i<64;i++) g_sysin[i]=NULL;
    int seen_in[256]={0};
    for(int i=0;i<ni;i++){ const DgSig*s=&isgn[i];
        // PS SV_Position = pixel-center coords in render-target space (top-left origin), z = depth, w = clip w.
        // The pipeline renders D3D row y into GL row y (VS y-flip, dx_posfix), so gl_FragCoord.xy IS that;
        // GL's .w is 1/w. (Was: skipped -> read back as an unbound varying in 1841/2426 pixel shaders.)
        if(!strcmp(stage,"ps") && !strcmp(s->name,"SV_Position")){ if(s->reg<64) g_sysin[s->reg]="vec4(gl_FragCoord.xy,gl_FragCoord.z,1.0/gl_FragCoord.w)"; continue; }
        if(!strcmp(s->name,"SV_VertexID")){ if(s->reg<64) g_sysin[s->reg]="vec4(float(gl_VertexID))"; continue; }
        if(!strcmp(s->name,"SV_InstanceID")){ if(s->reg<64) g_sysin[s->reg]="vec4(float(gl_InstanceID))"; continue; }
        if(s->reg<256){ if(seen_in[s->reg]) continue; seen_in[s->reg]=1; }
        dg_lprintf(&L,"in vec4 v%u; // %s%u",s->reg,s->name,s->idx);
    }
    int seen_out[256]={0};
    for(int i=0;i<no;i++){ const DgSig*s=&osgn[i];
        if(!strcmp(s->name,"SV_Position")) continue;
        if(!strncmp(s->name,"SV_Depth",8) || s->reg>=0xffffffffu) continue;
        if(s->reg<256){ if(seen_out[s->reg]) continue; seen_out[s->reg]=1; }
        dg_lprintf(&L,"out vec4 o%u; // %s%u",s->reg,s->name,s->idx);
    }
    for(int b=0;b<64;b++) if(cbused[b]){ int n = (dclsz[b] && !g_dg_nodclsz) ? dclsz[b] : cbs[b];
        if(g_dg_ubo) dg_lprintf(&L,"layout(std140) uniform cb%d_blk { vec4 cb%d[%d]; };",b,b,n); else dg_lprintf(&L,"uniform vec4 cb%d[%d];",b,n); }
    for(int r=0;r<256;r++) if(res_[r]){
        if(rcmp[r] && !g_dg_nocmp){ const char*st = (rdim[r]==6)?"samplerCubeShadow":(rdim[r]==8||rdim[r]==7)?"sampler2DArrayShadow":"sampler2DShadow";
            dg_lprintf(&L,"uniform %s t%d;",st,r); }
        else dg_lprintf(&L,"uniform %s t%d;",dg_samptype(rdim[r]),r); }
    int isvs=!strcmp(stage,"vs");
    // D3D -> GL clip space at every VS exit: y is flipped so GL row y holds D3D row y (render targets match
    // D3D's top-down layout, like uploaded textures; present flips once), and D3D's z in [0,w] maps onto
    // GL's [-w,w] (D3D near-plane clipping; window depth = z/w as in D3D).
    if(isvs) dg_lprintf(&L,"void dx_posfix(){\n#ifndef DX_NOYFLIP\n  gl_Position.y=-gl_Position.y;\n#endif\n  gl_Position.z=2.0*gl_Position.z-gl_Position.w;\n}");
    if(g_icbn>=4){ int nv=g_icbn/4; size_t cap=(size_t)nv*80+64; char*b=malloc(cap); size_t bl=0;
        bl+=snprintf(b+bl,cap-bl,"const vec4 dx_icb[%d] = vec4[%d](",nv,nv);
        for(int v=0;v<nv;v++){ char f[4][48]; for(int c=0;c<4;c++) dg_fmtf(g_icb[v*4+c],f[c],sizeof f[c]);
            bl+=snprintf(b+bl,cap-bl,"%svec4(%s,%s,%s,%s)",v?",":"",f[0],f[1],f[2],f[3]); }
        snprintf(b+bl,cap-bl,");"); dg_lpush(&L,b); }
    dg_lprintf(&L,"void main(){");
    for(int i=0;i<ntemps;i++) dg_lprintf(&L,"  vec4 r%d=vec4(0.0);",i);
    for(int x=0;x<nins;x++) if(ins[x].op==105){ unsigned xr=ins[x].raw[0], xn=ins[x].raw[1]; if(xn>0 && xn<=4096) dg_lprintf(&L,"  vec4 x%u[%u];",xr,xn); }

    DgCtx ctx={osgn,no};
    // control-flow stack
    typedef struct { int kind; char sv[16]; int first; } CF; // kind 0 if,1 else,2 loop,3 switch
    CF cf[64]; int cfn=0;
    #define S(k) ( (k)<ins[x].nop ? dg_src(&ins[x].ops[k]) : "vec4(0.0)" )
    for(int x=0;x<nins;x++){
        const char*n=ins[x].name; int nopn=ins[x].nop; DgOp*o=ins[x].ops;
        g_dg_sat = (ins[x].op<88) && ((ins[x].tok>>13)&1);
        if(!n){ continue; } // op%d unknown -> skipped like startswith('op')
        if(!strcmp(n,"dcl_cb")||!strcmp(n,"dcl_sampler")||!strcmp(n,"dcl_resource")||!strcmp(n,"dcl_input")||
           !strcmp(n,"dcl_input_ps")||!strcmp(n,"dcl_input_sgv")||!strcmp(n,"dcl_input_ps_sgv")||
           !strcmp(n,"dcl_input_ps_siv")||!strcmp(n,"dcl_input_siv")||!strcmp(n,"dcl_output")||
           !strcmp(n,"dcl_output_sgv")||!strcmp(n,"dcl_output_siv")||!strcmp(n,"dcl_temps")||
           !strcmp(n,"dcl_globalflags")||!strcmp(n,"dcl_indexable_temp")||!strcmp(n,"customdata")||!strcmp(n,"nop")||
           !strcmp(n,"dcl_resource_raw")||!strcmp(n,"dcl_resource_structured")) continue;
        const char*cmp = ins[x].test ? "!=" : "==";
        // conditions test the register's BITS (D3D: any nonzero 32-bit value is true; masks are 0xFFFFFFFF)
        if(!strcmp(n,"ret")){ dg_lprintf(&L,isvs?"  { dx_posfix(); return; }":"  return;"); continue; }
        if(!strcmp(n,"retc")){ dg_lprintf(&L,isvs?"  if(floatBitsToInt((%s).x) %s 0){ dx_posfix(); return; }":"  if(floatBitsToInt((%s).x) %s 0) return;",S(0),cmp); continue; }
        if(!strcmp(n,"if")){ dg_lprintf(&L,"  if(floatBitsToInt((%s).x) %s 0){",S(0),cmp); cf[cfn].kind=0; cf[cfn].first=0; cfn++; continue; }
        if(!strcmp(n,"else")){ if(cfn && cf[cfn-1].kind==0){ cf[cfn-1].kind=1; dg_lprintf(&L,"  }else{"); } continue; }
        if(!strcmp(n,"endif")){ if(cfn && (cf[cfn-1].kind==0||cf[cfn-1].kind==1)){ cfn--; dg_lprintf(&L,"  }"); } continue; }
        if(!strcmp(n,"loop")){ dg_lprintf(&L,"  for(int _li=0;_li<255;_li++){"); cf[cfn].kind=2; cfn++; continue; }
        if(!strcmp(n,"endloop")){ if(cfn && cf[cfn-1].kind==2){ cfn--; dg_lprintf(&L,"  }"); } continue; }
        if(!strcmp(n,"continue")){ int inl=0; for(int e=0;e<cfn;e++) if(cf[e].kind==2) inl=1; if(inl) dg_lprintf(&L,"  continue;"); continue; }
        if(!strcmp(n,"breakc")){ int inl=0; for(int e=0;e<cfn;e++) if(cf[e].kind==2) inl=1;
            if(inl) dg_lprintf(&L,"  if(floatBitsToInt((%s).x) %s 0) break;",S(0),cmp); else dg_lprintf(&L,"  // breakc (switch) omitted"); continue; }
        if(!strcmp(n,"break")){ int innerloop=-1; for(int e=cfn-1;e>=0;e--){ if(cf[e].kind==2){ innerloop=1; break; } if(cf[e].kind==3){ innerloop=0; break; } }
            if(innerloop==1) dg_lprintf(&L,"  break;"); else dg_lprintf(&L,"  // break (switch) -> if-chain"); continue; }
        if(!strcmp(n,"discard")){ dg_lprintf(&L,"  if(floatBitsToInt((%s).x) %s 0) discard;",S(0),cmp); continue; } // discard_z/_nz (alpha test)
        if(!strcmp(n,"switch")){ snprintf(cf[cfn].sv,sizeof cf[cfn].sv,"_sw%d",cfn); dg_lprintf(&L,"  { int %s=floatBitsToInt((%s).x);",cf[cfn].sv,S(0)); cf[cfn].kind=3; cf[cfn].first=0; cfn++; continue; }
        if(!strcmp(n,"case")){ int cv=(nopn && o[0].has_imm)?(int)o[0].imm[0]:0; if(cfn && cf[cfn-1].kind==3){ CF*st=&cf[cfn-1];
            dg_lprintf(&L,"  %sif(%s==%d){", st->first?"}else ":"", st->sv, cv); st->first=1; } continue; }
        if(!strcmp(n,"default")){ if(cfn && cf[cfn-1].kind==3){ CF*st=&cf[cfn-1]; dg_lprintf(&L,"  %s{", st->first?"}else ":""); st->first=1; } continue; }
        if(!strcmp(n,"endswitch")){ if(cfn && cf[cfn-1].kind==3){ cfn--; dg_lprintf(&L,"  } }"); } continue; }
        if(nopn==0){ dg_lprintf(&L,"  // %s (no operands)",n); continue; }
        // integer helpers
        // integer operand: the NEG modifier is two's-complement negation of the int (not a float sign flip)
        #define SI(k) ({ char*_t=dg_pool(); if((k)<nopn && (o[k].emod&1)){ DgOp _c=o[k]; _c.emod&=~1; snprintf(_t,256,"(-floatBitsToInt(%s))",dg_src(&_c)); } else snprintf(_t,256,"floatBitsToInt(%s)",S(k)); _t; })
        #define TX(k) ( (k)<nopn && o[k].nidx ? o[k].idx[0].val : 0 )
        #define RSW(k) ({ char*_r=dg_pool(); if((k)<nopn) dg_swz(&o[k],_r); else strcpy(_r,"xyzw"); _r; })
        #define DIM2 ( TX(2)<256 ? rdim[TX(2)] : 0 )
        char*line=NULL;
        if(!strcmp(n,"iadd")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s + %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"imul")){   // imul destHI, destLO, a, b : low 32 bits -> destLO, high -> destHI
            if(nopn>3){ if(o[0].type!=T_NULL) dg_lprintf(&L,"  { ivec4 _hi,_lo; imulExtended(%s,%s,_hi,_lo); %s }",SI(2),SI(3),
                            ({ char*a=dg_asg_i(&ctx,&o[0],"_hi"); char*t=dg_pool(); snprintf(t,256,"%s",a+2); free(a); t; }));
                        line=dg_asg_i(&ctx,&o[1],({char*t=dg_pool();snprintf(t,256,"%s * %s",SI(2),SI(3));t;})); }
            else line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s*%s",SI(1),SI(2));t;})); }
        else if(!strcmp(n,"imin")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"min(%s,%s)",SI(1),SI(2));t;}));
        else if(!strcmp(n,"imax")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"max(%s,%s)",SI(1),SI(2));t;}));
        else if(!strcmp(n,"imad")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s*%s+%s",SI(1),SI(2),SI(3));t;}));
        else if(!strcmp(n,"and")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s & %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"or")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s | %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"xor")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s ^ %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"not")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"~%s",SI(1));t;}));
        else if(!strcmp(n,"ishl")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s << %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ishr")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s >> %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ushr")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"ivec4(uvec4(%s) >> uvec4(%s))",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ftoi")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"ivec4(%s)",S(1));t;}));
        else if(!strcmp(n,"ftou")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"ivec4(uvec4(%s))",S(1));t;}));
        else if(!strcmp(n,"itof")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(%s)",SI(1));t;}));
        else if(!strcmp(n,"utof")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(uvec4(%s))",SI(1));t;}));
        else if(!strcmp(n,"ld_ms")||!strcmp(n,"ld")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s.%s",dg_fetch(DIM2,(unsigned)TX(2),S(1),dg_offs(DIM2,&ins[x])),RSW(2));t;}));
        else if(!strcmp(n,"sample_d")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool(); const char*of=dg_offs(DIM2,&ins[x]);
            if(of[0]) snprintf(t,256,"textureGradOffset(t%u,%s,%s,%s,%s).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),dg_grd(DIM2,S(3)),dg_grd(DIM2,S(4)),of,RSW(2));
            else snprintf(t,256,"textureGrad(t%u,%s,%s,%s).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),dg_grd(DIM2,S(3)),dg_grd(DIM2,S(4)),RSW(2)); t;}));
        else if(!strcmp(n,"resinfo")){   // resinfo[_rcpFloat|_uint] dst, mip, t# -> (w,h,depth|slices,levels) swizzled by the t# operand
            int rt=(int)((ins[x].tok>>11)&3), dm=DIM2; char*t=dg_pool(); const char*sz=dg_pool();
            if(dm==5||dm==7||dm==8||dm==9||dm==10) snprintf((char*)sz,256,"vec4(textureSize(t%u,floatBitsToInt(%s).x),1)",(unsigned)TX(2),S(1));
            else snprintf((char*)sz,256,"vec4(textureSize(t%u,floatBitsToInt(%s).x),%s,1)",(unsigned)TX(2),S(1),dm==6?"6":"1");
            if(rt==2) snprintf(t,256,"intBitsToFloat(ivec4(%s)).%s",sz,RSW(2));
            else if(rt==1) snprintf(t,256,"(vec4(1.0,1.0,1.0,0.0)/max(%s,vec4(1e-30))+vec4(0,0,0,1)).%s",sz,RSW(2));
            else snprintf(t,256,"(%s).%s",sz,RSW(2));
            line=dg_asg(&ctx,&o[0],t); }
        else if(!strcmp(n,"mov")) line=dg_asg(&ctx,&o[0],S(1));
        else if((ins[x].op==167||ins[x].op==165) && nopn>=3){   // ld_structured dst, idx, byteoff, t# | ld_raw dst, byteaddr, t#
            int ri=ins[x].op==167?3:2; unsigned tr=(ri<nopn&&o[ri].nidx)?o[ri].idx[0].val:0; char sw[5]; if(ri<nopn) dg_swz(&o[ri],sw); else strcpy(sw,"xyzw");
            char a1[256]; snprintf(a1,256,"%s",SI(1)); char e[512];
            if(ins[x].op==167){ char a2[256]; snprintf(a2,256,"%s",SI(2)); snprintf(e,512,"uintBitsToFloat(dx_ldb(t%u,((%s).x*%d+(%s).x)>>2)).%s",tr,a1,tr<256?rstride[tr]:4,a2,sw); }
            else snprintf(e,512,"uintBitsToFloat(dx_ldb(t%u,(%s).x>>2)).%s",tr,a1,sw);
            line=dg_asg(&ctx,&o[0],e); }
        // screen-space derivatives: GL row y == D3D row y (VS y-flip), so dFdy has D3D's sign
        else if(!strncmp(n,"deriv_rtx",9)) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"dFdx(%s)",S(1));t;}));
        else if(!strncmp(n,"deriv_rty",9)) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"dFdy(%s)",S(1));t;}));
        else if(!strcmp(n,"rcp")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"(vec4(1.0)/%s)",S(1));t;}));
        else if(!strcmp(n,"f16tof32")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"dx_h2f(floatBitsToUint(%s))",S(1));t;}));
        else if(!strcmp(n,"f32tof16")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"ivec4(dx_f2h(%s))",S(1));t;}));
        else if(!strcmp(n,"ubfe")||!strcmp(n,"ibfe")||!strcmp(n,"bfi")){   // per-lane bitfield ops (width/offset & 31)
            int u=(n[0]=='u'), bi=(n[1]=='f'); char*e[4]; const char*C="xyzw"; char a1[256],a2[256],a3[256],a4[256];
            snprintf(a1,256,"%s",S(1)); snprintf(a2,256,"%s",S(2)); snprintf(a3,256,"%s",S(3)); snprintf(a4,256,"%s",S(4));
            for(int c=0;c<4;c++){
                if(bi) asprintf(&e[c],"bitfieldInsert(floatBitsToInt(%s.%c),floatBitsToInt(%s.%c),floatBitsToInt(%s.%c)&31,floatBitsToInt(%s.%c)&31)",a4,C[c],a3,C[c],a2,C[c],a1,C[c]);
                else asprintf(&e[c],"%sbitfieldExtract(%s(%s.%c),floatBitsToInt(%s.%c)&31,floatBitsToInt(%s.%c)&31)%s",u?"int(":"",u?"floatBitsToUint":"floatBitsToInt",a3,C[c],a2,C[c],a1,C[c],u?")":""); }
            char*t; asprintf(&t,"ivec4(%s,%s,%s,%s)",e[0],e[1],e[2],e[3]); line=dg_asg_i(&ctx,&o[0],t); free(t); for(int c=0;c<4;c++) free(e[c]); }
        else if(!strcmp(n,"bfrev")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"bitfieldReverse(%s)",SI(1));t;}));
        else if(!strcmp(n,"countbits")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"bitCount(%s)",SI(1));t;}));
        else if(!strcmp(n,"gather4")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"textureGather(t%u,%s,%d).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),nopn>3&&o[3].selmode==2?o[3].sel:0,RSW(2));t;}));
        else if(!strcmp(n,"add")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s + %s",S(1),S(2));t;}));
        else if(!strcmp(n,"mul")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s * %s",S(1),S(2));t;}));
        else if(!strcmp(n,"div")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s / %s",S(1),S(2));t;}));
        else if(!strcmp(n,"mad")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s * %s + %s",S(1),S(2),S(3));t;}));
        else if(!strcmp(n,"min")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"min(%s,%s)",S(1),S(2));t;}));
        else if(!strcmp(n,"max")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"max(%s,%s)",S(1),S(2));t;}));
        else if(!strcmp(n,"frc")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"fract(%s)",S(1));t;}));
        else if(!strcmp(n,"exp")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"exp2(%s)",S(1));t;}));
        else if(!strcmp(n,"log")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"log2(%s)",S(1));t;}));
        else if(!strcmp(n,"sqrt")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"sqrt(%s)",S(1));t;}));
        else if(!strcmp(n,"rsq")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"inversesqrt(%s)",S(1));t;}));
        else if(!strcmp(n,"round_z")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"trunc(%s)",S(1));t;}));
        else if(!strcmp(n,"round_ni")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"floor(%s)",S(1));t;}));
        else if(!strcmp(n,"round_pi")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"ceil(%s)",S(1));t;}));
        else if(!strcmp(n,"round_ne")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"roundEven(%s)",S(1));t;}));
        else if(!strcmp(n,"ineg")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-%s",SI(1));t;}));
        // comparisons produce D3D masks (0xFFFFFFFF / 0); integer forms compare the registers' int bits
        else if(!strcmp(n,"ult")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(lessThan(floatBitsToUint(%s),floatBitsToUint(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"uge")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(greaterThanEqual(floatBitsToUint(%s),floatBitsToUint(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"sample_b")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool(); const char*of=dg_offs(DIM2,&ins[x]);
            if(of[0]) snprintf(t,256,"textureOffset(t%u,%s,%s,(%s).x).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),of,S(4),RSW(2));
            else snprintf(t,256,"texture(t%u,%s,(%s).x).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),S(4),RSW(2)); t;}));
        else if(!strcmp(n,"sincos")){   // sincos dst_sin, dst_cos, src  (either destination may be null)
            if(o[0].type!=T_NULL) dg_lpush(&L,dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"sin(%s)",S(2));t;})));
            if(nopn>1 && o[1].type!=T_NULL) line=dg_asg(&ctx,&o[1],({char*t=dg_pool();snprintf(t,256,"cos(%s)",S(2));t;})); }
        else if(!strcmp(n,"dp2")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot((%s).xy,(%s).xy))",S(1),S(2));t;}));
        else if(!strcmp(n,"dp3")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot((%s).xyz,(%s).xyz))",S(1),S(2));t;}));
        else if(!strcmp(n,"dp4")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"movc")) line = (nopn>=4)?dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"mix(vec4(%s),vec4(%s),notEqual(floatBitsToInt(%s),ivec4(0)))",S(3),S(2),S(1));t;})):dg_asg(&ctx,&o[0],S(1));
        else if(!strcmp(n,"ne")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(notEqual(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"eq")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(equal(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"lt")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(lessThan(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"ge")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(greaterThanEqual(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"ine")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(notEqual(%s,%s))",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ieq")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(equal(%s,%s))",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ilt")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(lessThan(%s,%s))",SI(1),SI(2));t;}));
        else if(!strcmp(n,"ige")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"-ivec4(greaterThanEqual(%s,%s))",SI(1),SI(2));t;}));
        else if(!strcmp(n,"sample")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool(); const char*of=dg_offs(DIM2,&ins[x]);
            if(of[0]) snprintf(t,256,"textureOffset(t%u,%s,%s).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),of,RSW(2));
            else snprintf(t,256,"texture(t%u,%s).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),RSW(2)); t;}));
        else if(!strcmp(n,"sample_l")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool(); const char*of=dg_offs(DIM2,&ins[x]);
            if(of[0]) snprintf(t,256,"textureLodOffset(t%u,%s,(%s).x,%s).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),S(4),of,RSW(2));
            else snprintf(t,256,"textureLod(t%u,%s,(%s).x).%s",(unsigned)TX(2),dg_crd(DIM2,S(1)),S(4),RSW(2)); t;}));
        else if(!strcmp(n,"sample_c")||!strcmp(n,"sample_c_lz")){ char*t=dg_pool(); const char*ref=nopn>4?S(4):"vec4(0.5)"; int dm=DIM2;
            if(g_dg_nocmp) snprintf(t,256,"vec4(textureLod(t%u,%s,0.0).r >= (%s).x ?1.0:0.0)",(unsigned)TX(2),dg_crd(dm,S(1)),ref);
            else if(dm==6) snprintf(t,256,"vec4(texture(t%u,vec4((%s).xyz,(%s).x)))",(unsigned)TX(2),S(1),ref);
            else if(dm==8||dm==7) snprintf(t,256,"vec4(texture(t%u,vec4((%s).xyz,(%s).x)))",(unsigned)TX(2),S(1),ref);
            else snprintf(t,256,"vec4(texture(t%u,vec3((%s).xy,(%s).x)))",(unsigned)TX(2),S(1),ref);
            line=dg_asg(&ctx,&o[0],t); }
        else dg_lprintf(&L,"  // UNTRANSLATED: %s",n);
        if(line) dg_lpush(&L,line);
    }
    #undef S
    #undef SI
    #undef TX
    #undef RSW
    #undef DIM2
    // --- safety passes: declare any referenced-but-undeclared identifier, then balance braces ---
    char*body=dg_join(&L);
    int has_t[256]={0},has_cb[256]={0},has_v[256]={0},has_o[256]={0},has_r[256]={0};
    // what's already declared
    { // declared samplers "uniform sampler2D t%d", cbs "uniform vec4 cb%d", in v%d, out o%d, temps "vec4 r%d=vec4"
      for(int i=0;i<L.n;i++){ int d; const char*s=L.v[i];
        if(!strncmp(s,"uniform sampler",15)||!strncmp(s,"uniform usampler",16)){ const char*q=strrchr(s,' '); if(q && sscanf(q," t%d;",&d)==1 && d>=0 && d<256) has_t[d]=1; }
        if(sscanf(s,"uniform vec4 cb%d[",&d)==1 && d<256) has_cb[d]=1;
        if(sscanf(s,"layout(std140) uniform cb%d_blk",&d)==1 && d<256) has_cb[d]=1;
        if(sscanf(s,"in vec4 v%d;",&d)==1 && d<256) has_v[d]=1;
        if(sscanf(s,"out vec4 o%d;",&d)==1 && d<256) has_o[d]=1;
        if(sscanf(s,"  vec4 r%d=vec4",&d)==1 && d<256) has_r[d]=1;
      }
    }
    int ref_t[256]={0},ref_cb[256]={0},ref_v[256]={0},ref_o[256]={0},ref_r[256]={0};
    dg_scan_ids(body,"t",ref_t,256); dg_scan_ids(body,"cb",ref_cb,256);
    dg_scan_ids(body,"v",ref_v,256); dg_scan_ids(body,"o",ref_o,256); dg_scan_ids(body,"r",ref_r,256);
    int has_glpos = strstr(body,"gl_Position")!=NULL;
    int need_h2f = strstr(body,"dx_h2f(")!=NULL, need_f2h = strstr(body,"dx_f2h(")!=NULL, need_ldb = strstr(body,"dx_ldb(")!=NULL;
    // find insertion index (the 'void main(){' line)
    int mi=0; for(int i=0;i<L.n;i++) if(!strncmp(L.v[i],"void main",9)){ mi=i; break; }
    // header decls (t, cb, v, o) inserted before main, in that order, ascending — matches Python
    int ins_at=mi;
    for(int m=0;m<256;m++) if(ref_t[m]&&!has_t[m]){ char*s; asprintf(&s,"uniform sampler2D t%d;",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_cb[m]&&!has_cb[m]){ char*s; if(g_dg_ubo) asprintf(&s,"layout(std140) uniform cb%d_blk { vec4 cb%d[4096]; };",m,m); else asprintf(&s,"uniform vec4 cb%d[4096];",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_v[m]&&!has_v[m]){ char*s; asprintf(&s,"in vec4 v%d;",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_o[m]&&!has_o[m]){ if(m!=0||!has_glpos){ char*s; asprintf(&s,"out vec4 o%d;",m); dg_linsert(&L,ins_at++,s); mi++; } }
    if(need_ldb){ dg_linsert(&L,ins_at++,strdup("uvec4 dx_ldb(usamplerBuffer b,int dw){ return uvec4(texelFetch(b,dw).x,texelFetch(b,dw+1).x,texelFetch(b,dw+2).x,texelFetch(b,dw+3).x); }")); mi++; }
    // half-float helpers (packHalf2x16/unpackHalf2x16 are GLSL 4.20; we target 4.10)
    if(need_h2f){ dg_linsert(&L,ins_at++,strdup("float dx_h2f1(uint h){ uint e=(h>>10)&31u, m=h&1023u; float v = e==0u ? float(m)*exp2(-24.0) : (e==31u ? (m==0u?uintBitsToFloat(0x7f800000u):uintBitsToFloat(0x7fc00000u)) : (1.0+float(m)/1024.0)*exp2(float(e)-15.0)); return ((h>>15)&1u)!=0u ? -v : v; }\nvec4 dx_h2f(uvec4 h){ return vec4(dx_h2f1(h.x),dx_h2f1(h.y),dx_h2f1(h.z),dx_h2f1(h.w)); }")); mi++; }
    if(need_f2h){ dg_linsert(&L,ins_at++,strdup("uint dx_f2h1(float f){ uint b=floatBitsToUint(f), s=(b>>16)&0x8000u; int e=int((b>>23)&255u)-112; uint m=b&0x7fffffu; if(e<=0) return s; if(e>=31) return s|0x7c00u; return s|(uint(e)<<10)|(m>>13); }\nuvec4 dx_f2h(vec4 f){ return uvec4(dx_f2h1(f.x),dx_f2h1(f.y),dx_f2h1(f.z),dx_f2h1(f.w)); }")); mi++; }
    // missing temps right after 'void main(){'
    int tat=mi+1;
    for(int m=0;m<256;m++) if(ref_r[m]&&!has_r[m]){ char*s; asprintf(&s,"  vec4 r%d=vec4(0.0);",m); dg_linsert(&L,tat++,s); }
    free(body);
    // brace balance
    body=dg_join(&L); int depth=0; for(char*p=body;*p;p++){ if(*p=='{')depth++; else if(*p=='}')depth--; }
    free(body);
    if(depth>1){ char*s; asprintf(&s,"  %.*s // auto-closed %d block(s)", depth-1, "}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}", depth-1);
        // ensure enough braces if depth-1 > 32
        if(depth-1>32){ char*b2=malloc(depth); memset(b2,'}',depth-1); b2[depth-1]=0; free(s); asprintf(&s,"  %s // auto-closed %d block(s)",b2,depth-1); free(b2); }
        dg_lpush(&L,s); }
    if(isvs) dg_lprintf(&L,"  dx_posfix();");
    dg_lprintf(&L,"}");
    char*out=dg_join(&L);
    for(int i=0;i<L.n;i++) free(L.v[i]); free(L.v);
    return out;
}

// top-level: bytes -> GLSL (malloc'd) or NULL
char* dg_translate(const uint8_t*d,size_t len){
    if(!d || len<32 || memcmp(d,"DXBC",4)) return NULL;
    uint32_t nch=dg_u32(d,len,28);
    size_t isgn=0,osgn=0,shex=0,shex_sz=0; int have_shex=0,have_isgn=0,have_osgn=0;
    for(uint32_t i=0;i<nch;i++){ size_t o=dg_u32(d,len,32+4*i); if(o+8>len) continue;
        char fc[5]={0}; memcpy(fc,d+o,4); uint32_t sz=dg_u32(d,len,o+4); size_t data=o+8;
        if(!memcmp(fc,"ISGN",4)){ isgn=data; have_isgn=1; }
        else if(!memcmp(fc,"OSGN",4)){ osgn=data; have_osgn=1; }
        else if(!memcmp(fc,"SHEX",4)){ shex=data; shex_sz=sz; have_shex=2; }
        else if(!memcmp(fc,"SHDR",4) && have_shex!=2){ shex=data; shex_sz=sz; have_shex=1; }
    }
    if(!have_shex) return NULL;
    static DgSig sig_in[64], sig_out[64];
    int ni=have_isgn?dg_parse_sig(d,len,isgn,sig_in,64):0;
    int no=have_osgn?dg_parse_sig(d,len,osgn,sig_out,64):0;
    DgIns*ins=calloc(8192,sizeof(DgIns)); if(!ins) return NULL; int nins=0;
    g_icbn=0;
    int ptype=dg_decode_program(d,len,shex,shex_sz,ins,&nins,8192);
    int pt=(ptype>0xfff)?(ptype>>12):ptype;
    const char*PT[]={"ps","vs","gs","hs","ds","cs"};
    const char*stage=(pt>=0&&pt<6)?PT[pt]:"ps";
    char*glsl=dg_emit(stage,sig_in,ni,sig_out,no,ins,nins);
    free(ins);
    return glsl;
}

#ifdef DXBC2GLSL_MAIN
int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <shader.dxbc>\n",argv[0]); return 2; }
    FILE*f=fopen(argv[1],"rb"); if(!f){ perror("open"); return 1; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n){ return 1; } fclose(f);
    char*g=dg_translate(d,n);
    if(!g){ fprintf(stderr,"translate failed\n"); return 1; }
    fputs(g,stdout); fputc('\n',stdout);
    return 0;
}
#endif
