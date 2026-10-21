// win32x.c -- Win32 functions first needed by Project Gravemind (2026-10-20). Included by peload.c after winhttp.c (it uses
// wh_w2u / wh_u2w). Each was a logging stub returning 0:
//   CommandLineToArgvW  -- 0 = NULL argv: Gravemind read argv[1] through it at once (fault at 0x8, exe+0x42da65).
//   GetPrivateProfile* / WritePrivateProfileStringW -- its settings file h2gravemind.ini: every read came back empty, every
//                          write was lost.
//   CreateFile2          -- CreateFileW with the extended-parameters struct.
//   GetStringTypeW       -- character classes (CT_CTYPE1 / 2 / 3).
// PELOAD_WIN32X_STUB=1 = the old stubs for all of them.
static void* MSABI i_CreateFileW(const uint16_t*w,uint32_t acc,uint32_t share,void*sec,uint32_t disp,uint32_t fl,void*tmpl);   // (peload.c)
static int w32x_off(void){ static int o=-1; if(o<0){ const char*e=getenv("PELOAD_WIN32X_STUB"); o=e&&*e&&strcmp(e,"0"); } return o; }

// ---- CommandLineToArgvW: the documented rules. argv[0] = up to the next quote (when it starts with one) or the first blank,
// no escapes; later arguments: 2n backslashes + quote = n backslashes and a quote toggle, 2n+1 backslashes + quote = n
// backslashes and a literal quote, other backslashes are literal, "" inside quotes = a literal quote. Empty line = the exe path.
static int w32x_argv(const uint16_t*c,uint16_t**av,uint16_t*buf){   // buf NULL: count; returns argc, fills av/buf when given
    int argc=0; size_t k=0; const uint16_t*p=c;
    #define PUT(ch) do{ if(buf) buf[k]=(ch); k++; }while(0)
    if(av) av[0]=buf?buf+k:0;
    if(*p=='"'){ p++; while(*p && *p!='"') { PUT(*p); p++; }; if(*p=='"') p++; }
    else while(*p && *p!=' ' && *p!='\t') { PUT(*p); p++; };
    PUT(0); argc=1;
    for(;;){ while(*p==' '||*p=='\t') p++; if(!*p) break;
        if(av) av[argc]=buf?buf+k:0; int q=0;
        while(*p && (q || (*p!=' ' && *p!='\t'))){
            if(*p=='\\'){ int n=0; while(p[n]=='\\') n++;
                if(p[n]=='"'){ for(int i=0;i<n/2;i++) PUT('\\'); p+=n; if(n&1){ PUT('"'); p++; } }
                else { for(int i=0;i<n;i++) PUT('\\'); p+=n; } continue; }
            if(*p=='"'){ if(q && p[1]=='"'){ PUT('"'); p+=2; continue; } q=!q; p++; continue; }
            { PUT(*p); p++; }; }
        PUT(0); argc++; }
    #undef PUT
    return argc; }
static size_t w32x_argv_chars(const uint16_t*c){   // characters (NULs included) the arguments need
    size_t k=0; const uint16_t*p=c; int q=0;
    if(*p=='"'){ p++; while(*p && *p!='"'){ p++; k++; } if(*p) p++; } else while(*p && *p!=' ' && *p!='\t'){ p++; k++; }
    k++;
    for(;;){ while(*p==' '||*p=='\t') p++; if(!*p) break; q=0;
        while(*p && (q || (*p!=' ' && *p!='\t'))){
            if(*p=='\\'){ int n=0; while(p[n]=='\\') n++; if(p[n]=='"'){ k+=n/2; p+=n; if(n&1){ k++; p++; } } else { k+=n; p+=n; } continue; }
            if(*p=='"'){ if(q && p[1]=='"'){ k++; p+=2; continue; } q=!q; p++; continue; }
            k++; p++; }
        k++; }
    return k; }
static uint32_t MSABI i_GetModuleFileNameW(void*m,uint16_t*buf,uint32_t sz);   // (peload.c)
static uint16_t** MSABI i_CommandLineToArgvW(const uint16_t*cl,int*pn){
    if(w32x_off()) return 0;
    if(!pn){ i_SetLastError(87); return 0; }
    uint16_t exe[1100]; if(!cl || !*cl){ uint32_t n=i_GetModuleFileNameW(0,exe+1,1000); exe[0]='"'; exe[n+1]='"'; exe[n+2]=0; cl=exe; }
    int argc=w32x_argv(cl,0,0); size_t chars=w32x_argv_chars(cl);
    uint8_t*blk=malloc((size_t)(argc+1)*sizeof(uint16_t*)+chars*2+16); if(!blk){ i_SetLastError(8); return 0; }
    uint16_t**av=(uint16_t**)blk; uint16_t*buf=(uint16_t*)(blk+(size_t)(argc+1)*sizeof(uint16_t*));
    w32x_argv(cl,av,buf); av[argc]=0; *pn=argc; return av; }

// ---- INI files (GetPrivateProfile*): read whole, edit in memory, write back. Encoding kept: UTF-16LE with BOM, UTF-8 with
// BOM, or plain (read and written as UTF-8; a new file is plain). Names without a folder live in C:\Windows (as on Windows).
// Section and key names are case-insensitive; values are trimmed and lose one pair of surrounding quotes.
static int ini_path(const uint16_t*f,char*u,size_t cap){ if(!f || !*f) return 0; int hasdir=0; for(const uint16_t*p=f;*p;p++) if(*p=='\\'||*p=='/'||*p==':') hasdir=1;
    uint16_t w[1100]; size_t n=0; if(!hasdir){ const char*pre="C:\\Windows\\"; while(*pre) w[n++]=(uint8_t)*pre++; }
    for(const uint16_t*p=f;*p && n<1090;p++) w[n++]=*p; w[n]=0; return win2unix(w,u,(int)cap)!=0; }
static char* ini_load(const uint16_t*f,int*enc){   // malloc'd UTF-8 text ("" when missing); enc 0 plain, 1 UTF-8 BOM, 2 UTF-16LE
    *enc=0; char u[1400]; if(!ini_path(f,u,sizeof u)) return strdup("");
    FILE*fp=fopen(u,"rb"); if(!fp) return strdup(""); fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    if(n<0 || n>(16<<20)){ fclose(fp); return strdup(""); } uint8_t*d=malloc((size_t)n+2); size_t got=fread(d,1,(size_t)n,fp); fclose(fp); d[got]=0; d[got+1]=0;
    if(got>=2 && d[0]==0xFF && d[1]==0xFE){ *enc=2; size_t cap=got*2+8; char*o=malloc(cap); wh_w2u((uint16_t*)(d+2),(long)((got-2)/2),o,cap); free(d); return o; }
    if(got>=3 && d[0]==0xEF && d[1]==0xBB && d[2]==0xBF){ *enc=1; memmove(d,d+3,got-3+1); }
    return (char*)d; }
static int ini_save(const uint16_t*f,const char*t,int enc){ char u[1400]; if(!ini_path(f,u,sizeof u)) return 0; mkparents(u);
    FILE*fp=fopen(u,"wb"); if(!fp) return 0; size_t n=strlen(t);
    if(enc==2){ uint16_t*w=malloc((n+2)*2); size_t k=wh_u2w(t,n,w,n+2); fwrite("\xFF\xFE",1,2,fp); fwrite(w,2,k,fp); free(w); }
    else { if(enc==1) fwrite("\xEF\xBB\xBF",1,3,fp); fwrite(t,1,n,fp); }
    fclose(fp); return 1; }
static void ini_trim(const char**a,const char**b){ while(*a<*b && (**a==' '||**a=='\t')) (*a)++; while(*b>*a && ((*b)[-1]==' '||(*b)[-1]=='\t'||(*b)[-1]=='\r')) (*b)--; }
typedef struct { const char*ls,*le; int kind; const char*n0,*n1,*v0,*v1; } IniLine;   // kind 0 other, 1 [section], 2 key=value
static int ini_next(const char**p,IniLine*l){ if(!**p) return 0; const char*s=*p, *e=strchr(s,'\n'); if(!e) e=s+strlen(s);
    l->ls=s; l->le=e; *p=*e?e+1:e; const char*a=s,*b=e; ini_trim(&a,&b); l->kind=0;
    if(a<b && *a=='['){ const char*c=a+1; while(c<b && *c!=']') c++; l->n0=a+1; l->n1=c; ini_trim(&l->n0,&l->n1); l->kind=1; }
    else if(a<b && *a!=';' && *a!='#'){ const char*eq=memchr(a,'=',(size_t)(b-a)); l->n0=a; l->n1=eq?eq:b; ini_trim(&l->n0,&l->n1);
        if(eq){ l->v0=eq+1; l->v1=b; ini_trim(&l->v0,&l->v1); if(l->v1-l->v0>=2 && (*l->v0=='"'||*l->v0=='\'') && l->v1[-1]==*l->v0){ l->v0++; l->v1--; } }
        else { l->v0=l->v1=b; } l->kind=2; }
    return 1; }
static int ini_eq(const char*a0,const char*a1,const char*s){ size_t n=strlen(s); return (size_t)(a1-a0)==n && !strncasecmp(a0,s,n); }
// copy a UTF-8 list (items separated by \0, ends with an extra \0) into a wide buffer with Windows' truncation rules
static uint32_t ini_put_list(const char*lst,size_t ln,uint16_t*out,uint32_t sz){
    if(!out || sz<2){ if(out && sz) out[0]=0; return 0; }
    size_t k=0; uint16_t tmp[4096]; for(size_t i=0;i<ln;){ size_t sl=strlen(lst+i); size_t w=wh_u2w(lst+i,sl,tmp,4096);
        if(k+w+2>sz){ size_t room=sz>=2+k?sz-2-k:0; memcpy(out+k,tmp,room*2); k+=room; out[k]=0; out[k+1]=0; return sz-2; }
        memcpy(out+k,tmp,w*2); k+=w; out[k++]=0; i+=sl+1; }
    out[k]=0; return (uint32_t)k; }
static uint32_t MSABI i_GetPrivateProfileStringW(const uint16_t*app,const uint16_t*key,const uint16_t*def,uint16_t*out,uint32_t sz,const uint16_t*file){
    if(w32x_off()) return 0; if(!out || !sz) return 0;
    int enc; char*t=ini_load(file,&enc); char sa[512]="", sk[512]=""; if(app) wh_w2u(app,-1,sa,sizeof sa); if(key) wh_w2u(key,-1,sk,sizeof sk);
    const char*p=t; IniLine l; int in=0;
    if(!app || !key){   // a list: section names, or the keys of one section
        size_t cap=strlen(t)+2, n=0; char*lst=malloc(cap+2);
        while(ini_next(&p,&l)){ if(l.kind==1){ in=app?ini_eq(l.n0,l.n1,sa):1; if(!app){ size_t m=(size_t)(l.n1-l.n0); memcpy(lst+n,l.n0,m); n+=m; lst[n++]=0; } }
            else if(l.kind==2 && app && in){ size_t m=(size_t)(l.n1-l.n0); memcpy(lst+n,l.n0,m); n+=m; lst[n++]=0; } }
        uint32_t r=ini_put_list(lst,n,out,sz); free(lst); free(t); i_SetLastError(0); return r; }
    const char*v0=0,*v1=0;
    while(ini_next(&p,&l)){ if(l.kind==1) in=ini_eq(l.n0,l.n1,sa); else if(l.kind==2 && in && ini_eq(l.n0,l.n1,sk)){ v0=l.v0; v1=l.v1; break; } }
    char val[4096]; if(v0){ size_t m=(size_t)(v1-v0); if(m>=sizeof val) m=sizeof val-1; memcpy(val,v0,m); val[m]=0; i_SetLastError(0); }
    else { wh_w2u(def?def:(const uint16_t*)u"",-1,val,sizeof val); size_t m=strlen(val); while(m && val[m-1]==' ') val[--m]=0; i_SetLastError(2); }
    free(t);
    uint16_t w[4100]; size_t n=wh_u2w(val,strlen(val),w,4100); if(n>sz-1) n=sz-1; memcpy(out,w,n*2); out[n]=0; return (uint32_t)n; }
static uint32_t MSABI i_GetPrivateProfileIntW(const uint16_t*app,const uint16_t*key,int def,const uint16_t*file){
    if(w32x_off()) return 0; uint16_t b[64]; uint16_t dflt[]={'\x01',0};   // a default no number starts with
    i_GetPrivateProfileStringW(app,key,dflt,b,64,file); if(b[0]==1 && !b[1]) return (uint32_t)def;
    char a[64]; wh_w2u(b,-1,a,sizeof a); return (uint32_t)strtol(a,0,10); }
static uint32_t MSABI i_GetPrivateProfileSectionW(const uint16_t*app,uint16_t*out,uint32_t sz,const uint16_t*file){
    if(w32x_off()) return 0; int enc; char*t=ini_load(file,&enc); char sa[512]=""; wh_w2u(app,-1,sa,sizeof sa);
    size_t cap=strlen(t)+2, n=0; char*lst=malloc(cap+2); const char*p=t; IniLine l; int in=0;
    while(ini_next(&p,&l)){ if(l.kind==1) in=ini_eq(l.n0,l.n1,sa);
        else if(in && (l.kind==2)){ const char*a=l.ls,*b=l.le; ini_trim(&a,&b); size_t m=(size_t)(b-a); memcpy(lst+n,a,m); n+=m; lst[n++]=0; } }
    uint32_t r=ini_put_list(lst,n,out,sz); free(lst); free(t); return r; }
static uint32_t MSABI i_GetPrivateProfileSectionNamesW(uint16_t*out,uint32_t sz,const uint16_t*file){ return i_GetPrivateProfileStringW(0,0,0,out,sz,file); }
static int MSABI i_WritePrivateProfileStringW(const uint16_t*app,const uint16_t*key,const uint16_t*str,const uint16_t*file){
    if(w32x_off()) return 0; if(!app){ return 1; }   // (all NULL = flush: nothing is cached here)
    int enc; char*t=ini_load(file,&enc); char sa[512], sk[512]="", sv[4096]="";
    wh_w2u(app,-1,sa,sizeof sa); if(key) wh_w2u(key,-1,sk,sizeof sk); if(str) wh_w2u(str,-1,sv,sizeof sv);
    size_t tl=strlen(t), cap=tl+strlen(sa)+strlen(sk)+strlen(sv)+64; char*o=malloc(cap); size_t n=0;
    const char*p=t; IniLine l; int in=0, insec=0, done=0; const char*sec_end=0;
    #define EMIT(a,len) do{ memcpy(o+n,(a),(len)); n+=(len); }while(0)
    while(ini_next(&p,&l)){
        if(l.kind==1){ if(in && !done && key && str){ char kv[4700]; int k=snprintf(kv,sizeof kv,"%s=%s\r\n",sk,sv); EMIT(kv,(size_t)k); done=1; }
            in=ini_eq(l.n0,l.n1,sa); if(in) insec=1; }
        if(in && !key) continue;                                                      // deleting the whole section
        if(in && l.kind==2 && ini_eq(l.n0,l.n1,sk)){ if(!done && str){ char kv[4700]; int k=snprintf(kv,sizeof kv,"%s=%s\r\n",sk,sv); EMIT(kv,(size_t)k); } done=1; continue; }
        size_t ln=(size_t)(l.le-l.ls); EMIT(l.ls,ln); if(*l.le=='\n') o[n++]='\n'; (void)sec_end; }
    if(key && str && !done){ if(n && o[n-1]!='\n'){ o[n++]='\r'; o[n++]='\n'; }
        char kv[5300]; int k=insec&&in?snprintf(kv,sizeof kv,"%s=%s\r\n",sk,sv):snprintf(kv,sizeof kv,"[%s]\r\n%s=%s\r\n",sa,sk,sv); EMIT(kv,(size_t)k); }
    #undef EMIT
    o[n]=0; int ok=ini_save(file,o,enc); free(o); free(t); if(!ok) i_SetLastError(5); return ok; }

// ---- CreateFile2: CREATEFILE2_EXTENDED_PARAMETERS {size, attributes, flags, QoS, SECURITY_ATTRIBUTES* (+16), template (+24)}
static void* MSABI i_CreateFile2(const uint16_t*name,uint32_t acc,uint32_t share,uint32_t disp,const uint8_t*ex){
    if(w32x_off()) return 0;
    uint32_t fl=0; void*sa=0,*tmpl=0; if(ex){ fl=*(const uint32_t*)(ex+4)|*(const uint32_t*)(ex+8)|*(const uint32_t*)(ex+12); sa=*(void*const*)(ex+16); tmpl=*(void*const*)(ex+24); }
    return i_CreateFileW(name,acc,share,sa,disp,fl,tmpl); }

// ---- GetStringTypeW: CT_CTYPE1 (C1_* flags), CT_CTYPE2 (direction), CT_CTYPE3 (alpha / symbol). cch -1 = through the NUL.
#include <wctype.h>
static int MSABI i_GetStringTypeW(uint32_t type,const uint16_t*s,int n,uint16_t*out){
    if(w32x_off()) return 0; if(!s || !out){ i_SetLastError(87); return 0; } if(n<0){ n=0; while(s[n]) n++; n++; }
    for(int i=0;i<n;i++){ wint_t c=s[i]; uint16_t f=0;
        if(type==1){ if(iswupper(c)) f|=0x1; if(iswlower(c)) f|=0x2; if(iswdigit(c)) f|=0x4; if(iswspace(c)) f|=0x8; if(iswpunct(c)) f|=0x10;
            if(iswcntrl(c)) f|=0x20; if(c==' '||c=='\t') f|=0x40; if(iswxdigit(c)) f|=0x80; if(iswalpha(c)) f|=0x100; f|=0x200; }
        else if(type==2){ f=iswalpha(c)?1:iswdigit(c)?3:iswspace(c)?10:iswpunct(c)?11:0; }
        else if(type==4){ if(iswalpha(c)) f|=0x8000; if(iswpunct(c)) f|=0x8; }
        else { i_SetLastError(1004); return 0; }   // ERROR_INVALID_FLAGS
        out[i]=f; }
    return 1; }

// ---- Resources: FindResourceW / FindResourceExW / LoadResource / LockResource / SizeofResource over the module's own PE
// resource tree (Dir[2]: type -> name -> language directories, leaves = {data RVA, size, codepage}). A name or type below
// 0x10000 is an integer id, "#123" a string id, anything else a case-insensitive name. HRSRC = the leaf entry; the loaded
// "resource" is the data inside the mapped image (read-only, never freed, as on Windows). hModule NULL = the exe.
// Gravemind keeps its fonts and pictures in RCDATA; FindResourceW was a stub (NULL: "not found").
static const uint8_t* rsrc_find_entry(const uint8_t*root,const uint8_t*dir,const uint16_t*id){   // -> entry (8 bytes) or NULL
    uint16_t nn=*(const uint16_t*)(dir+12), ni=*(const uint16_t*)(dir+14); const uint8_t*e=dir+16;
    uintptr_t iv=(uintptr_t)id; uint32_t want=0; int byid=iv<0x10000;
    if(!byid && id[0]=='#'){ byid=1; for(const uint16_t*p=id+1;*p>='0'&&*p<='9';p++) want=want*10+(*p-'0'); } else if(byid) want=(uint32_t)iv;
    for(int i=0;i<nn+ni;i++,e+=8){ uint32_t nm=*(const uint32_t*)e;
        if(byid){ if(!(nm&0x80000000u) && (nm&0xFFFF)==want) return e; continue; }
        if(!(nm&0x80000000u)) continue; const uint16_t*s=(const uint16_t*)(root+(nm&0x7FFFFFFFu)); uint16_t len=s[0]; s++;
        int k=0; for(;k<len && id[k];k++){ uint16_t a=s[k], b=id[k]; if(a<128) a=(uint16_t)toupper(a); if(b<128) b=(uint16_t)toupper(b); if(a!=b) break; }
        if(k==len && !id[k]) return e; }
    return 0; }
static void* rsrc_find(void*mod,const uint16_t*type,const uint16_t*name,int lang){
    uint8_t*b=mod?(uint8_t*)mod:g_base; if(!b) return 0; DOS*dh=(DOS*)b; if(dh->e_magic!=0x5A4D) return 0;
    OPT64*oh=(OPT64*)(b+dh->e_lfanew+sizeof(COFF)); if(oh->NumDirs<3 || !oh->Dir[2].VA) return 0;
    const uint8_t*root=b+oh->Dir[2].VA;
    const uint8_t*e=rsrc_find_entry(root,root,type); if(!e || !(*(const uint32_t*)(e+4)&0x80000000u)) return 0;
    const uint8_t*d2=root+(*(const uint32_t*)(e+4)&0x7FFFFFFFu); e=rsrc_find_entry(root,d2,name); if(!e || !(*(const uint32_t*)(e+4)&0x80000000u)) return 0;
    const uint8_t*d3=root+(*(const uint32_t*)(e+4)&0x7FFFFFFFu); uint16_t n3=*(const uint16_t*)(d3+12)+*(const uint16_t*)(d3+14); if(!n3) return 0;
    const uint8_t*pick=d3+16; if(lang>=0) for(int i=0;i<n3;i++){ const uint8_t*x=d3+16+i*8; if((*(const uint32_t*)x&0xFFFF)==(uint32_t)lang){ pick=x; break; } }
    uint32_t off=*(const uint32_t*)(pick+4); if(off&0x80000000u) return 0; return (void*)(root+off); }   // IMAGE_RESOURCE_DATA_ENTRY
static void* MSABI i_FindResourceW(void*mod,const uint16_t*name,const uint16_t*type){ if(w32x_off()) return 0;
    void*r=rsrc_find(mod,type,name,-1); if(!r) i_SetLastError(1814); return r; }   // ERROR_RESOURCE_NAME_NOT_FOUND
static void* MSABI i_FindResourceExW(void*mod,const uint16_t*type,const uint16_t*name,uint16_t lang){ if(w32x_off()) return 0;
    void*r=rsrc_find(mod,type,name,lang?lang:-1); if(!r) i_SetLastError(1814); return r; }
static void* MSABI i_LoadResource(void*mod,void*h){ if(!h || w32x_off()) return 0; uint8_t*b=mod?(uint8_t*)mod:g_base; return b+*(const uint32_t*)h; }
static void* MSABI i_LockResource(void*h){ return h; }
static uint32_t MSABI i_SizeofResource(void*mod,void*h){ (void)mod; return h?*(const uint32_t*)((const uint8_t*)h+4):0; }
static int MSABI i_FreeResource(void*h){ (void)h; return 0; }

// ---- D3D11CreateDeviceAndSwapChain = D3D11CreateDevice + IDXGIFactory::CreateSwapChain (the loader's swap chain presents
// into its own window whatever the description says). It was a stub: S_OK with no device ("feature level 0x0") and
// Gravemind faulted on the NULL device (exe+0x234334).
static int MSABI i_D3D11CreateDeviceAndSwapChain(void*ad,int dt,void*sw,uint32_t fl,void*lv,uint32_t nlv,uint32_t sdk,void*desc,void**sc,void**dev,void*flv,void**ctx){
    void*d=0; int hr=i_D3D11CreateDevice(ad,dt,sw,fl,lv,nlv,sdk,&d,flv,ctx); if(hr<0) return hr;
    if(dev) *dev=d; if(sc) dxgi_CreateSwapChain(&g_dxgifactory,d,desc,sc); return 0; }
// ---- AdjustWindowRect: like the loader's AdjustWindowRectEx (the window's client area is the whole content view)
static int MSABI i_AdjustWindowRect(int32_t*r,uint32_t s,int m){ return i_AdjustWindowRectEx(r,s,m,0); }
// ---- AppPolicyGetThreadInitializationType (the CRT's thread start): AppPolicyThreadInitializationType_None
static int32_t MSABI i_AppPolicyGetThreadInitializationType(void*tok,int32_t*pol){ (void)tok; if(pol) *pol=0; return 0; }
// ---- BSTR (oleaut32 #2 SysAllocString, #4 SysAllocStringLen, #6 SysFreeString, #7 SysStringLen): a 4-byte byte length
// before the characters, NUL after them
static uint16_t* MSABI i_SysAllocStringLen(const uint16_t*s,uint32_t n){ uint8_t*b=malloc(4+(size_t)n*2+2); if(!b) return 0; *(uint32_t*)b=n*2;
    uint16_t*w=(uint16_t*)(b+4); if(s) memcpy(w,s,(size_t)n*2); else memset(w,0,(size_t)n*2); w[n]=0; return w; }
static uint16_t* MSABI i_SysAllocString(const uint16_t*s){ if(!s) return 0; uint32_t n=0; while(s[n]) n++; return i_SysAllocStringLen(s,n); }
static void MSABI i_SysFreeString(uint16_t*s){ if(s) free((uint8_t*)s-4); }
static uint32_t MSABI i_SysStringLen(const uint16_t*s){ return s?*(const uint32_t*)((const uint8_t*)s-4)/2:0; }
// ---- Not available here; they now fail cleanly instead of S_OK with nothing behind it:
//   Direct2D / DirectWrite (Gravemind's menus: "ui: Direct2D init failed 0x80004001"), Media Foundation's audio-format helpers
//   and MFTEnumEx (MFStartup already fails), winmm wave in / out (voice chat: no devices).
static int MSABI i_D2D1CreateFactory(int type,const void*iid,const void*opt,void**pp){ (void)type;(void)iid;(void)opt; if(pp) *pp=0; return (int)0x80004001; }
static int MSABI i_DWriteCreateFactory(int type,const void*iid,void**pp){ (void)type;(void)iid; if(pp) *pp=0; return (int)0x80004001; }
static int MSABI i_MFTEnumEx(const void*cat,uint32_t fl,const void*in,const void*out,void***act,uint32_t*n){ (void)cat;(void)fl;(void)in;(void)out; if(act) *act=0; if(n) *n=0; return (int)0x80004001; }
static int MSABI i_MFCreateWaveFormatExFromMFMediaType(void*mt,void**wf,uint32_t*sz,uint32_t fl){ (void)mt;(void)fl; if(wf) *wf=0; if(sz) *sz=0; return (int)0x80004001; }
static int MSABI i_MFInitMediaTypeFromWaveFormatEx(void*mt,const void*wf,uint32_t sz){ (void)mt;(void)wf;(void)sz; return (int)0x80004001; }
static uint32_t MSABI i_waveGetNumDevs(void){ return 0; }
static uint32_t MSABI i_waveOpen(void**h,uint32_t id,const void*fmt,uint64_t cb,uint64_t inst,uint32_t fl){ (void)id;(void)fmt;(void)cb;(void)inst;(void)fl; if(h) *h=0; return 6; }   // MMSYSERR_NODRIVER
static uint32_t MSABI i_waveNoDevice(void){ return 2; }   // MMSYSERR_BADDEVICEID (every other wave call: there is no device handle)
// ordinal-only imports, per DLL -> name (uni_resolve)
static const char* ord_name(const char*dll,unsigned ord){
    if(name_has(dll,"d2d1")) return ord==1?"D2D1CreateFactory":0;
    if(name_has(dll,"oleaut32")) return ord==2?"SysAllocString":ord==4?"SysAllocStringLen":ord==6?"SysFreeString":ord==7?"SysStringLen":0;
    if(name_has(dll,"xinput")) return ord==2?"XInputGetState":ord==3?"XInputSetState":ord==4?"XInputGetCapabilities":ord==5?"XInputEnable":0;
    return 0; }
// ---- K32GetModuleInformation / GetModuleInformation: MODULEINFO {base, SizeOfImage, entry} from the module's own PE header
// (hModule NULL = the exe). Gravemind finds halo2.dll's base this way before patching it; the stub left base 0 and it read the
// RVA 0x6a8f30 as an address (exe+0x2b8b5a).
static int MSABI i_K32GetModuleInformation(void*proc,void*mod,uint8_t*mi,uint32_t cb){ (void)proc;
    uint8_t*b=mod?(uint8_t*)mod:g_base; if(!mi || cb<24 || !b || ((DOS*)b)->e_magic!=0x5A4D){ i_SetLastError(87); return 0; }
    OPT64*oh=(OPT64*)(b+((DOS*)b)->e_lfanew+sizeof(COFF));
    *(uint64_t*)mi=(uint64_t)b; *(uint32_t*)(mi+8)=oh->ImageSize; *(uint64_t*)(mi+16)=oh->EntryRVA?(uint64_t)(b+oh->EntryRVA):0; return 1; }
static void* MSABI i_GetConsoleWindow(void){ return 0; }
static int MSABI i_AttachConsole(uint32_t pid){ (void)pid; i_SetLastError(6); return 0; }   // no parent console
// ---- halo2.dll's first needs (Gravemind, 2026-10-20): CreateSemaphoreExW (= CreateSemaphoreW: name / flags / access ignored,
// as there), CoInitialize (= CoInitializeEx apartment), SetThreadAffinityMask (scheduling is macOS's: report "all CPUs" as the
// old mask), shlwapi PathAppendW (join with one backslash, MAX_PATH result). The stubs returned NULL / FALSE / 0.
static void* MSABI i_CreateSemaphoreW(void*sec,long init,long max,const uint16_t*nm);   // (peload.c)
static void* MSABI i_CreateSemaphoreExW(void*sec,long init,long max,const uint16_t*nm,uint32_t fl,uint32_t acc){ (void)fl;(void)acc; return i_CreateSemaphoreW(sec,init,max,nm); }
static int MSABI i_CoInitialize(void*r){ (void)r; return 0; }
static uint64_t MSABI i_SetThreadAffinityMask(void*th,uint64_t m){ (void)th; return m?0xFFFFull:0; }
static int MSABI i_PathAppendW(uint16_t*p,const uint16_t*more){
    if(!p) return 0; if(!more) return 1; size_t n=0; while(p[n]) n++; while(*more=='\\') more++;
    size_t m=0; while(more[m]) m++; if(n+1+m+1>260) return 0;
    if(n && p[n-1]!='\\' && m) p[n++]='\\'; for(size_t i=0;i<=m;i++) p[n+i]=more[i]; return 1; }
// ---- Registry, ANSI side + enumeration (halo2.dll / MCC's DLLs, 2026-10-20). The loader's registry answers Steam's install
// path and "not found" for everything else (i_RegGetValueW). RegOpenKeyExA / RegQueryValueExA / RegGetValueA now go through
// the W versions (REG_SZ answers converted back to ANSI); RegEnumKeyEx(A/W) / RegEnumValue(A/W) say ERROR_NO_MORE_ITEMS (259)
// -- as logging stubs they returned 0 = ERROR_SUCCESS forever and halo2's engine thread enumerated keys in an endless loop.
static long MSABI i_RegOpenKeyExW(void*hk,const uint16_t*sub,uint32_t o,uint32_t sam,void**out);                       // (peload.c)
static long MSABI i_RegGetValueW(void*hk,const uint16_t*sub,const uint16_t*val,uint32_t fl,uint32_t*ty,void*data,uint32_t*cb);
static void reg_a2w(const char*a,uint16_t*w,size_t cap){ size_t i=0; if(a) for(;a[i] && i+1<cap;i++) w[i]=(uint8_t)a[i]; w[i]=0; }
static long MSABI i_RegOpenKeyExA(void*hk,const char*sub,uint32_t o,uint32_t sam,void**out){ uint16_t w[512]; reg_a2w(sub,w,512); return i_RegOpenKeyExW(hk,sub?w:0,o,sam,out); }
static long MSABI i_RegGetValueA(void*hk,const char*sub,const char*val,uint32_t fl,uint32_t*ty,void*data,uint32_t*cb){
    uint16_t ws_[512], wv[256]; reg_a2w(sub,ws_,512); reg_a2w(val,wv,256); uint32_t t=0, wcb=0;
    long r=i_RegGetValueW(hk,sub?ws_:0,val?wv:0,fl,&t,0,&wcb); if(r) return r;
    uint16_t*tmp=malloc(wcb+2); uint32_t c2=wcb; r=i_RegGetValueW(hk,sub?ws_:0,val?wv:0,fl,&t,tmp,&c2); if(r){ free(tmp); return r; }
    if(ty) *ty=t; uint32_t n=c2/2;   // REG_SZ in characters, NUL included
    if(!data){ if(cb) *cb=n; free(tmp); return 0; } if(cb && *cb<n){ *cb=n; free(tmp); return 234; }
    for(uint32_t i=0;i<n;i++) ((char*)data)[i]=(char)(tmp[i]<256?tmp[i]:'?'); if(cb) *cb=n; free(tmp); return 0; }
static long MSABI i_RegQueryValueExA(void*hk,const char*val,void*r,uint32_t*ty,void*data,uint32_t*cb){ (void)r; return i_RegGetValueA(hk,0,val,0,ty,data,cb); }
static long MSABI i_RegEnumNone(void){ return 259; }   // ERROR_NO_MORE_ITEMS

// ---- msvcrt.dll names the guest UCRT / VC runtime DLLs don't export (2026-10-20; MCC's real D3DCompiler_47.dll, loaded as a
// guest DLL for runtime HLSL, imports 115 msvcrt names: these 14 were missing and became logging stubs).
//   printf family -> the guest ucrtbase's __stdio_common_* (they take Windows va_lists), with the legacy-msvcrt option bits
//   (null termination on truncation, legacy wide specifiers, msvcrt compatibility); variadic ones via __builtin_ms_va_list.
//   _lock / _unlock: recursive mutexes; __dllonexit / _onexit: accepted (DLLs are never unloaded here); _XcptFilter: continue
//   search; _amsg_exit / std::terminate: abort with a message; type_info's destructor: nothing to free.
static void *export_lookup(uint8_t *base, const char *want);   // (peload.c)
static void* crt_fn(const char*n){ for(int m=0;m<g_nmods;m++) if(g_mods[m].name && !strcasecmp(g_mods[m].name,"ucrtbase.dll")) return export_lookup(g_mods[m].base,n); return 0; }
#define CRT_LEGACY 0x0Dull   // _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION | _LEGACY_WIDE_SPECIFIERS | _LEGACY_MSVCRT_COMPATIBILITY
typedef int (MSABI *crt_vsp_t)(uint64_t,char*,size_t,const char*,void*,void*);
typedef int (MSABI *crt_vswp_t)(uint64_t,uint16_t*,size_t,const uint16_t*,void*,void*);
typedef int (MSABI *crt_vsnwp_t)(uint64_t,uint16_t*,size_t,size_t,const uint16_t*,void*,void*);
typedef int (MSABI *crt_vss_t)(uint64_t,const char*,size_t,const char*,void*,void*);
static int MSABI i_ms_vsnprintf(char*b,size_t n,const char*f,void*va){ static crt_vsp_t fn; if(!fn) fn=(crt_vsp_t)crt_fn("__stdio_common_vsprintf"); if(!fn) return -1;
    int r=fn(CRT_LEGACY,b,n,f,0,va); return r<0?-1:r; }
static int MSABI i_ms_vsnwprintf(uint16_t*b,size_t n,const uint16_t*f,void*va){ static crt_vswp_t fn; if(!fn) fn=(crt_vswp_t)crt_fn("__stdio_common_vswprintf"); if(!fn) return -1;
    int r=fn(CRT_LEGACY,b,n,f,0,va); return r<0?-1:r; }
static int MSABI i_ms_sprintf_s(char*b,size_t n,const char*f,...){ static crt_vsp_t fn; if(!fn) fn=(crt_vsp_t)crt_fn("__stdio_common_vsprintf_s"); if(!fn) return -1;
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap,f); int r=fn(0,b,n,f,0,(void*)ap); __builtin_ms_va_end(ap); return r<0?-1:r; }
static int MSABI i_ms_swprintf_s(uint16_t*b,size_t n,const uint16_t*f,...){ static crt_vswp_t fn; if(!fn) fn=(crt_vswp_t)crt_fn("__stdio_common_vswprintf_s"); if(!fn) return -1;
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap,f); int r=fn(0,b,n,f,0,(void*)ap); __builtin_ms_va_end(ap); return r<0?-1:r; }
static int MSABI i_ms_snwprintf_s(uint16_t*b,size_t n,size_t cnt,const uint16_t*f,...){ static crt_vsnwp_t fn; if(!fn) fn=(crt_vsnwp_t)crt_fn("__stdio_common_vsnwprintf_s"); if(!fn) return -1;
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap,f); int r=fn(0,b,n,cnt,f,0,(void*)ap); __builtin_ms_va_end(ap); return r<0?-1:r; }
static int MSABI i_ms_sscanf_s(const char*s,const char*f,...){ static crt_vss_t fn; if(!fn) fn=(crt_vss_t)crt_fn("__stdio_common_vsscanf"); if(!fn) return -1;
    __builtin_ms_va_list ap; __builtin_ms_va_start(ap,f); int r=fn(1/*_CRT_INTERNAL_SCANF_SECURECRT*/,s,(size_t)-1,f,0,(void*)ap); __builtin_ms_va_end(ap); return r; }
static pthread_mutex_t g_ms_locks[64];
static void ms_locks_init(void){ pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE); for(int i=0;i<64;i++) pthread_mutex_init(&g_ms_locks[i],&a); }
static void MSABI i_ms_lock(int n){ static pthread_once_t o=PTHREAD_ONCE_INIT; pthread_once(&o,ms_locks_init); pthread_mutex_lock(&g_ms_locks[(unsigned)n&63]); }
static void MSABI i_ms_unlock(int n){ static pthread_once_t o=PTHREAD_ONCE_INIT; pthread_once(&o,ms_locks_init); pthread_mutex_unlock(&g_ms_locks[(unsigned)n&63]); }
static void* MSABI i_ms_dllonexit(void*fn,void*b,void*e){ (void)b;(void)e; return fn; }
static void* MSABI i_ms_onexit(void*fn){ return fn; }
static int MSABI i_ms_XcptFilter(uint32_t code,void*ptrs){ (void)code;(void)ptrs; return 0; }   // EXCEPTION_CONTINUE_SEARCH
static void MSABI i_ms_amsg_exit(int n){ char m[80]; int k=snprintf(m,sizeof m,"  [msvcrt] _amsg_exit(%d): runtime error, aborting\n",n); (void)!write(2,m,k); abort(); }
static void MSABI i_ms_terminate(void){ (void)!write(2,"  [msvcrt] std::terminate called\n",33); abort(); }
static void MSABI i_ms_typeinfo_dtor(void*t){ (void)t; }

// ---- halo2.dll's next calls (2026-10-20). Each was a logging stub; the dangerous one was SHGetFolderPathW: S_OK with the path
// buffer left as it was.
//   SHGetFolderPathW(A): CSIDL -> the loader's home folders (as SHGetKnownFolderPath); CSIDL_FLAG_CREATE (0x8000) makes it.
//   GetFileSize, GetFullPathNameA, GetEnvironmentVariableA: through the existing Ex / W versions.
//   EnumDisplaySettingsA: the W answer in DEVMODEA's layout (names 32 chars, not 32 wide chars).
//   GetDesktopWindow: a fixed handle whose GetWindowRect is the reported monitor (MON_W x MON_H).
//   LoadIconA / LoadCursorA = the W versions; LocaleNameToLCID: en-US 0x409 for any name; HeapDestroy: TRUE (heaps are malloc);
//   GetAdaptersAddresses: ERROR_NO_DATA (no adapter list yet: Gravemind's LAN game list stays empty).
static uint32_t MSABI i_GetFullPathNameW(const uint16_t*name,uint32_t len,uint16_t*buf,uint16_t**part);   // (peload.c)
static uint32_t MSABI i_GetEnvironmentVariableW(const uint16_t*n,uint16_t*v,uint32_t sz);
static int MSABI i_EnumDisplaySettingsW(const uint16_t*dev,uint32_t mode,uint8_t*dm);
static int MSABI i_GetFileSizeEx(void*h,int64_t*sz);
static void* MSABI i_LoadIconW(void*h,const void*n); static void* MSABI i_LoadCursorW(void*h,const void*n);
static int MSABI i_SHGetFolderPathW(void*hw,int csidl,void*tok,uint32_t fl,uint16_t*out){ (void)hw;(void)fl;
    if(!out) return (int)0x80070057; out[0]=0;
    static const struct { int id; uint32_t kf; } map[]={ {0x05,0xFDD39AD0},{0x1A,0x3EB685DB},{0x1C,0xF1B32785},{0x10,0xB4BFCC3A},{0x00,0xB4BFCC3A},
        {0x28,0x5E6C858F},{0x0E,0x18989B1D},{0x27,0x33E28130},{0x0D,0x4BD8D571},{0x26,0x905E63B6},{0x2A,0x7C5A40EF},{0x23,0x62AB5D82} };
    int c=csidl&0xFF; uint16_t*p=0; GUID_ g={0};
    if(c==0x24 || c==0x25){ const char*s=c==0x24?"C:\\Windows":"C:\\Windows\\System32"; size_t i=0; for(;s[i];i++) out[i]=(uint8_t)s[i]; out[i]=0; return 0; }
    for(size_t i=0;i<sizeof map/sizeof map[0];i++) if(map[i].id==c){ g.D1=map[i].kf; break; }
    if(!g.D1) return (int)0x80004005;
    if(i_SHGetKnownFolderPath(&g,(csidl&0x8000)?0x8000:0,tok,&p)!=0 || !p) return (int)0x80004005;
    size_t n=0; while(p[n] && n<259){ out[n]=p[n]; n++; } out[n]=0; free(p); return 0; }
static int MSABI i_SHGetFolderPathA(void*hw,int csidl,void*tok,uint32_t fl,char*out){ uint16_t w[262]; int r=i_SHGetFolderPathW(hw,csidl,tok,fl,w);
    if(out){ size_t i=0; for(;w[i] && i<259;i++) out[i]=(char)(w[i]<256?w[i]:'?'); out[i]=0; } return r; }
static uint32_t MSABI i_GetFileSize(void*h,uint32_t*hi){ int64_t s=0; if(!i_GetFileSizeEx(h,&s)){ if(hi) *hi=0; return 0xFFFFFFFFu; } if(hi) *hi=(uint32_t)(s>>32); return (uint32_t)s; }
static uint32_t MSABI i_GetFullPathNameA(const char*name,uint32_t len,char*buf,char**part){
    uint16_t wn[1100], wb[1100]; size_t i=0; for(;name && name[i] && i<1099;i++) wn[i]=(uint8_t)name[i]; wn[i]=0;
    uint32_t r=i_GetFullPathNameW(wn,1100,wb,0); if(!r || r>=1100) return r; if(len<r+1) return r+1;
    for(uint32_t k=0;k<=r;k++) buf[k]=(char)(wb[k]<256?wb[k]:'?');
    if(part){ char*l=0; for(char*q=buf;*q;q++) if(*q=='\\'||*q=='/') l=q+1; *part=l&&*l?l:0; } return r; }
static uint32_t MSABI i_GetEnvironmentVariableA(const char*n,char*v,uint32_t sz){
    uint16_t wn[300], wv[4100]; size_t i=0; for(;n && n[i] && i<299;i++) wn[i]=(uint8_t)n[i]; wn[i]=0;
    uint32_t r=i_GetEnvironmentVariableW(wn,wv,4100); if(!r || r>=4100) return r; if(!v || sz<r+1) return r+1;
    for(uint32_t k=0;k<=r;k++) v[k]=(char)(wv[k]<256?wv[k]:'?'); return r; }
static int MSABI i_EnumDisplaySettingsA(const char*dev,uint32_t mode,uint8_t*dm){ (void)dev; if(!dm) return 0;
    uint8_t w[220]; memset(w,0,sizeof w); *(uint16_t*)(w+68)=220; if(!i_EnumDisplaySettingsW(0,mode,w)) return 0;
    uint16_t sz=*(uint16_t*)(dm+36); (void)sz; memset(dm,0,156);
    for(int i=0;i<31 && *(uint16_t*)(w+i*2);i++) dm[i]=(uint8_t)*(uint16_t*)(w+i*2);
    memcpy(dm+32,w+64,38); for(int i=0;i<31 && *(uint16_t*)(w+102+i*2);i++) dm[70+i]=(uint8_t)*(uint16_t*)(w+102+i*2);
    memcpy(dm+102,w+166,54); *(uint16_t*)(dm+36)=156; return 1; }
#define DESKTOP_HWND ((void*)0xD35C70)
static void* MSABI i_GetDesktopWindow(void){ return DESKTOP_HWND; }
static void* MSABI i_LoadIconA(void*h,const void*n){ return i_LoadIconW(h,n); }
static void* MSABI i_LoadCursorA(void*h,const void*n){ return i_LoadCursorW(h,n); }
static uint32_t MSABI i_LocaleNameToLCID(const uint16_t*n,uint32_t fl){ (void)n;(void)fl; return 0x409; }
static int MSABI i_HeapDestroy(void*h){ (void)h; return 1; }
static uint32_t MSABI i_GetAdaptersAddresses(uint32_t fam,uint32_t fl,void*res,void*buf,uint32_t*sz){ (void)fam;(void)fl;(void)res;(void)buf; if(sz) *sz=0; return 232; }   // ERROR_NO_DATA
// ---- ResumeThread / SuspendThread (2026-10-20): the suspend count of a thread made with CREATE_SUSPENDED (i_CreateThread /
// thread_tramp keep it in the thread object's count). ResumeThread returns the previous count and wakes the thread at 0;
// SuspendThread of a running thread is not possible here (-1, as for a thread that cannot be suspended).
static int is_sync(intptr_t h);   // (peload.c)
static uint32_t MSABI i_ResumeThread(void*h){ if(!is_sync((intptr_t)h)) return 0; SyncObj*o=h; if(o->kind!=K_THREAD) return (uint32_t)-1;
    pthread_mutex_lock(&o->m); long prev=o->count; if(o->count>0) o->count--; if(!o->count) pthread_cond_broadcast(&o->c); pthread_mutex_unlock(&o->m); return (uint32_t)prev; }
static uint32_t MSABI i_SuspendThread(void*h){ (void)h; i_SetLastError(5); return (uint32_t)-1; }
static int MSABI i_GetThreadPriority(void*h){ (void)h; return 0; }   // THREAD_PRIORITY_NORMAL
// ---- GetLogicalProcessorInformation: one RelationProcessorCore entry per logical CPU (hw.logicalcpu) + one package
static int MSABI i_GetLogicalProcessorInformation(uint8_t*buf,uint32_t*len){ if(!len){ i_SetLastError(87); return 0; }
    int lc=8; size_t sz=sizeof lc; sysctlbyname("hw.logicalcpu",&lc,&sz,0,0); if(lc<1) lc=1; if(lc>64) lc=64;
    uint32_t need=(uint32_t)(lc+1)*32; if(!buf || *len<need){ *len=need; i_SetLastError(122); return 0; }
    memset(buf,0,need); for(int i=0;i<lc;i++){ *(uint64_t*)(buf+i*32)=1ull<<i; *(int32_t*)(buf+i*32+8)=0; }
    *(uint64_t*)(buf+lc*32)=(lc>=64)?~0ull:((1ull<<lc)-1); *(int32_t*)(buf+lc*32+8)=3;   // RelationProcessorPackage
    *len=need; return 1; }
