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
    if(isnan(v)||isinf(v)){ snprintf(out,n,"uintBitsToFloat(%uu)",(unsigned)bits); return; }
    snprintf(out,n,"%.9g",(double)v);
}

// ---- operand model ----
enum { T_TEMP=0,T_INPUT=1,T_OUTPUT=2,T_INDEXABLE_TEMP=3,T_IMM32=4,T_IMM64=5,
       T_SAMPLER=6,T_RESOURCE=7,T_CB=8,T_ICB=9,T_LABEL=10,T_INPUT_PRIM=11,
       T_OUTPUT_DEPTH=12,T_NULL=13,T_CONST=14,T_INPUT_GSV=25 };
static const char COMP[4]={'x','y','z','w'};

typedef struct { int kind; uint32_t val; } DgIdx;           // kind 0=imm, 1=rel (val=base immediate)
typedef struct {
    int type, mask, sel, numc, selmode, emod;
    int has_swz; int swz[4];
    int nidx; DgIdx idx[3];
    int has_imm; int immn; uint32_t imm[4];
} DgOp;

typedef struct { int op; const char*name; int test; int nop; DgOp ops[8]; } DgIns;

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
        else if(r==2){ out->idx[k].kind=1; out->idx[k].val=0; long no=dg_decode_operand(d,len,o,NULL); if(no<0)return -1; o=(size_t)no; }
        else if(r==3){ out->idx[k].kind=1; out->idx[k].val=dg_u32(d,len,o); o+=4; long no=dg_decode_operand(d,len,o,NULL); if(no<0)return -1; o=(size_t)no; }
        else if(r==4){ out->idx[k].kind=1; out->idx[k].val=dg_u32(d,len,o); o+=8; long no=dg_decode_operand(d,len,o,NULL); if(no<0)return -1; o=(size_t)no; }
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
            ins[n].op=53; ins[n].name="customdata"; ins[n].test=0; ins[n].nop=0; n++; i+=(size_t)clen*4; continue; }
        int ln=(tok>>24)&0x7f; int ext=(tok>>31)&1; int test=(tok>>18)&1;
        if(ln==0) break;
        const char*nm=dg_ops(op);
        size_t o=i+4;
        if(ext){ while(dg_u32(d,len,o)>>31) o+=4; o+=4; }
        ins[n].op=op; ins[n].name=nm; ins[n].test=test; ins[n].nop=0;
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
        case T_CB:     snprintf(base,sizeof base,"cb%u[%u]",dg_ix(op,0),dg_ix(op,1)); break;
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
    snprintf(o,256,"r%u",ix); return o; }
// DXBC is lane-wise: destination lane i reads lane i of the (already per-lane swizzled) 4-wide source, so
// "mov o0.zw, l(0,0,0.5,0.5)" sets z=0.5,w=0.5. Select the MASK lanes, not the first N lanes -- the old
// "(expr).xy" for a .zw mask wrote w=0 into gl_Position (geometry vanished) and scrambled every non-prefix
// write mask (.y .z .w .xz .yw .zw ...) across all shaders.
static char* dg_asg(DgCtx*c,const DgOp*op,const char*expr){ const char*d=dg_dst(c,op);
    if(!strcmp(d,"gl_FragDepth")){ char*s; asprintf(&s,"  gl_FragDepth = (%s).x;",expr); return s; }
    char m[5]; dg_destmask(op,m);
    char*s; asprintf(&s,"  %s.%s = (%s).%s;",d,m,expr,m); return s; }
static char* dg_asg_i(DgCtx*c,const DgOp*op,const char*expr){ const char*d=dg_dst(c,op);
    if(!strcmp(d,"gl_FragDepth")){ char*s; asprintf(&s,"  gl_FragDepth = intBitsToFloat((%s)).x;",expr); return s; }
    char m[5]; dg_destmask(op,m);
    char*s; asprintf(&s,"  %s.%s = intBitsToFloat((%s)).%s;",d,m,expr,m); return s; }

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
        if(!strcmp(stage,"ps") && !strcmp(s->name,"SV_Position")) continue;
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
    for(int b=0;b<64;b++) if(cbused[b]) dg_lprintf(&L,"uniform vec4 cb%d[%d];",b,cbs[b]);
    for(int r=0;r<256;r++) if(res_[r]) dg_lprintf(&L,"uniform sampler2D t%d;",r);
    dg_lprintf(&L,"void main(){");
    for(int i=0;i<ntemps;i++) dg_lprintf(&L,"  vec4 r%d=vec4(0.0);",i);

    DgCtx ctx={osgn,no};
    // control-flow stack
    typedef struct { int kind; char sv[16]; int first; } CF; // kind 0 if,1 else,2 loop,3 switch
    CF cf[64]; int cfn=0;
    #define S(k) ( (k)<ins[x].nop ? dg_src(&ins[x].ops[k]) : "vec4(0.0)" )
    for(int x=0;x<nins;x++){
        const char*n=ins[x].name; int nopn=ins[x].nop; DgOp*o=ins[x].ops;
        if(!n){ continue; } // op%d unknown -> skipped like startswith('op')
        if(!strcmp(n,"dcl_cb")||!strcmp(n,"dcl_sampler")||!strcmp(n,"dcl_resource")||!strcmp(n,"dcl_input")||
           !strcmp(n,"dcl_input_ps")||!strcmp(n,"dcl_input_sgv")||!strcmp(n,"dcl_input_ps_sgv")||
           !strcmp(n,"dcl_input_ps_siv")||!strcmp(n,"dcl_input_siv")||!strcmp(n,"dcl_output")||
           !strcmp(n,"dcl_output_sgv")||!strcmp(n,"dcl_output_siv")||!strcmp(n,"dcl_temps")||
           !strcmp(n,"dcl_globalflags")||!strcmp(n,"dcl_indexable_temp")||!strcmp(n,"customdata")||!strcmp(n,"nop")) continue;
        const char*cmp = ins[x].test ? "!=" : "==";
        if(!strcmp(n,"ret")){ dg_lprintf(&L,"  return;"); continue; }
        if(!strcmp(n,"retc")){ dg_lprintf(&L,"  if((%s).x %s 0.0) return;",S(0),cmp); continue; }
        if(!strcmp(n,"if")){ dg_lprintf(&L,"  if((%s).x %s 0.0){",S(0),cmp); cf[cfn].kind=0; cf[cfn].first=0; cfn++; continue; }
        if(!strcmp(n,"else")){ if(cfn && cf[cfn-1].kind==0){ cf[cfn-1].kind=1; dg_lprintf(&L,"  }else{"); } continue; }
        if(!strcmp(n,"endif")){ if(cfn && (cf[cfn-1].kind==0||cf[cfn-1].kind==1)){ cfn--; dg_lprintf(&L,"  }"); } continue; }
        if(!strcmp(n,"loop")){ dg_lprintf(&L,"  for(int _li=0;_li<255;_li++){"); cf[cfn].kind=2; cfn++; continue; }
        if(!strcmp(n,"endloop")){ if(cfn && cf[cfn-1].kind==2){ cfn--; dg_lprintf(&L,"  }"); } continue; }
        if(!strcmp(n,"continue")){ int inl=0; for(int e=0;e<cfn;e++) if(cf[e].kind==2) inl=1; if(inl) dg_lprintf(&L,"  continue;"); continue; }
        if(!strcmp(n,"breakc")){ int inl=0; for(int e=0;e<cfn;e++) if(cf[e].kind==2) inl=1;
            if(inl) dg_lprintf(&L,"  if((%s).x %s 0.0) break;",S(0),cmp); else dg_lprintf(&L,"  // breakc (switch) omitted"); continue; }
        if(!strcmp(n,"break")){ int innerloop=-1; for(int e=cfn-1;e>=0;e--){ if(cf[e].kind==2){ innerloop=1; break; } if(cf[e].kind==3){ innerloop=0; break; } }
            if(innerloop==1) dg_lprintf(&L,"  break;"); else dg_lprintf(&L,"  // break (switch) -> if-chain"); continue; }
        if(!strcmp(n,"discard")){ dg_lprintf(&L,"  if((%s).x %s 0.0) discard;",S(0),cmp); continue; } // discard_z/_nz (alpha test)
        if(!strcmp(n,"switch")){ snprintf(cf[cfn].sv,sizeof cf[cfn].sv,"_sw%d",cfn); dg_lprintf(&L,"  { int %s=floatBitsToInt((%s).x);",cf[cfn].sv,S(0)); cf[cfn].kind=3; cf[cfn].first=0; cfn++; continue; }
        if(!strcmp(n,"case")){ int cv=(nopn && o[0].has_imm)?(int)o[0].imm[0]:0; if(cfn && cf[cfn-1].kind==3){ CF*st=&cf[cfn-1];
            dg_lprintf(&L,"  %sif(%s==%d){", st->first?"}else ":"", st->sv, cv); st->first=1; } continue; }
        if(!strcmp(n,"default")){ if(cfn && cf[cfn-1].kind==3){ CF*st=&cf[cfn-1]; dg_lprintf(&L,"  %s{", st->first?"}else ":""); st->first=1; } continue; }
        if(!strcmp(n,"endswitch")){ if(cfn && cf[cfn-1].kind==3){ cfn--; dg_lprintf(&L,"  } }"); } continue; }
        if(nopn==0){ dg_lprintf(&L,"  // %s (no operands)",n); continue; }
        // integer helpers
        #define SI(k) ({ char*_t=dg_pool(); snprintf(_t,256,"floatBitsToInt(%s)",S(k)); _t; })
        #define TX(k) ( (k)<nopn && o[k].nidx ? o[k].idx[0].val : 0 )
        #define RSW(k) ({ char*_r=dg_pool(); if((k)<nopn) dg_swz(&o[k],_r); else strcpy(_r,"xyzw"); _r; })
        char*line=NULL;
        if(!strcmp(n,"iadd")) line=dg_asg_i(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"%s + %s",SI(1),SI(2));t;}));
        else if(!strcmp(n,"imul")) line=dg_asg_i(&ctx,&o[0], nopn>3?({char*t=dg_pool();snprintf(t,256,"%s * %s",SI(2),SI(3));t;}):({char*t=dg_pool();snprintf(t,256,"%s*%s",SI(1),SI(2));t;}));
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
        else if(!strcmp(n,"ld_ms")||!strcmp(n,"ld")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"texelFetch(t%u,ivec2((%s).xy),0).%s",(unsigned)TX(2),S(1),RSW(2));t;}));
        else if(!strcmp(n,"sample_d")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"textureGrad(t%u,(%s).xy,(%s).xy,(%s).xy).%s",(unsigned)TX(2),S(1),S(3),S(4),RSW(2));t;}));
        else if(!strcmp(n,"resinfo")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(textureSize(t%u,0),0,0)",(unsigned)TX(2));t;}));
        else if(!strcmp(n,"mov")) line=dg_asg(&ctx,&o[0],S(1));
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
        else if(!strcmp(n,"ult")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(lessThan(uvec4(floatBitsToUint(%s)),uvec4(floatBitsToUint(%s))))",S(1),S(2));t;}));
        else if(!strcmp(n,"sample_b")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"texture(t%u,(%s).xy,(%s).x).%s",(unsigned)TX(2),S(1),S(4),RSW(2));t;}));
        else if(!strcmp(n,"sincos")){   // sincos dst_sin, dst_cos, src  (either destination may be null)
            if(o[0].type!=T_NULL) dg_lpush(&L,dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"sin(%s)",S(2));t;})));
            if(nopn>1 && o[1].type!=T_NULL) line=dg_asg(&ctx,&o[1],({char*t=dg_pool();snprintf(t,256,"cos(%s)",S(2));t;})); }
        else if(!strcmp(n,"dp2")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot((%s).xy,(%s).xy))",S(1),S(2));t;}));
        else if(!strcmp(n,"dp3")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot((%s).xyz,(%s).xyz))",S(1),S(2));t;}));
        else if(!strcmp(n,"dp4")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(dot(%s,%s))",S(1),S(2));t;}));
        else if(!strcmp(n,"movc")) line = (nopn>=4)?dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"mix(vec4(%s),vec4(%s),notEqual(vec4(%s),vec4(0.0)))",S(3),S(2),S(1));t;})):dg_asg(&ctx,&o[0],S(1));
        else if(!strcmp(n,"ne")||!strcmp(n,"ine")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(notEqual(vec4(%s),vec4(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"eq")||!strcmp(n,"ieq")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(equal(vec4(%s),vec4(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"lt")||!strcmp(n,"ilt")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(lessThan(vec4(%s),vec4(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"ge")||!strcmp(n,"ige")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(greaterThanEqual(vec4(%s),vec4(%s)))",S(1),S(2));t;}));
        else if(!strcmp(n,"sample")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"texture(t%u,(%s).xy).%s",(unsigned)TX(2),S(1),RSW(2));t;}));
        else if(!strcmp(n,"sample_l")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"textureLod(t%u,(%s).xy,(%s).x).%s",(unsigned)TX(2),S(1),S(4),RSW(2));t;}));
        else if(!strcmp(n,"sample_c")||!strcmp(n,"sample_c_lz")) line=dg_asg(&ctx,&o[0],({char*t=dg_pool();snprintf(t,256,"vec4(texture(t%u,(%s).xy).r >= (%s).x ?1.0:0.0)",(unsigned)TX(2),S(1), nopn>4?S(4):"vec4(0.5)");t;}));
        else dg_lprintf(&L,"  // UNTRANSLATED: %s",n);
        if(line) dg_lpush(&L,line);
    }
    #undef S
    #undef SI
    #undef TX
    #undef RSW
    // --- safety passes: declare any referenced-but-undeclared identifier, then balance braces ---
    char*body=dg_join(&L);
    int has_t[256]={0},has_cb[256]={0},has_v[256]={0},has_o[256]={0},has_r[256]={0};
    // what's already declared
    { // declared samplers "uniform sampler2D t%d", cbs "uniform vec4 cb%d", in v%d, out o%d, temps "vec4 r%d=vec4"
      for(int i=0;i<L.n;i++){ int d; const char*s=L.v[i];
        if(sscanf(s,"uniform sampler2D t%d;",&d)==1 && d<256) has_t[d]=1;
        if(sscanf(s,"uniform vec4 cb%d[",&d)==1 && d<256) has_cb[d]=1;
        if(sscanf(s,"in vec4 v%d;",&d)==1 && d<256) has_v[d]=1;
        if(sscanf(s,"out vec4 o%d;",&d)==1 && d<256) has_o[d]=1;
        if(sscanf(s,"  vec4 r%d=vec4",&d)==1 && d<256) has_r[d]=1;
      }
    }
    int ref_t[256]={0},ref_cb[256]={0},ref_v[256]={0},ref_o[256]={0},ref_r[256]={0};
    dg_scan_ids(body,"t",ref_t,256); dg_scan_ids(body,"cb",ref_cb,256);
    dg_scan_ids(body,"v",ref_v,256); dg_scan_ids(body,"o",ref_o,256); dg_scan_ids(body,"r",ref_r,256);
    int has_glpos = strstr(body,"gl_Position")!=NULL;
    // find insertion index (the 'void main(){' line)
    int mi=0; for(int i=0;i<L.n;i++) if(!strncmp(L.v[i],"void main",9)){ mi=i; break; }
    // header decls (t, cb, v, o) inserted before main, in that order, ascending — matches Python
    int ins_at=mi;
    for(int m=0;m<256;m++) if(ref_t[m]&&!has_t[m]){ char*s; asprintf(&s,"uniform sampler2D t%d;",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_cb[m]&&!has_cb[m]){ char*s; asprintf(&s,"uniform vec4 cb%d[4096];",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_v[m]&&!has_v[m]){ char*s; asprintf(&s,"in vec4 v%d;",m); dg_linsert(&L,ins_at++,s); mi++; }
    for(int m=0;m<256;m++) if(ref_o[m]&&!has_o[m]){ if(m!=0||!has_glpos){ char*s; asprintf(&s,"out vec4 o%d;",m); dg_linsert(&L,ins_at++,s); mi++; } }
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
