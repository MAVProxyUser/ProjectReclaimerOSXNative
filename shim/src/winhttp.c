// winhttp.c -- WinHTTP (synchronous mode) on macOS's libcurl (2026-10-20, Project Gravemind: the beta status file
// raw.githubusercontent.com/BriRoz1/h2gravemind-beta/main/beta.txt, the GitHub release check, api.ipify.org). Included by
// peload.c; build.sh links -lcurl (the system library: TLS with the macOS trust store).
//   WinHttpOpen / SetTimeouts / Connect / OpenRequest -> handles (malloc'd, magic-checked); SendRequest keeps the headers
//   and the body; ReceiveResponse runs the whole transfer (curl_easy_perform: redirects followed, as WinHTTP's default
//   policy does for same-or-safer schemes); QueryHeaders answers the status code / text, Content-Length / -Type, Location,
//   raw headers and custom names (string or WINHTTP_QUERY_FLAG_NUMBER); ReadData copies the stored body out; CrackUrl
//   splits a URL into URL_COMPONENTS. No async mode (Gravemind never sets a status callback).
// Before: every WinHttp* was a logging stub; WinHttpOpen returned NULL, so Gravemind logged "beta: status file not
// reachable" and refused to start. PELOAD_WINHTTP_STUB=1 = that old behaviour; PELOAD_TRACE_WINHTTP=1 logs each request.
#include <curl/curl.h>

#define WH_MAGIC 0x57486e64u
enum { WH_SESSION=1, WH_CONNECT=2, WH_REQUEST=3 };
typedef struct WhH {
    uint32_t magic, kind;
    struct WhH*parent;
    char agent[256];                       // session
    int t_resolve, t_connect, t_send, t_recv;   // session timeouts (ms; 0 = none)
    char host[512]; int port;              // connect
    char verb[32], *path; int secure;      // request
    struct curl_slist*hdrs; uint8_t*body; size_t nbody;
    long status; char*statustext;          // response
    char*rhdr; size_t nrhdr, caprhdr;      // raw response headers (last response after redirects), "\r\n" lines
    uint8_t*resp; size_t nresp, capresp, rd;
    int done;
} WhH;

static int wh_trace(void){ static int t=-1; if(t<0){ const char*e=getenv("PELOAD_TRACE_WINHTTP"); t=e&&*e&&strcmp(e,"0"); } return t; }
static WhH* wh_get(void*h,int kind){ WhH*w=(WhH*)h; if(!w || w->magic!=WH_MAGIC || (kind && w->kind!=kind)){ i_SetLastError(6); return 0; } return w; }
// UTF-16 <-> UTF-8 (surrogate pairs included)
static size_t wh_w2u(const uint16_t*w,long n,char*o,size_t cap){ size_t k=0; if(!w){ if(cap) o[0]=0; return 0; }
    for(long i=0;(n<0?w[i]!=0:i<n) && k+4<cap;i++){ uint32_t c=w[i];
        if(c>=0xD800 && c<0xDC00 && (n<0||i+1<n) && w[i+1]>=0xDC00 && w[i+1]<0xE000){ c=0x10000+((c-0xD800)<<10)+(w[i+1]-0xDC00); i++; }
        if(c<0x80) o[k++]=(char)c; else if(c<0x800){ o[k++]=(char)(0xC0|c>>6); o[k++]=(char)(0x80|(c&63)); }
        else if(c<0x10000){ o[k++]=(char)(0xE0|c>>12); o[k++]=(char)(0x80|((c>>6)&63)); o[k++]=(char)(0x80|(c&63)); }
        else { o[k++]=(char)(0xF0|c>>18); o[k++]=(char)(0x80|((c>>12)&63)); o[k++]=(char)(0x80|((c>>6)&63)); o[k++]=(char)(0x80|(c&63)); } }
    if(cap) o[k]=0; return k; }
static size_t wh_u2w(const char*s,size_t n,uint16_t*o,size_t cap){   // cap in units, NUL included
    size_t k=0,i=0; const uint8_t*u=(const uint8_t*)s;
    while(i<n && k+2<cap){ uint32_t c=u[i++]; int more=0;
        if(c>=0xF0){ c&=7; more=3; } else if(c>=0xE0){ c&=15; more=2; } else if(c>=0xC0){ c&=31; more=1; }
        while(more-- > 0 && i<n) c=(c<<6)|(u[i++]&63);
        if(c>=0x10000){ c-=0x10000; o[k++]=(uint16_t)(0xD800+(c>>10)); o[k++]=(uint16_t)(0xDC00+(c&1023)); }
        else o[k++]=(uint16_t)c; }
    if(cap) o[k]=0; return k; }
static void wh_curl_init(void){ curl_global_init(CURL_GLOBAL_DEFAULT); }

static void* MSABI i_WinHttpOpen(const uint16_t*agent,uint32_t access,const uint16_t*proxy,const uint16_t*bypass,uint32_t fl){ (void)access;(void)proxy;(void)bypass;
    if(getenv("PELOAD_WINHTTP_STUB")) return 0;
    if(fl&0x10000000){ i_SetLastError(87); return 0; }   // WINHTTP_FLAG_ASYNC: not supported here
    static pthread_once_t once=PTHREAD_ONCE_INIT; pthread_once(&once,wh_curl_init);
    WhH*s=calloc(1,sizeof *s); s->magic=WH_MAGIC; s->kind=WH_SESSION; wh_w2u(agent,-1,s->agent,sizeof s->agent);
    s->t_resolve=0; s->t_connect=60000; s->t_send=30000; s->t_recv=30000; return s; }
static int MSABI i_WinHttpSetTimeouts(void*h,int rs,int cn,int sd,int rc){ WhH*s=wh_get(h,0); if(!s) return 0;
    while(s->kind!=WH_SESSION && s->parent) s=s->parent; s->t_resolve=rs; s->t_connect=cn; s->t_send=sd; s->t_recv=rc; return 1; }
static void* MSABI i_WinHttpConnect(void*hs,const uint16_t*host,uint16_t port,uint32_t res){ (void)res; WhH*s=wh_get(hs,WH_SESSION); if(!s) return 0;
    if(!host || !*host){ i_SetLastError(12005); return 0; }   // ERROR_WINHTTP_INVALID_URL
    WhH*c=calloc(1,sizeof *c); c->magic=WH_MAGIC; c->kind=WH_CONNECT; c->parent=s; wh_w2u(host,-1,c->host,sizeof c->host); c->port=port; return c; }
static void* MSABI i_WinHttpOpenRequest(void*hc,const uint16_t*verb,const uint16_t*obj,const uint16_t*ver,const uint16_t*ref,const uint16_t**accept,uint32_t fl){
    (void)ver;(void)ref;(void)accept; WhH*c=wh_get(hc,WH_CONNECT); if(!c) return 0;
    WhH*r=calloc(1,sizeof *r); r->magic=WH_MAGIC; r->kind=WH_REQUEST; r->parent=c;
    if(verb && *verb) wh_w2u(verb,-1,r->verb,sizeof r->verb); else strcpy(r->verb,"GET");
    char p[4096]; wh_w2u(obj?obj:(const uint16_t*)u"/",-1,p,sizeof p); r->path=strdup(p[0]?p:"/");
    r->secure=(fl&0x00800000)!=0;   // WINHTTP_FLAG_SECURE
    return r; }
static int MSABI i_WinHttpAddRequestHeaders(void*hr,const uint16_t*h,uint32_t n,uint32_t mod);
static int MSABI i_WinHttpSendRequest(void*hr,const uint16_t*hdr,uint32_t nhdr,void*opt,uint32_t nopt,uint32_t total,uint64_t ctx){ (void)total;(void)ctx;
    WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0;
    if(hdr && nhdr) i_WinHttpAddRequestHeaders(hr,hdr,nhdr,0);
    free(r->body); r->body=0; r->nbody=0; if(opt && nopt){ r->body=malloc(nopt); memcpy(r->body,opt,nopt); r->nbody=nopt; }
    return 1; }
static int MSABI i_WinHttpAddRequestHeaders(void*hr,const uint16_t*h,uint32_t n,uint32_t mod){ (void)mod; WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0;
    if(!h) return 1; size_t cap=(n==0xFFFFFFFFu?8192:(size_t)n*4+8); char*u=malloc(cap); wh_w2u(h,n==0xFFFFFFFFu?-1:(long)n,u,cap);
    for(char*l=u;*l;){ char*e=strstr(l,"\r\n"); size_t len=e?(size_t)(e-l):strlen(l); if(len){ char t=l[len]; l[len]=0; r->hdrs=curl_slist_append(r->hdrs,l); l[len]=t; }
        l+=len; if(e) l+=2; }
    free(u); return 1; }
static size_t wh_on_body(char*p,size_t sz,size_t n,void*u){ WhH*r=u; size_t k=sz*n;
    if(r->nresp+k>r->capresp){ size_t c=r->capresp?r->capresp:65536; while(c<r->nresp+k) c*=2; uint8_t*q=realloc(r->resp,c); if(!q) return 0; r->resp=q; r->capresp=c; }
    memcpy(r->resp+r->nresp,p,k); r->nresp+=k; return k; }
static size_t wh_on_hdr(char*p,size_t sz,size_t n,void*u){ WhH*r=u; size_t k=sz*n;
    if(k>=5 && !strncmp(p,"HTTP/",5)){ r->nrhdr=0; free(r->statustext); r->statustext=0;   // a new response (after a redirect): keep only the last
        const char*sp=memchr(p,' ',k); if(sp){ sp=memchr(sp+1,' ',(size_t)(p+k-sp-1)); if(sp){ size_t tl=(size_t)(p+k-sp-1); while(tl && (sp[tl]=='\r'||sp[tl]=='\n'||sp[tl]==0)) tl--; r->statustext=strndup(sp+1,tl); } } }
    if(r->nrhdr+k+1>r->caprhdr){ size_t c=r->caprhdr?r->caprhdr:4096; while(c<r->nrhdr+k+1) c*=2; char*q=realloc(r->rhdr,c); if(!q) return 0; r->rhdr=q; r->caprhdr=c; }
    memcpy(r->rhdr+r->nrhdr,p,k); r->nrhdr+=k; r->rhdr[r->nrhdr]=0; return k; }
static int MSABI i_WinHttpReceiveResponse(void*hr,void*res){ (void)res; WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0; if(r->done) return 1;
    WhH*c=r->parent, *s=c->parent; char url[5000];
    int defport=r->secure?443:80; if(c->port && c->port!=defport) snprintf(url,sizeof url,"%s://%s:%d%s",r->secure?"https":"http",c->host,c->port,r->path);
    else snprintf(url,sizeof url,"%s://%s%s",r->secure?"https":"http",c->host,r->path);
    CURL*e=curl_easy_init(); if(!e){ i_SetLastError(8); return 0; }
    curl_easy_setopt(e,CURLOPT_URL,url); curl_easy_setopt(e,CURLOPT_FOLLOWLOCATION,1L); curl_easy_setopt(e,CURLOPT_MAXREDIRS,10L);
    curl_easy_setopt(e,CURLOPT_REDIR_PROTOCOLS_STR,r->secure?"https":"http,https");
    if(s->agent[0]) curl_easy_setopt(e,CURLOPT_USERAGENT,s->agent);
    if(s->t_connect>0) curl_easy_setopt(e,CURLOPT_CONNECTTIMEOUT_MS,(long)s->t_connect);
    if(s->t_recv>0) { curl_easy_setopt(e,CURLOPT_LOW_SPEED_LIMIT,1L); curl_easy_setopt(e,CURLOPT_LOW_SPEED_TIME,(long)((s->t_recv+999)/1000)); }
    if(strcmp(r->verb,"GET")) curl_easy_setopt(e,CURLOPT_CUSTOMREQUEST,r->verb);
    if(!strcmp(r->verb,"HEAD")) curl_easy_setopt(e,CURLOPT_NOBODY,1L);
    if(r->body){ curl_easy_setopt(e,CURLOPT_POSTFIELDS,r->body); curl_easy_setopt(e,CURLOPT_POSTFIELDSIZE,(long)r->nbody); }
    if(r->hdrs) curl_easy_setopt(e,CURLOPT_HTTPHEADER,r->hdrs);
    curl_easy_setopt(e,CURLOPT_WRITEFUNCTION,wh_on_body); curl_easy_setopt(e,CURLOPT_WRITEDATA,r);
    curl_easy_setopt(e,CURLOPT_HEADERFUNCTION,wh_on_hdr); curl_easy_setopt(e,CURLOPT_HEADERDATA,r);
    curl_easy_setopt(e,CURLOPT_NOSIGNAL,1L);
    CURLcode rc=curl_easy_perform(e); curl_easy_getinfo(e,CURLINFO_RESPONSE_CODE,&r->status); curl_easy_cleanup(e);
    if(wh_trace() || rc!=CURLE_OK){ char m[700]; int k=snprintf(m,sizeof m,"  [winhttp] %s %.400s -> %ld, %zu bytes%s%s\n",r->verb,url,r->status,r->nresp,
        rc!=CURLE_OK?" | curl: ":"",rc!=CURLE_OK?curl_easy_strerror(rc):""); (void)!write(2,m,k); }
    if(rc!=CURLE_OK){ uint32_t err=12029;   // ERROR_WINHTTP_CANNOT_CONNECT
        if(rc==CURLE_COULDNT_RESOLVE_HOST) err=12007; else if(rc==CURLE_OPERATION_TIMEDOUT) err=12002;
        else if(rc==CURLE_SSL_CONNECT_ERROR || rc==CURLE_PEER_FAILED_VERIFICATION) err=12175; else if(rc==CURLE_URL_MALFORMAT) err=12005;
        i_SetLastError(err); return 0; }
    r->done=1; return 1; }
// one header's value from the raw block (case-insensitive name; index = which occurrence)
static int wh_header(WhH*r,const char*name,uint32_t idx,char*out,size_t cap){ if(!r->rhdr) return 0; size_t nl=strlen(name); uint32_t seen=0;
    for(char*l=r->rhdr;*l;){ char*e=strstr(l,"\r\n"); size_t len=e?(size_t)(e-l):strlen(l);
        if(len>nl && l[nl]==':' && !strncasecmp(l,name,nl)){ if(seen++==idx){ const char*v=l+nl+1; while(*v==' '||*v=='\t') v++; size_t vl=(size_t)(l+len-v);
                if(vl+1>cap) vl=cap-1; memcpy(out,v,vl); out[vl]=0; return 1; } }
        l+=len; if(e) l+=2; else break; }
    return 0; }
static int MSABI i_WinHttpQueryHeaders(void*hr,uint32_t level,const uint16_t*name,void*buf,uint32_t*len,uint32_t*index){
    WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0; if(!r->done){ i_SetLastError(12019); return 0; }   // ERROR_WINHTTP_INCORRECT_HANDLE_STATE
    uint32_t info=level&0xFFFF, idx=index?*index:0; int num=(level&0x20000000)!=0, num64=(level&0x08000000)!=0;
    char v[8192]; int found=0; size_t vlen=0;
    switch(info){
    case 19: snprintf(v,sizeof v,"%ld",r->status); found=1; break;                                        // STATUS_CODE
    case 20: snprintf(v,sizeof v,"%s",r->statustext?r->statustext:""); found=1; break;                  // STATUS_TEXT
    case 18: snprintf(v,sizeof v,"%s","HTTP/1.1"); found=1; break;                                     // VERSION
    case 22: case 21: { size_t n=r->nrhdr; if(n+1>sizeof v) n=sizeof v-1; memcpy(v,r->rhdr?r->rhdr:"",n); v[n]=0; found=1; vlen=n; break; }   // RAW_HEADERS(_CRLF)
    default: { const char*hn=0; switch(info){ case 1: hn="Content-Type"; break; case 5: hn="Content-Length"; break; case 33: hn="Location"; break;
            case 9: hn="Date"; break; case 11: hn="Last-Modified"; break; case 54: hn="ETag"; break; case 29: hn="Content-Encoding"; break;
            case 65535: { static char cn[256]; wh_w2u(name,-1,cn,sizeof cn); hn=cn; break; } }
        if(hn) found=wh_header(r,hn,idx,v,sizeof v); break; } }
    if(!found){ i_SetLastError(12150); return 0; }   // ERROR_WINHTTP_HEADER_NOT_FOUND
    if(num || num64){ uint32_t need=num64?8:4; if(!buf || !len || *len<need){ if(len) *len=need; i_SetLastError(122); return 0; }
        unsigned long long x=strtoull(v,0,10); if(num64) *(uint64_t*)buf=x; else *(uint32_t*)buf=(uint32_t)x; *len=need; if(index) (*index)++; return 1; }
    if(!vlen) vlen=strlen(v);
    uint16_t wv[8200]; size_t nw=wh_u2w(v,vlen,wv,8200);
    if(info==21){ size_t k=0; for(size_t i=0;i<nw;i++){ if(wv[i]=='\r' && i+1<nw && wv[i+1]=='\n'){ wv[k++]=0; i++; } else wv[k++]=wv[i]; } nw=k; }   // RAW_HEADERS: one NUL per line
    uint32_t need=(uint32_t)((nw+1)*2); if(!buf || !len || *len<need){ if(len) *len=need; i_SetLastError(122); return 0; }
    memcpy(buf,wv,nw*2); ((uint16_t*)buf)[nw]=0; *len=(uint32_t)(nw*2); if(index) (*index)++; return 1; }
static int MSABI i_WinHttpQueryDataAvailable(void*hr,uint32_t*n){ WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0; if(n) *n=(uint32_t)(r->nresp-r->rd); return 1; }
static int MSABI i_WinHttpReadData(void*hr,void*buf,uint32_t n,uint32_t*got){ WhH*r=wh_get(hr,WH_REQUEST); if(!r) return 0;
    if(!r->done){ i_SetLastError(12019); return 0; }
    size_t k=r->nresp-r->rd; if(k>n) k=n; if(k) memcpy(buf,r->resp+r->rd,k); r->rd+=k; if(got) *got=(uint32_t)k; return 1; }
static int MSABI i_WinHttpCloseHandle(void*h){ WhH*w=wh_get(h,0); if(!w) return 0; w->magic=0;
    if(w->kind==WH_REQUEST){ curl_slist_free_all(w->hdrs); free(w->path); free(w->body); free(w->rhdr); free(w->resp); free(w->statustext); }
    free(w); return 1; }
// URL_COMPONENTS (x64): +0 size, +8 scheme*, +16 len, +20 nScheme, +24 host*, +32 len, +36 port (u16), +40 user*, +48 len,
// +56 pass*, +64 len, +72 path*, +80 len, +88 extra*, +96 len. A NULL pointer with a non-zero length = point into the URL.
static int wh_comp(uint8_t*uc,int po,int lo,const uint16_t*s,uint32_t n){ uint16_t**pp=(uint16_t**)(uc+po); uint32_t*pl=(uint32_t*)(uc+lo);
    if(!*pp){ if(*pl){ *pp=(uint16_t*)s; *pl=n; } return 1; }
    if(*pl<=n){ *pl=n+1; i_SetLastError(122); return 0; } memcpy(*pp,s,n*2); (*pp)[n]=0; *pl=n; return 1; }
static int MSABI i_WinHttpCrackUrl(const uint16_t*url,uint32_t n,uint32_t fl,uint8_t*uc){ (void)fl;
    if(!url || !uc){ i_SetLastError(87); return 0; } if(!n){ while(url[n]) n++; }
    uint32_t i=0; while(i+2<n && !(url[i]==':' && url[i+1]=='/' && url[i+2]=='/')) i++;
    if(i+2>=n){ i_SetLastError(12006); return 0; }   // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
    int https=(i==5 && (url[0]|32)=='h' && (url[4]|32)=='s'), http=(i==4 && (url[0]|32)=='h');
    if(!https && !http){ i_SetLastError(12006); return 0; }
    *(int*)(uc+20)=https?2:1; int ok=wh_comp(uc,8,16,url,i);
    uint32_t a=i+3, e=a; while(e<n && url[e]!='/' && url[e]!='?' && url[e]!='#') e++;
    uint32_t at=a; for(uint32_t k=a;k<e;k++) if(url[k]=='@') at=k+1;
    if(at>a){ uint32_t cl=a; while(cl<at-1 && url[cl]!=':') cl++; ok&=wh_comp(uc,40,48,url+a,cl-a); ok&=wh_comp(uc,56,64,url+(cl<at-1?cl+1:at-1),cl<at-1?at-1-cl-1:0); }
    else { ok&=wh_comp(uc,40,48,url+a,0); ok&=wh_comp(uc,56,64,url+a,0); }
    uint32_t he=e; int port=https?443:80; for(uint32_t k=at;k<e;k++) if(url[k]==':'){ he=k; port=0; for(uint32_t q=k+1;q<e;q++) port=port*10+(url[q]-'0'); break; }
    ok&=wh_comp(uc,24,32,url+at,he-at); *(uint16_t*)(uc+36)=(uint16_t)port;
    uint32_t q=e; while(q<n && url[q]!='?' && url[q]!='#') q++;
    ok&=wh_comp(uc,72,80,url+e,q-e); ok&=wh_comp(uc,88,96,url+q,n-q);
    return ok; }
