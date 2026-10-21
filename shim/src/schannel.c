// schannel.c -- Windows' TLS (SSPI Schannel, secur32.dll) and the certificate-chain calls (crypt32.dll) a TLS client makes, on
// macOS's own LibreSSL (2026-10-20). #include'd by peload.c.
//
// Reach Renaissance's programs speak HTTPS to their master server (server list, joins, rendezvous) through Rust's native-tls ->
// schannel crate: AcquireCredentialsHandleA, InitializeSecurityContextW (the handshake, one token at a time), QueryContextAttributesW
// (stream sizes, the peer certificate), EncryptMessage / DecryptMessage (records in place), then CertGetCertificateChain +
// CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL) for the certificate check. None of it existed.
//
// The TLS engine is LibreSSL 3.3 from /usr/lib (libssl.48 / libcrypto.46: in every macOS's shared cache, loadable from x86_64,
// TLS 1.3) on memory BIOs, which is how SSPI works: the caller moves the bytes. Records are fed ONE at a time, so the SSPI rules
// hold exactly: a partial record = SEC_E_INCOMPLETE_MESSAGE (SECBUFFER_MISSING), bytes after the handshake's last record or after
// the record being decrypted = SECBUFFER_EXTRA. EncryptMessage turns one SSL_write into one record and lays it over the caller's
// header | data | trailer buffers (contiguous in every caller; their cbBuffer say how much to send). Only AEAD suites, so a record
// is the 5-byte header + at most 64 more bytes than the plaintext (StreamSizes 5 / 64 / 16384).
// The certificate is checked by LibreSSL against /etc/ssl/cert.pem (PELOAD_TLS_CAFILE=<file>) with the SNI host name as the
// expected name; the handshake itself never fails on it (SSL_VERIFY_NONE), and the result is what the chain / policy calls report
// -- the crate's own check then accepts or refuses, as on Windows. PELOAD_TLS_TRACE=1 logs handshakes and results;
// PELOAD_NO_SCHANNEL=1 = old (logging stubs: every TLS connection failed).

static int sch_on(void){ static int on=-1; if(on<0) on=!getenv("PELOAD_NO_SCHANNEL"); return on; }
static int sch_tr(void){ static int on=-1; if(on<0) on=getenv("PELOAD_TLS_TRACE")?1:0; return on; }

// ---- LibreSSL, loaded on first use ----
static struct {
    int ok;
    const void*(*client_method)(void);
    void*(*ctx_new)(const void*); int(*ctx_verify_locs)(void*,const char*,const char*); void(*ctx_set_verify)(void*,int,void*);
    int(*ctx_set_min)(void*,uint16_t); int(*ctx_cipher_list)(void*,const char*);
    void*(*ssl_new)(void*); void(*ssl_free)(void*); void(*set_bio)(void*,void*,void*); void(*set_connect)(void*);
    int(*handshake)(void*); int(*get_error)(const void*,int); long(*ssl_ctrl)(void*,int,long,void*);
    void*(*get0_param)(void*); int(*param_set1_host)(void*,const char*,size_t); long(*verify_result)(const void*);
    void*(*peer_cert)(const void*); int(*i2d_x509)(void*,unsigned char**); void(*x509_free)(void*);
    int(*ssl_read)(void*,void*,int); int(*ssl_write)(void*,const void*,int); int(*shutdown)(void*);
    int(*set_alpn)(void*,const unsigned char*,unsigned); void(*get_alpn)(const void*,const unsigned char**,unsigned*);
    const char*(*get_version)(const void*); const void*(*cur_cipher)(const void*); const char*(*cipher_name)(const void*);
    const void*(*bio_s_mem)(void); void*(*bio_new)(const void*); int(*bio_write)(void*,const void*,int); int(*bio_read)(void*,void*,int);
    long(*bio_ctrl)(void*,int,long,void*);
    unsigned long(*err_get)(void); void(*err_str)(unsigned long,char*,size_t); const char*(*verr_str)(long);
    void*ctx;   // one SSL_CTX for every connection (verify locations loaded once)
} L;
static pthread_mutex_t g_sch_mx=PTHREAD_MUTEX_INITIALIZER;
static int sch_lib(void){
    pthread_mutex_lock(&g_sch_mx);
    if(!L.ok){ L.ok=-1;
        void*c=dlopen("/usr/lib/libcrypto.46.dylib",RTLD_NOW|RTLD_LOCAL), *s=dlopen("/usr/lib/libssl.48.dylib",RTLD_NOW|RTLD_LOCAL);
        if(c&&s){
            #define LS(f,n) *(void**)&L.f=dlsym(s,n)
            #define LC(f,n) *(void**)&L.f=dlsym(c,n)
            LS(client_method,"TLS_client_method"); LS(ctx_new,"SSL_CTX_new"); LS(ctx_verify_locs,"SSL_CTX_load_verify_locations"); LS(ctx_set_verify,"SSL_CTX_set_verify");
            LS(ctx_set_min,"SSL_CTX_set_min_proto_version"); LS(ctx_cipher_list,"SSL_CTX_set_cipher_list");
            LS(ssl_new,"SSL_new"); LS(ssl_free,"SSL_free"); LS(set_bio,"SSL_set_bio"); LS(set_connect,"SSL_set_connect_state"); LS(handshake,"SSL_do_handshake");
            LS(get_error,"SSL_get_error"); LS(ssl_ctrl,"SSL_ctrl"); LS(get0_param,"SSL_get0_param"); LS(verify_result,"SSL_get_verify_result");
            LS(peer_cert,"SSL_get_peer_certificate"); LS(ssl_read,"SSL_read"); LS(ssl_write,"SSL_write"); LS(shutdown,"SSL_shutdown");
            LS(set_alpn,"SSL_set_alpn_protos"); LS(get_alpn,"SSL_get0_alpn_selected"); LS(get_version,"SSL_get_version"); LS(cur_cipher,"SSL_get_current_cipher");
            LS(cipher_name,"SSL_CIPHER_get_name");
            LC(param_set1_host,"X509_VERIFY_PARAM_set1_host"); LC(i2d_x509,"i2d_X509"); LC(x509_free,"X509_free"); LC(bio_s_mem,"BIO_s_mem"); LC(bio_new,"BIO_new");
            LC(bio_write,"BIO_write"); LC(bio_read,"BIO_read"); LC(bio_ctrl,"BIO_ctrl"); LC(err_get,"ERR_get_error"); LC(err_str,"ERR_error_string_n");
            LC(verr_str,"X509_verify_cert_error_string");
            #undef LS
            #undef LC
            if(L.client_method&&L.ctx_new&&L.ssl_new&&L.set_bio&&L.handshake&&L.ssl_read&&L.ssl_write&&L.bio_new&&L.bio_s_mem&&L.bio_ctrl&&L.verify_result&&L.peer_cert&&L.i2d_x509){
                L.ctx=L.ctx_new(L.client_method());
                if(L.ctx){ const char*ca=getenv("PELOAD_TLS_CAFILE"); if(!ca) ca="/etc/ssl/cert.pem";
                    if(L.ctx_verify_locs && L.ctx_verify_locs(L.ctx,ca,0)!=1){ char m[300]; int k=snprintf(m,sizeof m,"  [tls] cannot load the CA file %s: certificates will not verify\n",ca); (void)!write(2,m,k); }
                    if(L.ctx_set_verify) L.ctx_set_verify(L.ctx,0/*SSL_VERIFY_NONE: checked after, by the guest*/,0);
                    if(L.ctx_set_min) L.ctx_set_min(L.ctx,0x0303/*TLS 1.2*/);
                    if(L.ctx_cipher_list) L.ctx_cipher_list(L.ctx,"ECDHE+AESGCM:ECDHE+CHACHA20:!aNULL");   // AEAD only (TLS 1.3 suites are AEAD anyway)
                    L.ok=1; } } }
        if(L.ok!=1){ (void)!write(2,"  [tls] LibreSSL (/usr/lib/libssl.48.dylib) not usable: TLS connections will fail\n",82); } }
    int ok=L.ok==1; pthread_mutex_unlock(&g_sch_mx); return ok; }
static void sch_errlog(const char*what){ if(!L.err_get) return; unsigned long e; while((e=L.err_get())){ char b[256]; if(L.err_str) L.err_str(e,b,sizeof b); else snprintf(b,sizeof b,"%lx",e);
        char m[400]; int k=snprintf(m,sizeof m,"  [tls] %s: %s\n",what,b); (void)!write(2,m,k); } }

// ---- SSPI structures ----
typedef struct { uint32_t cb, type; void*pv; } SecBuf;
typedef struct { uint32_t ver, n; SecBuf*b; } SecBufDesc;
typedef struct { uintptr_t lo, hi; } SecHandle;
#define SCH_CRED_MAGIC 0x53434352u   // 'SCCR'
#define SCH_CTX_MAGIC  0x53435458u   // 'SCTX'
#define SCH_CERT_MAGIC 0x53434552u   // 'SCER'
enum { SB_EMPTY=0, SB_DATA=1, SB_TOKEN=2, SB_MISSING=4, SB_EXTRA=5, SB_TRAILER=6, SB_HEADER=7, SB_ALERT=17, SB_APPPROTO=18 };
#define SEC_OK 0
#define SEC_I_CONTINUE_NEEDED   0x00090312
#define SEC_I_CONTEXT_EXPIRED   0x00090317
#define SEC_E_INCOMPLETE_MESSAGE 0x80090318
#define SEC_E_INTERNAL_ERROR    0x80090304
#define SEC_E_INVALID_HANDLE    0x80090301
#define SEC_E_INVALID_TOKEN     0x80090308
#define SEC_E_UNSUPPORTED_FUNCTION 0x80090302
#define SEC_E_BUFFER_TOO_SMALL  0x80090321
#define SEC_E_DECRYPT_FAILURE   0x80090330
#define SEC_E_ILLEGAL_MESSAGE   0x80090326
#define SB_T(x) ((x)&0x0FFFFFFFu)
// CERT_CONTEXT (x64: encoding @0, pbCertEncoded @8, cbCertEncoded @16, pCertInfo @24, hCertStore @32) + our fields
typedef struct { uint32_t enc, _p0; uint8_t*der; uint32_t cb, _p1; void*info; void*store;
                 uint32_t magic; int refs; long verify; uint8_t infobuf[512]; } SchCert;
typedef struct { uint32_t magic; void*ssl, *rbio, *wbio; int done, shut, closed; long verify; char host[256]; uint8_t alpn[256]; int alpn_n; } SchCtx;
static void* sch_handle(const SecHandle*h,uint32_t magic){ if(!h||!h->lo) return 0; uint32_t*p=(uint32_t*)h->lo; return (*p==magic)?(void*)p:0; }
static size_t sch_pending(void*bio){ return (size_t)L.bio_ctrl(bio,10/*BIO_CTRL_PENDING*/,0,0); }
// the next complete TLS record in [p, p+n): its length, 0 = incomplete (*missing set)
static size_t sch_record(const uint8_t*p,size_t n,size_t*missing){ if(n<5){ *missing=5-n; return 0; } size_t len=5+(((size_t)p[3]<<8)|p[4]);
    if(n<len){ *missing=len-n; return 0; } *missing=0; return len; }
// pull everything LibreSSL wrote into a malloc'ed token (*out, *outn)
static void sch_take_out(SchCtx*c,uint8_t**out,uint32_t*outn){ size_t n=sch_pending(c->wbio); *out=0; *outn=0; if(!n) return; uint8_t*b=malloc(n); if(!b) return;
    int k=L.bio_read(c->wbio,b,(int)n); if(k<=0){ free(b); return; } *out=b; *outn=(uint32_t)k; }

// ---- secur32 ----
static uint32_t MSABI sch_AcquireCredentialsHandle(const void*principal,const void*package,uint32_t use,void*logon,void*auth,void*getkey,void*arg,SecHandle*cred,int64_t*expiry){
    (void)principal;(void)package;(void)logon;(void)auth;(void)getkey;(void)arg;
    if(!cred) return SEC_E_INVALID_HANDLE; if(!(use&2/*SECPKG_CRED_OUTBOUND*/)){ if(sch_tr()) ws("  [tls] inbound (server) credentials: not supported\n"); return SEC_E_UNSUPPORTED_FUNCTION; }
    uint32_t*cr=calloc(1,16); if(!cr) return SEC_E_INTERNAL_ERROR; cr[0]=SCH_CRED_MAGIC; cred->lo=(uintptr_t)cr; cred->hi=0; if(expiry) *expiry=0x7FFFFFFFFFFFFFFFLL;
    return SEC_OK; }
static uint32_t MSABI sch_FreeCredentialsHandle(SecHandle*cred){ uint32_t*cr=sch_handle(cred,SCH_CRED_MAGIC); if(cr){ cr[0]=0; free(cr); } return SEC_OK; }
static uint32_t MSABI sch_FreeContextBuffer(void*p){ free(p); return SEC_OK; }
static uint32_t sch_out_token(SchCtx*c,uint32_t req,SecBufDesc*out){ uint8_t*t; uint32_t tn; sch_take_out(c,&t,&tn);
    // the output token goes to the first TOKEN buffer, else the first EMPTY one (the schannel crate's first call passes EMPTY
    // and lets Windows fill it in -- with ISC_REQ_ALLOCATE_MEMORY -- as type TOKEN)
    SecBuf*ob=0; if(out) for(uint32_t i=0;i<out->n;i++) if(SB_T(out->b[i].type)==SB_TOKEN){ ob=&out->b[i]; break; }
    if(!ob && out) for(uint32_t i=0;i<out->n;i++) if(SB_T(out->b[i].type)==SB_EMPTY){ ob=&out->b[i]; ob->type=SB_TOKEN; ob->pv=(req&0x100)?0:ob->pv; ob->cb=(req&0x100)?0:ob->cb; break; }
    if(!ob){ free(t); return tn?SEC_E_BUFFER_TOO_SMALL:SEC_OK; }
    if(req&0x100/*ISC_REQ_ALLOCATE_MEMORY*/){ ob->pv=t; ob->cb=tn; return SEC_OK; }
    if(ob->cb<tn || (tn && !ob->pv)){ free(t); return SEC_E_BUFFER_TOO_SMALL; } if(tn) memcpy(ob->pv,t,tn); ob->cb=tn; free(t); return SEC_OK; }
static void sch_verify_done(SchCtx*c){ c->verify=L.verify_result(c->ssl);
    if(sch_tr()){ const char*v=L.get_version?L.get_version(c->ssl):"?"; const void*ci=L.cur_cipher?L.cur_cipher(c->ssl):0;
        char m[400]; int k=snprintf(m,sizeof m,"  [tls] handshake with %s done: %s %s, certificate %s (%ld)\n",c->host[0]?c->host:"(no name)",v?v:"?",ci&&L.cipher_name?L.cipher_name(ci):"?",
            c->verify==0?"verified":(L.verr_str?L.verr_str(c->verify):"NOT verified"),c->verify); (void)!write(2,m,k); } }
static uint32_t MSABI sch_InitializeSecurityContextW(SecHandle*cred,SecHandle*ctx,const uint16_t*target,uint32_t req,uint32_t r1,uint32_t rep,SecBufDesc*in,uint32_t r2,
        SecHandle*newctx,SecBufDesc*out,uint32_t*attr,int64_t*expiry){ (void)r1;(void)rep;(void)r2;
    if(!sch_lib()) return SEC_E_INTERNAL_ERROR;
    if(attr) *attr=req|((req&0x100)?0x100:0); if(expiry) *expiry=0x7FFFFFFFFFFFFFFFLL;
    SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC);
    if(!c){   // the first call: a new connection, its ClientHello
        if(!sch_handle(cred,SCH_CRED_MAGIC)) return SEC_E_INVALID_HANDLE;
        c=calloc(1,sizeof *c); if(!c) return SEC_E_INTERNAL_ERROR; c->magic=SCH_CTX_MAGIC; c->verify=-1;
        if(target){ int i=0; for(; target[i] && i<255; i++) c->host[i]=(char)(target[i]<128?target[i]:'?'); c->host[i]=0; }
        c->ssl=L.ssl_new(L.ctx); c->rbio=L.bio_new(L.bio_s_mem()); c->wbio=L.bio_new(L.bio_s_mem());
        if(!c->ssl||!c->rbio||!c->wbio){ sch_errlog("SSL_new"); free(c); return SEC_E_INTERNAL_ERROR; }
        L.set_bio(c->ssl,c->rbio,c->wbio); L.set_connect(c->ssl);
        if(c->host[0]){ L.ssl_ctrl(c->ssl,55/*SSL_CTRL_SET_TLSEXT_HOSTNAME*/,0/*host_name*/,c->host);
            if(L.get0_param && L.param_set1_host) L.param_set1_host(L.get0_param(c->ssl),c->host,0); }
        // ALPN (SECBUFFER_APPLICATION_PROTOCOLS: u32 size; then lists {u32 ext (2 = ALPN); u16 n; n bytes of wire-format names})
        if(in && L.set_alpn) for(uint32_t i=0;i<in->n;i++) if(SB_T(in->b[i].type)==SB_APPPROTO && in->b[i].pv && in->b[i].cb>=10){ const uint8_t*p=in->b[i].pv;
                uint32_t ext; uint16_t ln; memcpy(&ext,p+4,4); memcpy(&ln,p+8,2); if(ext==2 && ln && 10u+ln<=in->b[i].cb && ln<sizeof c->alpn){ memcpy(c->alpn,p+10,ln); c->alpn_n=ln; L.set_alpn(c->ssl,c->alpn,ln); } }
        if(newctx){ newctx->lo=(uintptr_t)c; newctx->hi=0; }
        int r=L.handshake(c->ssl); int e=r==1?0:L.get_error(c->ssl,r);
        if(r!=1 && e!=2/*WANT_READ*/ && e!=3/*WANT_WRITE*/){ sch_errlog("ClientHello"); return SEC_E_INTERNAL_ERROR; }
        uint32_t s=sch_out_token(c,req,out); if(s) return s;
        if(sch_tr()){ char m[320]; int k=snprintf(m,sizeof m,"  [tls] connecting to %s\n",c->host[0]?c->host:"(no name)"); (void)!write(2,m,k); }
        return SEC_I_CONTINUE_NEEDED; }
    if(newctx){ newctx->lo=(uintptr_t)c; newctx->hi=0; }
    if(c->shut){   // after ApplyControlToken(SCHANNEL_SHUTDOWN): the close_notify alert
        if(L.shutdown) L.shutdown(c->ssl); uint32_t s=sch_out_token(c,req,out); return s?s:SEC_OK; }
    // handshake: feed complete records, one at a time, until the handshake finishes or the input runs out
    SecBuf*tok=0,*extra=0; if(in) for(uint32_t i=0;i<in->n;i++){ uint32_t t=SB_T(in->b[i].type); if(t==SB_TOKEN&&!tok) tok=&in->b[i]; else if(t==SB_EMPTY&&!extra) extra=&in->b[i]; }
    const uint8_t*p=tok?tok->pv:0; size_t n=tok&&p?tok->cb:0, off=0, missing=0; int fed=0;
    while(!c->done){
        size_t rl=sch_record(p+off,n-off,&missing); if(!rl) break;
        L.bio_write(c->rbio,p+off,(int)rl); off+=rl; fed++;
        int r=L.handshake(c->ssl);
        if(r==1){ c->done=1; sch_verify_done(c); break; }
        int e=L.get_error(c->ssl,r); if(e!=2 && e!=3){ sch_errlog(c->host); uint8_t*t; uint32_t tn; sch_take_out(c,&t,&tn); free(t);   // (the alert we would send: not)
            if(sch_tr()){ char m[320]; int k=snprintf(m,sizeof m,"  [tls] handshake with %s failed (SSL error %d)\n",c->host,e); (void)!write(2,m,k); } return SEC_E_ILLEGAL_MESSAGE; } }
    if(!fed && !c->done){ for(uint32_t i=0;in && i<in->n;i++) if(SB_T(in->b[i].type)==SB_EMPTY){ in->b[i].type=SB_MISSING; in->b[i].cb=(uint32_t)missing; break; } return SEC_E_INCOMPLETE_MESSAGE; }
    if(extra && off<n){ extra->type=SB_EXTRA; extra->cb=(uint32_t)(n-off); extra->pv=0; }
    uint32_t s=sch_out_token(c,req,out); if(s) return s;
    return c->done?SEC_OK:SEC_I_CONTINUE_NEEDED; }
static uint32_t MSABI sch_InitializeSecurityContextA(SecHandle*cred,SecHandle*ctx,const char*target,uint32_t req,uint32_t r1,uint32_t rep,SecBufDesc*in,uint32_t r2,
        SecHandle*newctx,SecBufDesc*out,uint32_t*attr,int64_t*expiry){ uint16_t w[256]; int i=0; for(; target && target[i] && i<255; i++) w[i]=(uint8_t)target[i]; w[i]=0;
    return sch_InitializeSecurityContextW(cred,ctx,target?w:0,req,r1,rep,in,r2,newctx,out,attr,expiry); }
static uint32_t MSABI sch_DeleteSecurityContext(SecHandle*ctx){ SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC); if(!c) return SEC_E_INVALID_HANDLE;
    c->magic=0; if(c->ssl) L.ssl_free(c->ssl); /* (SSL_free frees both BIOs) */ free(c); return SEC_OK; }
static uint32_t MSABI sch_ApplyControlToken(SecHandle*ctx,SecBufDesc*in){ SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC); if(!c) return SEC_E_INVALID_HANDLE;
    if(in && in->n && in->b[0].pv && in->b[0].cb>=4 && *(uint32_t*)in->b[0].pv==1/*SCHANNEL_SHUTDOWN*/) c->shut=1; return SEC_OK; }
static SchCert* sch_cert_new(const uint8_t*der,uint32_t cb,long verify){ SchCert*k=calloc(1,sizeof *k+cb); if(!k) return 0;
    k->enc=1/*X509_ASN_ENCODING*/; k->der=(uint8_t*)(k+1); memcpy(k->der,der,cb); k->cb=cb; k->info=k->infobuf; k->store=0; k->magic=SCH_CERT_MAGIC; k->refs=1; k->verify=verify; return k; }
static uint32_t MSABI sch_QueryContextAttributesW(SecHandle*ctx,uint32_t a,void*buf){ SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC); if(!c) return SEC_E_INVALID_HANDLE; if(!buf) return SEC_E_INTERNAL_ERROR;
    switch(a){
    case 4: { uint32_t*s=buf; s[0]=5; s[1]=64; s[2]=16384; s[3]=4; s[4]=16; return SEC_OK; }   // StreamSizes: header, trailer, max message, buffers, block
    case 0x53: { void*x=L.peer_cert(c->ssl); if(!x) return SEC_E_INTERNAL_ERROR; unsigned char*d=0; int n=L.i2d_x509(x,&d); SchCert*k=0;   // REMOTE_CERT_CONTEXT
        if(n>0 && d){ k=sch_cert_new(d,(uint32_t)n,c->verify); free(d); } L.x509_free(x); if(!k) return SEC_E_INTERNAL_ERROR; *(void**)buf=k; return SEC_OK; }
    case 35: { uint8_t*o=buf; memset(o,0,4+4+1+255); const unsigned char*sel=0; unsigned sl=0; if(L.get_alpn) L.get_alpn(c->ssl,&sel,&sl);   // APPLICATION_PROTOCOL
        uint32_t st=sel&&sl?1/*SecApplicationProtocolNegotiationStatus_Success*/:0, ext=2; memcpy(o,&st,4); memcpy(o+4,&ext,4); if(st){ o[8]=(uint8_t)sl; memcpy(o+9,sel,sl); } return SEC_OK; }
    case 0x5a: { uint32_t*s=buf; memset(s,0,28); const char*v=L.get_version?L.get_version(c->ssl):""; s[0]= !strcmp(v,"TLSv1.3")?0x3000/*TLS1_3_CLIENT*/:0x800/*TLS1_2_CLIENT*/; return SEC_OK; }   // CONNECTION_INFO (protocol only)
    } if(sch_tr()){ char m[96]; int k=snprintf(m,sizeof m,"  [tls] QueryContextAttributes %#x not supported\n",a); (void)!write(2,m,k); } return SEC_E_UNSUPPORTED_FUNCTION; }
static uint32_t MSABI sch_EncryptMessage(SecHandle*ctx,uint32_t qop,SecBufDesc*m,uint32_t seq){ (void)qop;(void)seq; SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC); if(!c||!m) return SEC_E_INVALID_HANDLE;
    SecBuf*h=0,*d=0,*t=0; for(uint32_t i=0;i<m->n;i++){ uint32_t ty=SB_T(m->b[i].type); if(ty==SB_HEADER&&!h) h=&m->b[i]; else if(ty==SB_DATA&&!d) d=&m->b[i]; else if(ty==SB_TRAILER&&!t) t=&m->b[i]; }
    if(!h||!d||!t||h->cb<5) return SEC_E_INVALID_TOKEN; if(d->cb>16384) return SEC_E_BUFFER_TOO_SMALL;
    if(!d->cb){ h->cb=0; t->cb=0; return SEC_OK; }   // (nothing to send)
    if(d->cb && L.ssl_write(c->ssl,d->pv,(int)d->cb)!=(int)d->cb){ sch_errlog("EncryptMessage"); return SEC_E_INTERNAL_ERROR; }
    uint8_t*rec; uint32_t rn; sch_take_out(c,&rec,&rn); if(rn<5+d->cb || rn>5+d->cb+t->cb){ free(rec); return rn?SEC_E_BUFFER_TOO_SMALL:SEC_E_INTERNAL_ERROR; }
    memcpy(h->pv,rec,5); h->cb=5; memcpy(d->pv,rec+5,d->cb); uint32_t tl=rn-5-d->cb; memcpy(t->pv,rec+5+d->cb,tl); t->cb=tl; free(rec); return SEC_OK; }
static uint32_t MSABI sch_DecryptMessage(SecHandle*ctx,SecBufDesc*m,uint32_t seq,uint32_t*qop){ (void)seq; if(qop)*qop=0; SchCtx*c=sch_handle(ctx,SCH_CTX_MAGIC); if(!c||!m||m->n<4) return SEC_E_INVALID_HANDLE;
    SecBuf*b=m->b; if(SB_T(b[0].type)!=SB_DATA||!b[0].pv) return SEC_E_INVALID_TOKEN;
    uint8_t*p=b[0].pv; size_t n=b[0].cb, missing=0; size_t rl=sch_record(p,n,&missing);
    if(!rl){ b[0].type=SB_MISSING; b[0].cb=(uint32_t)missing; return SEC_E_INCOMPLETE_MESSAGE; }
    if(c->closed) return SEC_I_CONTEXT_EXPIRED;
    L.bio_write(c->rbio,p,(int)rl);
    uint8_t tmp[16384+256]; size_t got=0; int expired=0;
    for(;;){ int r=L.ssl_read(c->ssl,tmp+got,(int)(sizeof tmp-got)); if(r>0){ got+=(size_t)r; if(got>=sizeof tmp) break; continue; }
        int e=L.get_error(c->ssl,r); if(e==2/*WANT_READ*/||e==3) break; if(e==6/*ZERO_RETURN: close_notify*/){ expired=1; c->closed=1; break; }
        sch_errlog("DecryptMessage"); return SEC_E_DECRYPT_FAILURE; }
    { uint8_t*t; uint32_t tn; sch_take_out(c,&t,&tn); free(t); }   // (post-handshake output such as a key update reply: not sent)
    if(got>rl-5){ (void)!write(2,"  [tls] DecryptMessage: more plaintext than the record held\n",59); return SEC_E_INTERNAL_ERROR; }
    memmove(p+5,tmp,got);
    b[0].type=SB_HEADER; b[0].pv=p; b[0].cb=5;
    b[1].type=SB_DATA; b[1].pv=p+5; b[1].cb=(uint32_t)got;
    b[2].type=SB_TRAILER; b[2].pv=p+5+got; b[2].cb=(uint32_t)(rl-5-got);
    if(rl<n){ b[3].type=SB_EXTRA; b[3].pv=p+rl; b[3].cb=(uint32_t)(n-rl); } else { b[3].type=SB_EMPTY; b[3].pv=0; b[3].cb=0; }
    return expired?SEC_I_CONTEXT_EXPIRED:SEC_OK; }

// ---- crypt32: the certificate check the schannel crate makes after the handshake ----
static void* MSABI sch_CertDuplicateCertificateContext(void*cc){ SchCert*k=cc; if(k&&k->magic==SCH_CERT_MAGIC) __atomic_add_fetch(&k->refs,1,__ATOMIC_RELAXED); return cc; }
static int MSABI sch_CertFreeCertificateContext(void*cc){ SchCert*k=cc; if(k&&k->magic==SCH_CERT_MAGIC && __atomic_sub_fetch(&k->refs,1,__ATOMIC_ACQ_REL)==0){ k->magic=0; free(k); } return 1; }
// CERT_CHAIN_CONTEXT (72 B) -> one CERT_SIMPLE_CHAIN (40 B) -> one CERT_CHAIN_ELEMENT (56 B): the leaf, with LibreSSL's verdict
typedef struct { uint8_t chain[72]; uint8_t simple[40]; uint8_t elem[56]; void*psimple; void*pelem; SchCert*cert; long verify; int refs; } SchChain;
static int MSABI sch_CertGetCertificateChain(void*eng,void*cc,void*tm,void*store,void*para,uint32_t fl,void*rsv,void**out){ (void)eng;(void)tm;(void)store;(void)para;(void)fl;(void)rsv;
    if(!out) return 0; SchCert*k=cc; SchChain*ch=calloc(1,sizeof *ch); if(!ch) return 0;
    long v=(k&&k->magic==SCH_CERT_MAGIC)?k->verify:-1; uint32_t err=v==0?0:0x20/*CERT_TRUST_IS_UNTRUSTED_ROOT*/; ch->verify=v;
    if(k&&k->magic==SCH_CERT_MAGIC){ sch_CertDuplicateCertificateContext(k); ch->cert=k; }
    *(uint32_t*)ch->elem=56; *(void**)(ch->elem+8)=k; *(uint32_t*)(ch->elem+16)=err;
    ch->pelem=ch->elem; *(uint32_t*)ch->simple=40; *(uint32_t*)(ch->simple+4)=err; *(uint32_t*)(ch->simple+12)=1; *(void**)(ch->simple+16)=&ch->pelem;
    ch->psimple=ch->simple; *(uint32_t*)ch->chain=72; *(uint32_t*)(ch->chain+4)=err; *(uint32_t*)(ch->chain+12)=1; *(void**)(ch->chain+16)=&ch->psimple;
    ch->refs=1; *out=ch; return 1; }
static void MSABI sch_CertFreeCertificateChain(void*cc){ SchChain*ch=cc; if(!ch || __atomic_sub_fetch(&ch->refs,1,__ATOMIC_ACQ_REL)>0) return; if(ch->cert) sch_CertFreeCertificateContext(ch->cert); free(ch); }
static void* MSABI sch_CertDuplicateCertificateChain(void*cc){ SchChain*ch=cc; if(ch) __atomic_add_fetch(&ch->refs,1,__ATOMIC_RELAXED); return cc; }
static int MSABI sch_CertVerifyCertificateChainPolicy(const char*oid,void*cc,void*para,uint32_t*st){ (void)para; SchChain*ch=cc; if(!st) return 0;
    long v=ch?ch->verify:-1;
    uint32_t e = v==0 ? 0 : v==62/*X509_V_ERR_HOSTNAME_MISMATCH*/ ? 0x800B010Fu/*CERT_E_CN_NO_MATCH*/ : (v==10||v==9) ? 0x800B0101u/*CERT_E_EXPIRED*/ : 0x800B0109u/*CERT_E_UNTRUSTEDROOT*/;
    st[1]=e; st[2]=0; st[3]=0;
    if(sch_tr()){ char m[160]; int k=snprintf(m,sizeof m,"  [tls] chain policy %lu: %s\n",(unsigned long)(uintptr_t)oid,e?"REFUSED":"ok"); (void)!write(2,m,k); }
    return 1; }
static int MSABI sch_CertGetCertificateContextProperty(void*cc,uint32_t prop,void*data,uint32_t*cb){ (void)cc;(void)prop;(void)data;(void)cb; g_lasterr=0x80092004u/*CRYPT_E_NOT_FOUND*/; return 0; }
static void* MSABI sch_CertOpenStore(const char*prov,uint32_t enc,void*hp,uint32_t fl,const void*para){ (void)prov;(void)enc;(void)hp;(void)fl;(void)para; static uint32_t store=0x53544F52u; return &store; }
static int MSABI sch_CertCloseStore(void*s,uint32_t fl){ (void)s;(void)fl; return 1; }
static void* MSABI sch_CertDuplicateStore(void*s){ return s; }   // (native-tls clones its - empty - extra-roots store for every connection)
static int MSABI sch_CertAddCertificateContextToStore(void*s,void*cc,uint32_t how,void**out){ (void)s;(void)how; if(out) *out=sch_CertDuplicateCertificateContext(cc); return 1; }
static void* MSABI sch_CertEnumCertificatesInStore(void*s,void*prev){ (void)s; if(prev) sch_CertFreeCertificateContext(prev); return 0; }

static void* sch_resolve(const char*fn){
    if(!sch_on() || !fn) return 0;
    static const struct { const char*n; void*f; } T[]={
        {"AcquireCredentialsHandleA",(void*)sch_AcquireCredentialsHandle},{"AcquireCredentialsHandleW",(void*)sch_AcquireCredentialsHandle},
        {"FreeCredentialsHandle",(void*)sch_FreeCredentialsHandle},{"FreeContextBuffer",(void*)sch_FreeContextBuffer},
        {"InitializeSecurityContextW",(void*)sch_InitializeSecurityContextW},{"InitializeSecurityContextA",(void*)sch_InitializeSecurityContextA},
        {"DeleteSecurityContext",(void*)sch_DeleteSecurityContext},{"ApplyControlToken",(void*)sch_ApplyControlToken},
        {"QueryContextAttributesW",(void*)sch_QueryContextAttributesW},{"QueryContextAttributesA",(void*)sch_QueryContextAttributesW},
        {"EncryptMessage",(void*)sch_EncryptMessage},{"DecryptMessage",(void*)sch_DecryptMessage},
        {"CertDuplicateCertificateContext",(void*)sch_CertDuplicateCertificateContext},{"CertFreeCertificateContext",(void*)sch_CertFreeCertificateContext},
        {"CertGetCertificateChain",(void*)sch_CertGetCertificateChain},{"CertFreeCertificateChain",(void*)sch_CertFreeCertificateChain},{"CertDuplicateCertificateChain",(void*)sch_CertDuplicateCertificateChain},
        {"CertVerifyCertificateChainPolicy",(void*)sch_CertVerifyCertificateChainPolicy},{"CertGetCertificateContextProperty",(void*)sch_CertGetCertificateContextProperty},
        {"CertOpenStore",(void*)sch_CertOpenStore},{"CertCloseStore",(void*)sch_CertCloseStore},{"CertEnumCertificatesInStore",(void*)sch_CertEnumCertificatesInStore},
        {"CertDuplicateStore",(void*)sch_CertDuplicateStore},{"CertAddCertificateContextToStore",(void*)sch_CertAddCertificateContextToStore},
        {0,0}};
    for(int i=0;T[i].n;i++) if(!strcmp(T[i].n,fn)) return T[i].f;
    return 0; }
