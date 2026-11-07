// h3maps.c -- Halo 3 .map files on disk: the maps the game opens, the map being played, tag names, collision geometry and
// vehicle outlines. Shared by the loader (included by hitbox.c) and the native app's dylib (shim/native/nstats.c).
// Before including it define: hb_read(addr, dst, n) (a safe read), hb_object_array() / hb_object_array_tls(&tls) (the
// engine's "object" data array and the TLS block holding it), hb_scenario_name(out, n), mono_s(); optionally HB_MAXOBJ
// (object slots read; the loader's 8192 default, the native app's tables are larger).
#ifndef HB_MAXOBJ
#define HB_MAXOBJ 8192
#endif
// ---- real hitboxes: the object's collision model, posed by its live node matrices --------------------------------
// Damage tests use the model's collision model (coll): per region (head, body, arms, legs), per permutation, a set of
// small convex BSPs each attached to one skeleton node. The geometry lives in the map's tag data, which we read from
// the .map file on disk (the game opens it; hb_note_map remembers the path) -- layouts from Assembly's Halo3MCC
// plugins and LayoutsU13 (see re-tools/h3map.py, the same reader in Python):
//   header: +0x2E0 virtual base (u64), +0x2E8 index header address (u64), +0x4CC section offset masks[4],
//           +0x4DC sections[4] {va,size} (debug, resource, tag, locale); tag data file offset = tag.va + mask[2].
//   tag pointers are 32-bit: address = p << 2; file offset = tag data offset + (address - virtual base).
//   index header: +0 group count, +8 group table (16 B: magic), +0x10 tag count, +0x18 tag table (8 B: group, salt, p).
//   object def (bipd/vehi): +0x34 model tagref -> hlmt +0x10 collision model tagref (datum index at tagref+0xC)
//   coll: +0x20 regions[0x10] { +4 permutations[0x28] { +4 bsps[0x64] { +0 node, +0x4C edges[0xC] {start,end},
//         +0x58 vertices[0x10] {x,y,z} } } }
// Collision node indices equal the model's node indices (checked: mp_masterchief 51/51, mp_elite 55/55).
// Node matrices (real_matrix4x3: scale, forward, left, up, position) are in the object datum; the header block
// reference at datum+0x136 {u16 size, u16 offset} locates them (mp_masterchief: 0xa5c = 51 x 52 bytes at +0x758).
typedef struct { char path[1024]; int fd; uint32_t tagoff; uint64_t vbase, fsize; int ntags; uint32_t*tp; uint16_t*salt; uint32_t*grp; char*names; int32_t*noff; } HbMap;
#define HB_NMAPS 48   // map files opened for tag data (never closed: callers keep the pointer); a long session plays many maps
static HbMap g_hbm[HB_NMAPS]; static int g_hbm_n; static pthread_mutex_t g_hbm_mx=PTHREAD_MUTEX_INITIALIZER;
#define HB_NPATHS 32   // recently opened scenario maps: around a map change the client also opens maps it borrows skins / tags from
static char g_hb_mappaths[HB_NPATHS][1024]; static int g_hb_nmappaths; static pthread_mutex_t g_hb_mapmx=PTHREAD_MUTEX_INITIALIZER;
static void hb_note_map(const char*p){     // CreateFileW: remember the scenario maps the game opens (newest first)
    if(!p) return; size_t n=strlen(p); if(n<5 || strcasecmp(p+n-4,".map")) return;   // always: F2 can enable the overlay later
    const char*b=strrchr(p,'/'); b=b?b+1:p; if(!strcasecmp(b,"shared.map") || !strcasecmp(b,"campaign.map")) return;
    // already the newest: nothing to do (no header read, no list shift -- a map the game keeps opening)
    { pthread_mutex_lock(&g_hb_mapmx); int top=g_hb_nmappaths>0 && !strcmp(g_hb_mappaths[0],p); pthread_mutex_unlock(&g_hb_mapmx); if(top) return; }
    // only real Halo 3 cache maps (header "daeh", version 13): the engine also writes its autosaves as .map files
    // (ReclaimerCache/Temp/.../native-data/autosave/asq<hex>.map), and enough of those used to push the map being played
    // out of this list -- the stats page then named the map after an autosave and found no picture for it
    { int fd=open(p,O_RDONLY); uint8_t h[8]; int ok=fd>=0 && pread(fd,h,8,0)==8 && !memcmp(h,"daeh",4) && *(uint32_t*)(h+4)==13; if(fd>=0) close(fd); if(!ok) return; }
    pthread_mutex_lock(&g_hb_mapmx); int k=0; for(; k<g_hb_nmappaths; k++) if(!strcmp(g_hb_mappaths[k],p)) break;
    if(k==g_hb_nmappaths && k<HB_NPATHS) g_hb_nmappaths++; if(k>=HB_NPATHS) k=HB_NPATHS-1;
    for(; k>0; k--) memcpy(g_hb_mappaths[k],g_hb_mappaths[k-1],1024); snprintf(g_hb_mappaths[0],1024,"%s",p);
    pthread_mutex_unlock(&g_hb_mapmx); }
// The map being played. Around a map change the client opens other maps too (it borrows skins and tags from them --
// "<map> gained N objects and tags from other maps"; Workshop maps such as Forge's desert_sandbox), so "the newest map
// file opened" named the wrong one (2026-10-18: heatmap, walker, hitbox geometry and tag names all followed
// desert_sandbox while Sandtrap was played). Now: the candidate (halo3/maps or a Workshop folder) whose tag table
// matches the most live objects' definition datums (index -> salt); the newest wins ties and is the answer before
// any object exists (menu). Rechecked every 2 s; g_hb_mapgen counts changes (caches keyed by definition reset on it).
static HbMap* hbm_get(const char*path);
static volatile unsigned g_hb_mapgen;
static int hb_live_defs(uint32_t*defs,int max){   // distinct definition datums of live objects (the object table)
    uint64_t arr=hb_object_array(); if(!arr) return 0; uint8_t hdr[0x60];
    if(!hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) return 0;
    int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48);
    if(esz!=0x18 || hi<=0 || hi>maxc || hi>HB_MAXOBJ) return 0;
    static uint8_t tab[HB_MAXOBJ*0x18]; static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; int n=0; pthread_mutex_lock(&mx);
    if(hb_read(data,tab,(size_t)hi*0x18)) for(int i=0;i<hi && n<max;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); uint32_t d=0; if(!p || !hb_read(p,&d,4) || d==0xFFFFFFFFu || !(d>>16)) continue;
        int dup=0; for(int k=0;k<n;k++) if(defs[k]==d){ dup=1; break; } if(!dup) defs[n++]=d; }
    pthread_mutex_unlock(&mx); return n; }
static volatile int g_hb_map_recheck;   // set: the next call decides again now (stats.c, when a match's loading screen ends)
static int hb_current_map(char*out,size_t n){
    static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; static char cur[1024]; static double tchk; static int last_np=-1;
    pthread_mutex_lock(&mx); double now=mono_s();
    char paths[HB_NPATHS][1024]; int np; pthread_mutex_lock(&g_hb_mapmx); np=g_hb_nmappaths; memcpy(paths,g_hb_mappaths,sizeof(paths[0])*(size_t)np); pthread_mutex_unlock(&g_hb_mapmx);
    int force=__atomic_exchange_n(&g_hb_map_recheck,0,__ATOMIC_ACQ_REL);
    // ... and at once when the engine's scenario changes (polled every 0.1 s): a match's first seconds were still the loading
    // screen's scenery (130_epilogue, 2 s recheck), so the native app's first spawn summary named Guardian's weapons from the
    // epilogue's tags ("Dervish head", "Flood softform break small", 2026-11-07); PELOAD_MAP_SLOWCHECK=1 = the 2 s check only
    { static double tsn; static char lsn[128]; static int slow=-1; if(slow<0) slow=getenv("PELOAD_MAP_SLOWCHECK")!=0;
      if(!slow && now-tsn>0.1){ tsn=now; char sn2[128]; if(hb_scenario_name(sn2,sizeof sn2) && strcmp(sn2,lsn)){ force=1; snprintf(lsn,sizeof lsn,"%s",sn2); } } }
    if(!cur[0] || now-tchk>2.0 || np!=last_np || force){ tchk=now; last_np=np;
        // the engine's own scenario name decides when one candidate file carries it (the definition match is a vote:
        // Workshop maps built on the same tag base can tie on it, and the newer of two tied files used to win)
        static uint32_t defs[256]; int nd=hb_live_defs(defs,256); int best=-1, bs=-1, cs=-1; char sn[128]; int hs=hb_scenario_name(sn,sizeof sn);
        for(int k=0;k<np;k++){ if(!strstr(paths[k],"/halo3/maps/") && !strstr(paths[k],"/workshop/content/")) continue;
            if(hs){ const char*b=strrchr(paths[k],'/'); b=b?b+1:paths[k]; size_t L=strlen(sn); if(!strncasecmp(b,sn,L) && !strcasecmp(b+L,".map")){ best=k; bs=-2; break; } }
            int sc=0; if(nd){ HbMap*m=hbm_get(paths[k]); if(!m) continue;
                for(int i=0;i<nd;i++){ int ti=defs[i]&0xFFFF; if(ti<m->ntags && m->salt[ti]==(defs[i]>>16) && m->grp[ti]) sc++; } }
            if(!strcmp(paths[k],cur)) cs=sc;
            if(sc>bs){ bs=sc; best=k; } }   // (paths are newest first: ties keep the newer...)
        if(bs>=0 && cs>=bs){ for(int k=0;k<np;k++) if(!strcmp(paths[k],cur)){ best=k; break; } }   // (...but never over the current one)
        if(best<0 && np) best=0;
        // the engine names its scenario but no recent file is it: the client opens many maps after a match starts (skin /
        // helmet catalogs, maps it borrows from), so the played map can fall out of the 32 recent paths -- a Valhalla
        // game was then voted to Standoff (88 of 88 definitions match both). Use halo3/maps/<scenario>.map itself.
        static char forced[1024]; const char*pick=best>=0?paths[best]:0;
        if(hs && bs!=-2){ for(int k=0;k<np;k++){ const char*mp=strstr(paths[k],"/halo3/maps/"); if(!mp) continue;
                snprintf(forced,sizeof forced,"%.*s/halo3/maps/%s.map",(int)(mp-paths[k]),paths[k],sn);
                if(!access(forced,R_OK)){ pick=forced; bs=-2; } break; } }
        if(pick && strcmp(cur,pick)){
            if(cur[0]){ char m[2200]; int kk=bs==-2?snprintf(m,sizeof m,"  [hitbox] map being played: %s (the engine's scenario %s)\n",pick,sn)
                    :snprintf(m,sizeof m,"  [hitbox] map being played: %s (%d of %d live definitions match)\n",pick,bs,nd); (void)!write(2,m,kk); }
            snprintf(cur,sizeof cur,"%s",pick); __atomic_add_fetch(&g_hb_mapgen,1,__ATOMIC_ACQ_REL); } }
    snprintf(out,n,"%s",cur); pthread_mutex_unlock(&mx); return out[0]!=0; }
static int hbm_read(HbMap*m,uint64_t va,void*dst,size_t n){ if(va<m->vbase) return 0; uint64_t off=m->tagoff+(va-m->vbase); if(off+n>m->fsize) return 0;
    return pread(m->fd,dst,n,(off_t)off)==(ssize_t)n; }
static HbMap* hbm_get_l(const char*path);
static HbMap* hbm_get(const char*path){ pthread_mutex_lock(&g_hbm_mx); HbMap*m=hbm_get_l(path); pthread_mutex_unlock(&g_hbm_mx); return m; }
static HbMap* hbm_get_l(const char*path){
    for(int i=0;i<g_hbm_n;i++) if(!strcmp(g_hbm[i].path,path)) return g_hbm[i].fd>=0?&g_hbm[i]:0;
    if(g_hbm_n>=HB_NMAPS) return 0; HbMap*m=&g_hbm[g_hbm_n++]; memset(m,0,sizeof *m); snprintf(m->path,sizeof m->path,"%s",path); m->fd=open(path,O_RDONLY);
    uint8_t h[0x500]; struct stat st;
    if(m->fd<0 || fstat(m->fd,&st) || pread(m->fd,h,sizeof h,0)!=(ssize_t)sizeof h || memcmp(h,"daeh",4) || *(uint32_t*)(h+4)!=13) goto bad;
    m->fsize=(uint64_t)st.st_size; m->vbase=*(uint64_t*)(h+0x2E0); m->tagoff=(uint32_t)(*(uint32_t*)(h+0x4DC+16)+*(uint32_t*)(h+0x4CC+8));
    { uint8_t ix[0x48]; if(!hbm_read(m,*(uint64_t*)(h+0x2E8),ix,sizeof ix)) goto bad;
      int ng=*(int32_t*)ix, nt=*(int32_t*)(ix+0x10); if(ng<=0||ng>1024||nt<=0||nt>65535) goto bad;
      uint8_t*g=malloc((size_t)ng*16), *t=malloc((size_t)nt*8); m->tp=malloc(nt*4); m->salt=malloc(nt*2); m->grp=malloc(nt*4);
      if(!g||!t||!m->tp||!m->salt||!m->grp || !hbm_read(m,*(uint64_t*)(ix+8),g,(size_t)ng*16) || !hbm_read(m,*(uint64_t*)(ix+0x18),t,(size_t)nt*8)){ free(g); free(t); goto bad; }
      for(int i=0;i<nt;i++){ int16_t gi=*(int16_t*)(t+i*8); m->salt[i]=*(uint16_t*)(t+i*8+2); m->tp[i]=*(uint32_t*)(t+i*8+4); m->grp[i]=(gi>=0&&gi<ng)?*(uint32_t*)(g+gi*16):0; }
      m->ntags=nt; free(g); free(t); }
    { // tag names: header +0x20 count, +0x24 names offset, +0x28 names size, +0x2C index offset; file offset = value + debug mask (+0x4CC[0])
      int32_t cnt=*(int32_t*)(h+0x20), sz=*(int32_t*)(h+0x28); uint32_t dm=*(uint32_t*)(h+0x4CC), no=(uint32_t)(*(uint32_t*)(h+0x24)+dm), io=(uint32_t)(*(uint32_t*)(h+0x2C)+dm);
      if(cnt==m->ntags && sz>0 && sz<(64<<20)){ m->names=malloc((size_t)sz+1); m->noff=malloc((size_t)cnt*4);
          if(!m->names||!m->noff || pread(m->fd,m->names,(size_t)sz,(off_t)no)!=sz || pread(m->fd,m->noff,(size_t)cnt*4,(off_t)io)!=cnt*4){ free(m->names); free(m->noff); m->names=0; m->noff=0; }
          else { m->names[sz]=0; for(int i=0;i<cnt;i++) if(m->noff[i]<0 || m->noff[i]>=sz) m->noff[i]=sz; } } }   // (offsets from the file: outside the names = "")
    { char msg[1200]; int k=snprintf(msg,sizeof msg,"  [hitbox] map %s: %d tags%s\n",path,m->ntags,m->names?" (with names)":""); (void)!write(2,msg,k); }
    return m;
  bad: { char msg[1200]; int k=snprintf(msg,sizeof msg,"  [hitbox] could not read map %s\n",path); (void)!write(2,msg,k); }
    if(m->fd>=0) close(m->fd); m->fd=-1; return 0; }
// Per definition: every collision BSP edge of every region and permutation, plus the model's (hlmt) collision-region
// table that maps the object's per-region permutation bytes to collision permutations:
//   hlmt +0xA0 Collision Regions[0x14] { +0 name, +4 s8 collision region, +8 Permutations[8] { +0 name, +5 s8 collision perm } }
// Objects keep one byte per hlmt collision region (the chosen permutation): block reference at datum+0x13A
// {s16 size, s16 offset}, count = (size - (datum+4 bit 11 ? 0x20 : 0)) / 10, bytes at datum + offset + count*8
// (found in halo3 object_set_permutation, halo3+0x3457d8 / +0x345d38). Elite helmets are permutations of "head".
#define HB_MAXSEG (1<<18)    // collision edges kept per object definition (stock models: < 20k; a crafted map can alias blocks into billions)
typedef struct { uint32_t def; int ok, nseg, nreg, maxnode; int16_t*node; uint8_t*reg, *perm, *wk; float*pts;
    int nhreg; int8_t hcreg[32], hnperm[32], hcperm[32][64]; } HbGeom;
static HbGeom g_hbg[96]; static int g_hbg_n;
static int hbm_block(HbMap*m,const uint8_t*d,int off,int esz,uint8_t**out,int maxn){   // tag block {count, p32} -> malloc'd elements
    int32_t c=*(const int32_t*)(d+off); uint32_t p=*(const uint32_t*)(d+off+4); *out=0; if(c<=0 || c>maxn || !p) return 0;
    uint8_t*b=malloc((size_t)c*esz); if(!b || !hbm_read(m,(uint64_t)p<<2,b,(size_t)c*esz)){ free(b); return 0; } *out=b; return c; }
// Vehicle weak spots (backlog 31; re-tools/h3dmg.py prints the same data for a whole map). The model's damage info
// (hlmt +0x88 [0xA4]) splits the body into damage sections (+0x68 [0x2C]: +0 name, +8 share of body vitality, +0xC instant
// responses [0x88]: +8 flags, +0x40 region, +0x44 new state); a hit's section comes from the collision surface it lands on:
// surface material (bsp +0x40 surfaces [0xC], s16 at +4) -> coll materials (+0x14, name sid) -> hlmt Materials (+0x7C [0x14]:
// +0 name sid, +6 s16 section). Class 2: a section smaller than the body whose response kills or destroys the vehicle
// (Wraith back hatch: 10% = 75 of 750). Class 1: the cheapest section whose response puts the region that a shielded
// seat's region-specific damage (seats +0x8C [0x20]: +4 direct scale, +0x14 [0x2C]: +0 region, +0x20 major direct scale)
// opens up at "major" into that state (Scorpion driver hatch, Wraith cockpit: the driver then takes full direct hits).
// Class 3 (2026-10-18): a section under half the body whose response (not only on special death) damages a gun trigger, the
// steering or the engine or forces a weapon drop (flags bits 4, 8-15): Ghost wings (gun + steering, 80 of 400), Warthog front
// fenders and Mongoose wheels (steering). Every other vehicle's sections only change its looks: one damage pool.
static void hbg_weak(HbMap*m,const uint8_t*hl,const uint8_t*co,uint8_t*matcls,int ncm){
    memset(matcls,0,(size_t)ncm); uint8_t*di; if(hbm_block(m,hl,0x88,0xA4,&di,4)<1) return;
    uint8_t*sec; int ns=hbm_block(m,di,0x68,0x2C,&sec,64); uint8_t scls[64]={0};
    uint32_t open[8]; int nopen=0; uint8_t*st; int nst=hbm_block(m,di,0x8C,0x20,&st,32);
    for(int j=0;j<nst;j++){ if(*(float*)(st+j*0x20+4)>=0.5f) continue; uint8_t*rr; int nr=hbm_block(m,st+j*0x20,0x14,0x2C,&rr,8);
        for(int k=0;k<nr && nopen<8;k++) if(*(float*)(rr+k*0x2C+0x20)>=0.9f) open[nopen++]=*(uint32_t*)(rr+k*0x2C); free(rr); }
    float best=2; int bi=-1;
    for(int i=0;i<ns;i++){ const uint8_t*e=sec+i*0x2C; float vit=*(const float*)(e+8); uint8_t*rs; int nr=hbm_block(m,e,0xC,0x88,&rs,32);
        for(int k=0;k<nr;k++){ uint32_t f=*(uint32_t*)(rs+k*0x88+8), reg=*(uint32_t*)(rs+k*0x88+0x40); int16_t ns_=*(int16_t*)(rs+k*0x88+0x44);
            if((f&(1u|1u<<7|1u<<16)) && !(f&(1u<<23)) && vit<0.99f && vit>0.001f) scls[i]=2;
            if((f&(1u<<4|0xFFu<<8)) && !(f&(1u<<23)) && vit<0.5f && vit>0.001f && !scls[i]) scls[i]=3;
            for(int q=0;q<nopen;q++) if(reg==open[q] && ns_>=3 && vit<best){ best=vit; bi=i; } }
        free(rs); }
    if(bi>=0 && scls[bi]!=2) scls[bi]=1;
    uint8_t*hm; int nhm=hbm_block(m,hl,0x7C,0x14,&hm,64); uint8_t*cm; int n=hbm_block(m,co,0x14,4,&cm,64);
    for(int c=0;c<n && c<ncm;c++) for(int h=0;h<nhm;h++){ int16_t si=*(int16_t*)(hm+h*0x14+6);
        if(*(uint32_t*)(hm+h*0x14)==*(uint32_t*)(cm+c*4) && si>=0 && si<ns && si<64) matcls[c]=scls[si]; }
    free(hm); free(cm); free(sec); free(st); free(di); }
static void hbg_build(HbGeom*g,HbMap*m,int ti){
    uint8_t ob[0x60], hl[0xB0], co[0x44]; int cap=0;
    if(!hbm_read(m,(uint64_t)m->tp[ti]<<2,ob,sizeof ob)) return; uint32_t hd=*(uint32_t*)(ob+0x34+12); if(hd==0xFFFFFFFF || (hd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[hd&0xFFFF]<<2,hl,sizeof hl)) return; uint32_t cd=*(uint32_t*)(hl+0x10+12); if(cd==0xFFFFFFFF || (cd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[cd&0xFFFF]<<2,co,sizeof co)) return;
    uint8_t matcls[64]; memset(matcls,0,sizeof matcls); if(m->grp[ti]=='vehi') hbg_weak(m,hl,co,matcls,64);   // (bipeds: head / body colors instead)
    uint8_t*regs; int nr=hbm_block(m,co,0x20,0x10,&regs,64);
    for(int r=0;r<nr && r<255;r++){ uint8_t*perms; int np=hbm_block(m,regs+r*0x10,4,0x28,&perms,64);
        for(int pi=0;pi<np;pi++){ uint8_t*bsps; int nb=hbm_block(m,perms+pi*0x28,4,0x64,&bsps,256);
            for(int b=0;b<nb;b++){ const uint8_t*bp=bsps+b*0x64; int16_t node=*(const int16_t*)bp; uint8_t*ed,*vx,*sf;
                int ne=hbm_block(m,bp,0x4C,0xC,&ed,4096), nvx=hbm_block(m,bp,0x58,0x10,&vx,4096), nsf=hbm_block(m,bp,0x40,0xC,&sf,4096);
                if(node<0 || node>=128) ne=0;   // a node index outside the pose matrices (mt[128*13]): a damaged / crafted model
                for(int e=0;e<ne && g->nseg<HB_MAXSEG;e++){ int16_t a=*(int16_t*)(ed+e*12), z=*(int16_t*)(ed+e*12+2); if(a<0||z<0||a>=nvx||z>=nvx) continue;
                    if(g->nseg>=cap){ int nc=cap?cap*2:256; int16_t*n1=realloc(g->node,(size_t)nc*2); if(n1) g->node=n1; uint8_t*n2=realloc(g->reg,(size_t)nc); if(n2) g->reg=n2;
                        uint8_t*n3=realloc(g->perm,(size_t)nc); if(n3) g->perm=n3; uint8_t*n4=realloc(g->wk,(size_t)nc); if(n4) g->wk=n4; float*n5=realloc(g->pts,(size_t)nc*6*4); if(n5) g->pts=n5;
                        if(!n1||!n2||!n3||!n4||!n5) break; cap=nc; }
                    uint8_t wk=0; for(int sd=0;sd<2;sd++){ int16_t si=*(int16_t*)(ed+e*12+8+sd*2); if(si<0||si>=nsf) continue;   // left / right surface
                        int16_t mi=*(int16_t*)(sf+si*12+4); if(mi>=0 && mi<64 && matcls[mi]>wk) wk=matcls[mi]; }
                    g->node[g->nseg]=node; g->reg[g->nseg]=(uint8_t)r; g->perm[g->nseg]=(uint8_t)pi; g->wk[g->nseg]=wk; memcpy(g->pts+g->nseg*6,vx+a*16,12); memcpy(g->pts+g->nseg*6+3,vx+z*16,12); g->nseg++;
                    if(node>g->maxnode) g->maxnode=node; }
                free(ed); free(vx); free(sf); }
            free(bsps); }
        free(perms); if(r+1>g->nreg) g->nreg=r+1; }
    free(regs);
    uint8_t*hr; int nh=hbm_block(m,hl,0xA0,0x14,&hr,32); g->nhreg=nh;   // model collision regions -> collision region / permutations
    for(int i=0;i<nh;i++){ g->hcreg[i]=(int8_t)hr[i*0x14+4]; uint8_t*hp; int np=hbm_block(m,hr+i*0x14,8,8,&hp,64); g->hnperm[i]=(int8_t)np;
        for(int q=0;q<np;q++) g->hcperm[i][q]=(int8_t)hp[q*8+5]; free(hp); }
    free(hr); g->ok=g->nseg>0; }
static const char* hb_tag_name_early(uint32_t def,HbMap*m,int ti){ (void)def; if(!m->names||ti>=m->ntags) return "?"; const char*f=m->names+m->noff[ti]; const char*b=strrchr(f,'\\'); return b?b+1:f; }
// The collision geometry of an object definition (datum index), from whichever recently opened map holds that tag.
// Definition datums are only unique within one map: the cache starts over when the map being played changes, and a
// definition is looked up in that map (only if it isn't found there, in the other recent ones).
static unsigned g_hbg_gen;
static HbGeom* hb_geom(uint32_t def){
    if(g_hbg_gen!=g_hb_mapgen){ g_hbg_gen=g_hb_mapgen; for(int i=0;i<g_hbg_n;i++){ free(g_hbg[i].node); free(g_hbg[i].reg); free(g_hbg[i].perm); free(g_hbg[i].wk); free(g_hbg[i].pts); } g_hbg_n=0; }
    for(int i=0;i<g_hbg_n;i++) if(g_hbg[i].def==def) return g_hbg[i].ok?&g_hbg[i]:0;
    if(g_hbg_n>=96) return 0; HbGeom*g=&g_hbg[g_hbg_n++]; memset(g,0,sizeof *g); g->def=def;
    char paths[HB_NPATHS+1][1024]; int np=0; hb_current_map(paths[0],1024); if(paths[0][0]) np=1;
    { pthread_mutex_lock(&g_hb_mapmx); for(int k=0;k<g_hb_nmappaths;k++) if(strcmp(g_hb_mappaths[k],paths[0])) memcpy(paths[np++],g_hb_mappaths[k],1024); pthread_mutex_unlock(&g_hb_mapmx); }
    for(int k=0;k<np && !g->ok;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        if(m->grp[ti]!='bipd' && m->grp[ti]!='vehi' && m->grp[ti]!='gint') continue; hbg_build(g,m,ti);
        int nw[4]={0}; for(int q=0;q<g->nseg;q++) nw[g->wk[q]<4?g->wk[q]:0]++;
        char msg[300]; int kk=snprintf(msg,sizeof msg,"  [hitbox] definition %08x (%s): %d collision edges in %d regions (%d model regions), nodes 0-%d; weak-spot edges: %d lethal, %d driver, %d disabling\n",def,hb_tag_name_early(def,m,ti),g->nseg,g->nreg,g->nhreg,g->maxnode,nw[2],nw[1],nw[3]); (void)!write(2,msg,kk); }
    return g->ok?g:0; }
// ---- vehicle outlines for the stats page map (user, 2026-10-26: "mini icon assets ... that represent the real vehicle") ----
// A vehicle definition's top-down outline in its own frame (x forward, y left, units), from its collision model posed by one
// live instance's node matrices: for each (collision region, node) group of edges, the convex hull of the edge end points.
// Returns malloc'd JSON {"len":L,"wid":W,"parts":[[z,x0,y0,x1,y1,...],...]} (parts by height, lowest first, z their mean
// height) or 0 (no collision model, pose unreadable). Built privately (the render thread owns the g_hbg cache).
static int hbv_cmp(const void*a,const void*b){ const float*p=a,*q=b; return p[0]<q[0]?-1:p[0]>q[0]?1:p[1]<q[1]?-1:p[1]>q[1]?1:p[2]<q[2]?-1:p[2]>q[2]; }
static float hbv_cross(const float*o,const float*a,const float*b){ return (a[1]-o[1])*(b[2]-o[2])-(a[2]-o[2])*(b[1]-o[1]); }
char* hb_vshape(uint32_t def,uint64_t datum){
    HbGeom g; memset(&g,0,sizeof g); g.def=def;
    char paths[HB_NPATHS+1][1024]; int np=0; hb_current_map(paths[0],1024); if(paths[0][0]) np=1;
    { pthread_mutex_lock(&g_hb_mapmx); for(int k=0;k<g_hb_nmappaths;k++) if(strcmp(g_hb_mappaths[k],paths[0])) memcpy(paths[np++],g_hb_mappaths[k],1024); pthread_mutex_unlock(&g_hb_mapmx); }
    for(int k=0;k<np && !g.ok;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || ti>=m->ntags || m->salt[ti]!=(def>>16) || m->grp[ti]!='vehi') continue; hbg_build(&g,m,ti); }
    char*out=0; float*pt=0;
    if(!g.ok) goto done;
    uint8_t dh[0x140]; if(!hb_read(datum,dh,sizeof dh)) goto done;
    int nmsz=*(const uint16_t*)(dh+0x136), nmoff=*(const uint16_t*)(dh+0x138); int nn=nmsz/52;
    if(nmsz%52 || nn<=g.maxnode || nn>128 || nmoff<0x100) goto done;
    float mt[128*13]; if(!hb_read(datum+(uint64_t)nmoff,mt,(size_t)nn*52) || fabsf(mt[0]-1.0f)>0.1f) goto done;
    uint8_t sel[256]; memset(sel,0,sizeof sel);   // the instance's collision permutation per region (as hb_emit_hitbox)
    { int16_t rs=*(const int16_t*)(dh+0x13a); if((*(const uint32_t*)(dh+4)>>11)&1) rs-=0x20; int rc=rs>0?rs/10:0, ro=*(const uint16_t*)(dh+0x13c); uint8_t pb[32];
      if(g.nhreg>0 && rc>0 && rc<=32 && hb_read(datum+(uint64_t)ro+(uint64_t)rc*8,pb,(size_t)rc))
        for(int i=0;i<g.nhreg && i<rc;i++){ int pi=pb[i]; if(pi>=g.hnperm[i]) pi=0; int cr=g.hcreg[i], cp=g.hnperm[i]?g.hcperm[i][pi]:0; if(cr>=0 && cr<256 && cp>=0) sel[cr]=(uint8_t)cp; } }
    const float*P=(const float*)(dh+0x50), *F=(const float*)(dh+0x5c), *U=(const float*)(dh+0x68);
    float L[3]={U[1]*F[2]-U[2]*F[1], U[2]*F[0]-U[0]*F[2], U[0]*F[1]-U[1]*F[0]};   // left = up x forward
    pt=malloc((size_t)g.nseg*2*4*sizeof(float)); if(!pt) goto done; int n=0;   // [group, x, y, z]
    for(int s2=0;s2<g.nseg;s2++){ if(g.perm[s2]!=sel[g.reg[s2]]) continue; const float*M=mt+g.node[s2]*13;
        for(int e=0;e<2;e++){ const float*v=g.pts+s2*6+e*3; float w[3];
            for(int a=0;a<3;a++) w[a]=M[10+a]+M[0]*(v[0]*M[1+a]+v[1]*M[4+a]+v[2]*M[7+a]);
            float d[3]={w[0]-P[0],w[1]-P[1],w[2]-P[2]}; float*q=pt+n*4;
            q[0]=(float)(g.reg[s2]*128+g.node[s2]); q[1]=d[0]*F[0]+d[1]*F[1]+d[2]*F[2]; q[2]=d[0]*L[0]+d[1]*L[1]+d[2]*L[2]; q[3]=d[0]*U[0]+d[1]*U[1]+d[2]*U[2];
            if(isfinite(q[1]) && isfinite(q[2]) && fabsf(q[1])<60 && fabsf(q[2])<60) n++; } }
    if(n<3) goto done;
    qsort(pt,(size_t)n,4*sizeof(float),hbv_cmp);   // by group, then x, then y: each group's points in hull order
    typedef struct { float z; int a, k; } Part; Part parts[256]; int npart=0; float*hull=malloc((size_t)n*2*4*sizeof(float)); int nh=0; if(!hull) goto done;
    float x0=1e9f,x1=-1e9f,y0=1e9f,y1=-1e9f;
    for(int i=0;i<n && npart<256;){ int j=i; float zs=0; while(j<n && pt[j*4]==pt[i*4]){ zs+=pt[j*4+3]; j++; }
        int start=nh;   // Andrew's monotone chain over pt[i..j)
        for(int pass=0;pass<2;pass++){ int base=nh;
            for(int q=pass?j-1:i; pass?q>=i:q<j; q+=pass?-1:1){ const float*c=pt+q*4;
                while(nh-base>=2 && hbv_cross(hull+(nh-2)*4,hull+(nh-1)*4,c)<=0) nh--;
                memcpy(hull+nh*4,c,4*sizeof(float)); nh++; }
            nh--; }   // (the last point of each half is the first of the other)
        if(nh-start>=3){ parts[npart].z=zs/(float)(j-i); parts[npart].a=start; parts[npart].k=nh-start; npart++;
            for(int q=start;q<nh;q++){ float x=hull[q*4+1], y=hull[q*4+2]; if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y; } }
        else nh=start;
        i=j; }
    if(!npart){ free(hull); goto done; }
    for(int a=1;a<npart;a++){ Part t=parts[a]; int b=a-1; while(b>=0 && parts[b].z>t.z){ parts[b+1]=parts[b]; b--; } parts[b+1]=t; }   // lowest first
    size_t cap=64+(size_t)nh*24+(size_t)npart*16; out=malloc(cap);
    if(out){ size_t o=(size_t)snprintf(out,cap,"{\"len\":%.2f,\"wid\":%.2f,\"x0\":%.2f,\"x1\":%.2f,\"y0\":%.2f,\"y1\":%.2f,\"parts\":[",x1-x0,y1-y0,x0,x1,y0,y1);
        for(int a=0;a<npart && o+64<cap;a++){ o+=(size_t)snprintf(out+o,cap-o,"%s[%.2f",a?",":"",parts[a].z);
            for(int q=0;q<parts[a].k && o+32<cap;q++){ const float*c=hull+(parts[a].a+q)*4; o+=(size_t)snprintf(out+o,cap-o,",%.2f,%.2f",c[1],c[2]); }
            o+=(size_t)snprintf(out+o,cap-o,"]"); }
        snprintf(out+o,cap-o,"]}"); }
    free(hull);
  done:
    free(pt); free(g.node); free(g.reg); free(g.perm); free(g.wk); free(g.pts); return out; }
// Short display name of an object definition (last path component of its tag name, '_' -> ' ', upper case).
static const char* hb_tag_name_l(uint32_t def);
static const char* hb_tag_name(uint32_t def){   // also called by the stats thread (stats.c)
    static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mx); const char*r=hb_tag_name_l(def); pthread_mutex_unlock(&mx); return r; }
static const char* hb_tag_name_l(uint32_t def){
    static struct { uint32_t def; char nm[32]; } cache[256]; static int nc; static unsigned gen;
    if(gen!=g_hb_mapgen){ gen=g_hb_mapgen; nc=0; }   // (definition datums are per map: names start over with the map)
    for(int i=0;i<nc;i++) if(cache[i].def==def) return cache[i].nm;
    char out[32]="?"; char paths[HB_NPATHS+1][1024]; int np=0; hb_current_map(paths[0],1024); if(paths[0][0]) np=1;   // the map being played first
    { pthread_mutex_lock(&g_hb_mapmx); for(int k=0;k<g_hb_nmappaths;k++) if(strcmp(g_hb_mappaths[k],paths[0])) memcpy(paths[np++],g_hb_mappaths[k],1024); pthread_mutex_unlock(&g_hb_mapmx); }
    for(int k=0;k<np;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || !m->names || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        const char*full=m->names+m->noff[ti]; const char*b=strrchr(full,'\\'); b=b?b+1:full; int j=0;
        for(; b[j] && j<31; j++) out[j]=(char)(b[j]=='_'?' ':(b[j]>='a'&&b[j]<='z'?b[j]-32:b[j])); out[j]=0; break; }
    if(nc<256){ cache[nc].def=def; snprintf(cache[nc].nm,sizeof cache[nc].nm,"%s",out); return cache[nc++].nm; }
    static char last[32]; snprintf(last,sizeof last,"%s",out); return last; }
