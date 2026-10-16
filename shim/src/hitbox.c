// hitbox.c — hitbox / bounding-volume overlay for Halo 3 objects. Included by peload.c.
// The retail halo3.dll compiled out its own debug_objects_* overlays (see shim/docs/halo3-engine-globals.md),
// so we read the engine's object table and draw the volumes ourselves.
//
// DO NOT scan process memory to find engine structures. A first attempt walked every VM region with
// mach_vm_region + mach_vm_read_overwrite looking for halo3's 'd@t@' data-array signature; on a ~6 GB
// Rosetta process (GPU/IOSurface-mapped regions included) it caused a macOS KERNEL PANIC (2026-10-16,
// "Kernel data abort", panicked task peload). Everything below follows pointers found by static analysis
// of halo3.dll and reads only those known, small ranges.
//
// What static analysis of halo3.dll (MCC build) gives (2026-10-16):
//   * halo3's static-TLS index is stored at halo3+0xa39f9c; thread-local block = gs:[0x58][index].
//   * The object table is a data array created with name "object" (halo3+0x33f5bf -> data_array_new
//     halo3+0x13003c, 0x800 entries x 0x18 bytes) and stored at TLS block + 0x38.
//   * Data array header (halo3+0x130244): +0x00 name[32], +0x20 max count, +0x24 element size,
//     +0x2c signature 'd@t@' (0x64407440), +0x48 data pointer, +0x50 in-use bitmap pointer.
#include <mach/mach.h>

static int hb_read(uint64_t a,void*dst,size_t n){ mach_vm_size_t got=0;   // one small, known address range
    return a>=0x10000 && mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)a,n,(mach_vm_address_t)(uintptr_t)dst,&got)==KERN_SUCCESS && got==n; }
static uint8_t* hb_halo3_base(void){ for(int k=0;k<g_nmods;k++) if(g_mods[k].name && strcasestr(g_mods[k].name,"halo3.dll")) return g_mods[k].base; return 0; }
// The "object" data array: the TLS block (of whichever guest thread runs the engine) whose +0x38 points at a
// header named "object" with the 'd@t@' signature.
static uint64_t hb_object_array(void){
    uint8_t*h3=hb_halo3_base(); if(!h3) return 0; uint32_t idx=0; if(!hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) return 0;
    void**arrs[1025]; int n=0; arrs[n++]=g_tls_array; pthread_mutex_lock(&g_tlsm); for(int i=0;i<g_ntarrs&&n<1025;i++) arrs[n++]=g_tarrs[i]; pthread_mutex_unlock(&g_tlsm);
    for(int i=0;i<n;i++){ uint64_t blk=(uint64_t)arrs[i][idx]; uint64_t arr=0; if(!blk || !hb_read(blk+0x38,&arr,8) || !arr) continue;
        uint8_t hdr[0x60]; if(!hb_read(arr,hdr,sizeof hdr)) continue;
        if(*(uint32_t*)(hdr+0x2c)==0x64407440 && !memcmp(hdr,"object\0",7)) return arr; }
    return 0; }

// PELOAD_OBJDUMP=1: every 3 s after gameplay starts, print the object array header and the first in-use
// entries (raw 0x18-byte headers and the first 0x100 bytes their pointers lead to), to learn the layouts.
static void hb_hex(const char*tag,const uint8_t*p,int n){ char m[512]; int k=snprintf(m,sizeof m,"  [objdump] %s",tag);
    for(int i=0;i<n;i+=4) k+=snprintf(m+k,sizeof m-k," %08x",*(const uint32_t*)(p+i)); m[k++]='\n'; (void)!write(2,m,k); }
static void* objdump_thread(void*a){ (void)a; pthread_setname_np("shim-objdump"); extern double g_gameplay_t0; while(!g_gameplay_t0) usleep(200000);
    for(int round=0; round<8; round++){ sleep(2); char m[600]; int k;
        uint64_t arr=hb_object_array(); if(!arr){ k=snprintf(m,sizeof m,"  [objdump] object array not found\n"); (void)!write(2,m,k); continue; }
        uint8_t hdr[0x60]; hb_read(arr,hdr,sizeof hdr); int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24); uint64_t data=*(uint64_t*)(hdr+0x48);
        if(esz!=0x18||maxc<=0||maxc>65536) continue;
        int types[32]={0}, nused=0;
        for(int i=0;i<maxc;i++){ uint8_t e[0x18]; if(!hb_read(data+(uint64_t)i*esz,e,esz)) break; if(!*(uint16_t*)e) continue; nused++; if(e[3]<32) types[e[3]]++;
            if(e[3]<=1){ uint64_t p=*(uint64_t*)(e+0x10); uint8_t od[0x200]; uint16_t dsz=*(uint16_t*)(e+6);
                k=snprintf(m,sizeof m,"  [objdump] round %d obj %d type %d size 0x%x data 0x%llx\n",round,i,e[3],dsz,(unsigned long long)p); (void)!write(2,m,k);
                if(p && hb_read(p,od,sizeof od)){ for(int r=0;r<0x200;r+=0x40){ k=snprintf(m,sizeof m,"  [objdump]   +%03x:",r);
                    for(int j=0;j<0x40;j+=4){ float f=*(float*)(od+r+j); uint32_t u=*(uint32_t*)(od+r+j);
                        k+= (f>-100000.f&&f<100000.f&&(f>0.001f||f<-0.001f)) ? snprintf(m+k,sizeof m-k," %9.3f",f) : snprintf(m+k,sizeof m-k," %9x",u); }
                    m[k++]='\n'; (void)!write(2,m,k); } } } }
        // node matrices: the biped datum holds runs of real_matrix4x3 (scale, forward, left, up, position = 13 floats)
        for(int i=0;i<maxc && round<2;i++){ uint8_t e[0x18]; if(!hb_read(data+(uint64_t)i*esz,e,esz) || !*(uint16_t*)e || e[3]!=0) continue;
            uint64_t p=*(uint64_t*)(e+0x10); uint16_t dsz=*(uint16_t*)(e+6); static uint8_t od[0x8000]; int n=dsz?dsz:0x2000; if(n>0x8000) n=0x8000;
            if(!p || !hb_read(p,od,n)) continue; const float*org=(const float*)(od+0x50);
            k=snprintf(m,sizeof m,"  [objdump] biped %d datum 0x%llx size 0x%x origin (%.2f %.2f %.2f) defidx %08x\n",i,(unsigned long long)p,dsz,org[0],org[1],org[2],*(uint32_t*)od); (void)!write(2,m,k);
            for(int o=0;o+52<=n;o+=4){ int run=0; for(int q=o;q+52<=n;q+=52){ const float*f=(const float*)(od+q);
                    float sc=f[0], d0=f[1]*f[1]+f[2]*f[2]+f[3]*f[3], d1=f[4]*f[4]+f[5]*f[5]+f[6]*f[6], d2=f[7]*f[7]+f[8]*f[8]+f[9]*f[9];
                    float dx=f[10]-org[0],dy=f[11]-org[1],dz=f[12]-org[2];
                    if(!(fabsf(sc-1)<0.05f && fabsf(d0-1)<0.02f && fabsf(d1-1)<0.02f && fabsf(d2-1)<0.02f && dx*dx+dy*dy+dz*dz<9.0f)) break; run++; }
                if(run>=8){ k=snprintf(m,sizeof m,"  [objdump]   matrix run at +0x%x x %d (ends +0x%x)\n",o,run,o+run*52); (void)!write(2,m,k); o+=run*52-4; } }
            for(int o=0;o<0x400 && o+4<=n;o+=2){ uint16_t a=*(uint16_t*)(od+o), b=*(uint16_t*)(od+o+2); if(a && b && b%52==0 && a<n && a>=0x100){ k=snprintf(m,sizeof m,"  [objdump]   blockref? +0x%x: size 0x%x (%d mats) offset 0x%x\n",o,b,b/52,a); (void)!write(2,m,k); }
                if(a && b && a%52==0 && b<n && b>=0x100){ k=snprintf(m,sizeof m,"  [objdump]   blockref? +0x%x: size 0x%x (%d mats) offset 0x%x (size first)\n",o,a,a/52,b); (void)!write(2,m,k); } }
            break; }
        k=snprintf(m,sizeof m,"  [objdump] round %d: %d objects; by type:",round,nused);
        for(int t=0;t<32;t++) if(types[t]) k+=snprintf(m+k,sizeof m-k," %d:%d",t,types[t]); m[k++]='\n'; (void)!write(2,m,k); }
    return 0; }
// ---- PELOAD_HITBOXES: draw bounding volumes over the presented frame --------------------------------------
// Object datum layout (MCC halo3, found by watching the player biped while it walked, 2026-10-16):
//   +0x1c bounding-sphere centre (x,y,z world units), +0x28 radius; +0x50 origin (feet for bipeds).
// Object header (0x18 B): +0x00 identifier (0 = free), +0x03 object type, +0x10 pointer to the datum.
// Types seen: 0 biped, 1 vehicle, 2 weapon, 3 equipment, 5 projectile, 6 scenery, 10 crate, 11 creature,
// 12 giant, 13 effect scenery. Default: bipeds, vehicles, giants. PELOAD_HITBOXES=all adds weapons,
// equipment and projectiles.
// Camera: the world vertex shaders compute clip = dot(worldpos, cb0[0..3]) and cb0[7] is the camera position;
// d3dgl_draw hands us cb0 rows 0-7 from the first full-size scene draw of each frame (hb_capture_view).
// PELOAD_HITBOXES=1: collision hitboxes (what damage tests) for bipeds, vehicles and giants -- bounding spheres where
// no collision model is found; =all also spheres for weapons, equipment and projectiles; =spheres: bounding spheres only.
static int g_hb_on=-1, g_hb_all, g_hb_spheres;
static int hb_enabled(void){ if(g_hb_on<0){ const char*e=getenv("PELOAD_HITBOXES"); g_hb_on=e&&*e&&strcmp(e,"0"); g_hb_all=e&&!strcasecmp(e,"all"); g_hb_spheres=e&&!strcasecmp(e,"spheres"); } return g_hb_on; }
// A world camera: rows 0-3 are a perspective view-projection (cb_looks_viewproj) AND row 7 is the camera position
// it was built from -- the camera sits at the clip-space origin (x = y = w = 0). Rejects the view-space-only
// projections of full-screen passes (row 3 = 0,0,1,0), shadow-map views and the first-person weapon's view.
static int hb_is_world_camera(const float*m){
    if(!cb_looks_viewproj(m,64)) return 0;
    const float*cam=m+28; float c2=cam[0]*cam[0]+cam[1]*cam[1]+cam[2]*cam[2]; if(c2<0.25f || c2>1e8f) return 0;
    static const int rows[3]={0,1,3};
    for(int i=0;i<3;i++){ const float*r=m+rows[i]*4; float sc=fabsf(r[0])+fabsf(r[1])+fabsf(r[2]); if(sc<1e-4f) return 0;
        float v=r[0]*cam[0]+r[1]*cam[1]+r[2]*cam[2]+r[3]; if(fabsf(v) > 0.02f*sc*(1.0f+sqrtf(c2))) return 0; }
    return 1; }
static float g_hb_vp[32]; static volatile unsigned g_hb_vpseq; volatile int g_hb_taken;
static void hb_capture_view(const float*rows){ g_hb_vpseq++; __atomic_thread_fence(__ATOMIC_SEQ_CST); memcpy(g_hb_vp,rows,sizeof g_hb_vp); __atomic_thread_fence(__ATOMIC_SEQ_CST); g_hb_vpseq++; }
typedef struct { float c[3], r, o[3]; int type; uint64_t p; uint32_t def; uint16_t nmsz, nmoff; } HbObj;
// PELOAD_INPUT_FACE (test helper for unattended overlay checks): steer the scripted pad toward the nearest other biped.
static float g_hb_tgt[3], g_hb_cam[3], g_hb_fwd[3]; static volatile int g_hb_tgt_ok;
int hb_face_target(float*rx,float*ry){ if(!g_hb_tgt_ok) return 0;
    float d[3]={g_hb_tgt[0]-g_hb_cam[0],g_hb_tgt[1]-g_hb_cam[1],g_hb_tgt[2]-g_hb_cam[2]}; float h=sqrtf(d[0]*d[0]+d[1]*d[1]); if(h<0.01f) return 0;
    float yaw_err=atan2f(d[1],d[0])-atan2f(g_hb_fwd[1],g_hb_fwd[0]); while(yaw_err>3.14159f) yaw_err-=6.28318f; while(yaw_err<-3.14159f) yaw_err+=6.28318f;
    float fh=sqrtf(g_hb_fwd[0]*g_hb_fwd[0]+g_hb_fwd[1]*g_hb_fwd[1]); float pitch_err=atan2f(d[2],h)-atan2f(g_hb_fwd[2],fh);
    float k=getenv("PELOAD_INPUT_FACE_GAIN")?atof(getenv("PELOAD_INPUT_FACE_GAIN")):1.5f, ps=getenv("PELOAD_INPUT_FACE_PITCH")?atof(getenv("PELOAD_INPUT_FACE_PITCH")):-1.0f;
    float x=-yaw_err*k, y=ps*pitch_err*k; *rx=x>0.6f?0.6f:x<-0.6f?-0.6f:x; *ry=y>0.6f?0.6f:y<-0.6f?-0.6f:y; return 1; }
static int hb_collect(HbObj*out,int max){
    static uint64_t arr; static double tarr; double now=mono_s(); if(!arr || now-tarr>2.0){ arr=hb_object_array(); tarr=now; } if(!arr) return 0;
    uint8_t hdr[0x60]; if(!hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440){ arr=0; return 0; }
    int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48);
    if(esz!=0x18 || hi<=0 || hi>maxc || hi>8192) return 0;
    static uint8_t tab[8192*0x18]; if(!hb_read(data,tab,(size_t)hi*0x18)) return 0;
    int n=0;
    for(int i=0;i<hi && n<max;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e) continue; int t=e[3];
        int want = t==0||t==1||t==12 || (g_hb_all && (t==2||t==3||t==5)); if(!want) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); uint8_t dh[0x140]; if(!p || !hb_read(p,dh,sizeof dh)) continue;
        const float*od=(const float*)(dh+0x1c);   // +0x1c centre, +0x28 radius, +0x50 origin
        HbObj*o=&out[n]; o->c[0]=od[0]; o->c[1]=od[1]; o->c[2]=od[2]; o->r=od[3]; o->o[0]=od[13]; o->o[1]=od[14]; o->o[2]=od[15]; o->type=t;
        o->p=p; o->def=*(const uint32_t*)dh; o->nmsz=*(const uint16_t*)(dh+0x136); o->nmoff=*(const uint16_t*)(dh+0x138);
        if(!(o->r>0.01f && o->r<50.f) || o->c[0]!=o->c[0]) continue; n++; }
    return n; }
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
typedef struct { char path[1024]; int fd; uint32_t tagoff; uint64_t vbase, fsize; int ntags; uint32_t*tp; uint16_t*salt; uint32_t*grp; } HbMap;
static HbMap g_hbm[6]; static int g_hbm_n;
static char g_hb_mappaths[8][1024]; static int g_hb_nmappaths; static pthread_mutex_t g_hb_mapmx=PTHREAD_MUTEX_INITIALIZER;
static void hb_note_map(const char*p){     // CreateFileW: remember the scenario maps the game opens (newest first)
    if(!hb_enabled() || !p) return; size_t n=strlen(p); if(n<5 || strcasecmp(p+n-4,".map")) return;
    const char*b=strrchr(p,'/'); b=b?b+1:p; if(!strcasecmp(b,"shared.map") || !strcasecmp(b,"campaign.map")) return;
    pthread_mutex_lock(&g_hb_mapmx); int k=0; for(; k<g_hb_nmappaths; k++) if(!strcmp(g_hb_mappaths[k],p)) break;
    if(k==g_hb_nmappaths && k<8) g_hb_nmappaths++; if(k>=8) k=7;
    for(; k>0; k--) memcpy(g_hb_mappaths[k],g_hb_mappaths[k-1],1024); snprintf(g_hb_mappaths[0],1024,"%s",p);
    pthread_mutex_unlock(&g_hb_mapmx); }
static int hbm_read(HbMap*m,uint64_t va,void*dst,size_t n){ if(va<m->vbase) return 0; uint64_t off=m->tagoff+(va-m->vbase); if(off+n>m->fsize) return 0;
    return pread(m->fd,dst,n,(off_t)off)==(ssize_t)n; }
static HbMap* hbm_get(const char*path){
    for(int i=0;i<g_hbm_n;i++) if(!strcmp(g_hbm[i].path,path)) return g_hbm[i].fd>=0?&g_hbm[i]:0;
    if(g_hbm_n>=6) return 0; HbMap*m=&g_hbm[g_hbm_n++]; memset(m,0,sizeof *m); snprintf(m->path,sizeof m->path,"%s",path); m->fd=open(path,O_RDONLY);
    uint8_t h[0x500]; struct stat st;
    if(m->fd<0 || fstat(m->fd,&st) || pread(m->fd,h,sizeof h,0)!=(ssize_t)sizeof h || memcmp(h,"daeh",4) || *(uint32_t*)(h+4)!=13) goto bad;
    m->fsize=(uint64_t)st.st_size; m->vbase=*(uint64_t*)(h+0x2E0); m->tagoff=(uint32_t)(*(uint32_t*)(h+0x4DC+16)+*(uint32_t*)(h+0x4CC+8));
    { uint8_t ix[0x48]; if(!hbm_read(m,*(uint64_t*)(h+0x2E8),ix,sizeof ix)) goto bad;
      int ng=*(int32_t*)ix, nt=*(int32_t*)(ix+0x10); if(ng<=0||ng>1024||nt<=0||nt>65535) goto bad;
      uint8_t*g=malloc((size_t)ng*16), *t=malloc((size_t)nt*8); m->tp=malloc(nt*4); m->salt=malloc(nt*2); m->grp=malloc(nt*4);
      if(!g||!t||!m->tp||!m->salt||!m->grp || !hbm_read(m,*(uint64_t*)(ix+8),g,(size_t)ng*16) || !hbm_read(m,*(uint64_t*)(ix+0x18),t,(size_t)nt*8)){ free(g); free(t); goto bad; }
      for(int i=0;i<nt;i++){ int16_t gi=*(int16_t*)(t+i*8); m->salt[i]=*(uint16_t*)(t+i*8+2); m->tp[i]=*(uint32_t*)(t+i*8+4); m->grp[i]=(gi>=0&&gi<ng)?*(uint32_t*)(g+gi*16):0; }
      m->ntags=nt; free(g); free(t); }
    { char msg[1200]; int k=snprintf(msg,sizeof msg,"  [hitbox] map %s: %d tags\n",path,m->ntags); (void)!write(2,msg,k); }
    return m;
  bad: { char msg[1200]; int k=snprintf(msg,sizeof msg,"  [hitbox] could not read map %s\n",path); (void)!write(2,msg,k); }
    if(m->fd>=0) close(m->fd); m->fd=-1; return 0; }
typedef struct { uint32_t def; int ok, nseg, nreg, maxnode; int16_t*node; uint8_t*reg; float*pts; } HbGeom;
static HbGeom g_hbg[96]; static int g_hbg_n;
static int hbm_block(HbMap*m,const uint8_t*d,int off,int esz,uint8_t**out,int maxn){   // tag block {count, p32} -> malloc'd elements
    int32_t c=*(const int32_t*)(d+off); uint32_t p=*(const uint32_t*)(d+off+4); *out=0; if(c<=0 || c>maxn || !p) return 0;
    uint8_t*b=malloc((size_t)c*esz); if(!b || !hbm_read(m,(uint64_t)p<<2,b,(size_t)c*esz)){ free(b); return 0; } *out=b; return c; }
static void hbg_build(HbGeom*g,HbMap*m,int ti){
    uint8_t ob[0x60], hl[0x40], co[0x44]; int cap=0;
    if(!hbm_read(m,(uint64_t)m->tp[ti]<<2,ob,sizeof ob)) return; uint32_t hd=*(uint32_t*)(ob+0x34+12); if(hd==0xFFFFFFFF || (hd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[hd&0xFFFF]<<2,hl,sizeof hl)) return; uint32_t cd=*(uint32_t*)(hl+0x10+12); if(cd==0xFFFFFFFF || (cd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[cd&0xFFFF]<<2,co,sizeof co)) return;
    uint8_t*regs; int nr=hbm_block(m,co,0x20,0x10,&regs,64);
    for(int r=0;r<nr && r<255;r++){ uint8_t*perms; int np=hbm_block(m,regs+r*0x10,4,0x28,&perms,256); if(!np) continue;   // permutation 0 = default
        uint8_t*bsps; int nb=hbm_block(m,perms,4,0x64,&bsps,256);
        for(int b=0;b<nb;b++){ const uint8_t*bp=bsps+b*0x64; int16_t node=*(const int16_t*)bp; uint8_t*ed,*vx;
            int ne=hbm_block(m,bp,0x4C,0xC,&ed,4096), nvx=hbm_block(m,bp,0x58,0x10,&vx,4096);
            for(int e=0;e<ne;e++){ int16_t a=*(int16_t*)(ed+e*12), z=*(int16_t*)(ed+e*12+2); if(a<0||z<0||a>=nvx||z>=nvx) continue;
                if(g->nseg>=cap){ cap=cap?cap*2:256; g->node=realloc(g->node,cap*2); g->reg=realloc(g->reg,cap); g->pts=realloc(g->pts,cap*6*4); }
                g->node[g->nseg]=node; g->reg[g->nseg]=(uint8_t)r; memcpy(g->pts+g->nseg*6,vx+a*16,12); memcpy(g->pts+g->nseg*6+3,vx+z*16,12); g->nseg++;
                if(node>g->maxnode) g->maxnode=node; }
            free(ed); free(vx); }
        free(bsps); free(perms); if(r+1>g->nreg) g->nreg=r+1; }
    free(regs); g->ok=g->nseg>0; }
// The collision geometry of an object definition (datum index), from whichever recently opened map holds that tag.
static HbGeom* hb_geom(uint32_t def){
    for(int i=0;i<g_hbg_n;i++) if(g_hbg[i].def==def) return g_hbg[i].ok?&g_hbg[i]:0;
    if(g_hbg_n>=96) return 0; HbGeom*g=&g_hbg[g_hbg_n++]; memset(g,0,sizeof *g); g->def=def;
    char paths[8][1024]; int np; pthread_mutex_lock(&g_hb_mapmx); np=g_hb_nmappaths; memcpy(paths,g_hb_mappaths,sizeof paths); pthread_mutex_unlock(&g_hb_mapmx);
    for(int k=0;k<np && !g->ok;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        if(m->grp[ti]!='bipd' && m->grp[ti]!='vehi' && m->grp[ti]!='gint') continue; hbg_build(g,m,ti);
        char msg[300]; int kk=snprintf(msg,sizeof msg,"  [hitbox] definition %08x: %d collision edges in %d regions, nodes 0-%d\n",def,g->nseg,g->nreg,g->maxnode); (void)!write(2,msg,kk); }
    return g->ok?g:0; }
// Append one object's posed collision edges as clip-space line vertices; returns vertices added (0 = no geometry).
static int hb_emit_hitbox(const HbObj*o,const float*vp,float*verts,int nv,int cap){
    HbGeom*g=hb_geom(o->def); if(!g) return 0;
    int nn=o->nmsz/52; if(o->nmsz%52 || nn<=g->maxnode || nn>128 || o->nmoff<0x100) return 0;
    static float mt[128*13]; if(!hb_read(o->p+o->nmoff,mt,(size_t)nn*52) || fabsf(mt[0]-1.0f)>0.1f) return 0;
    int head=-1;   // bipeds: the region whose nodes sit highest is the head
    if(o->type==0 && g->nreg>=2){ float zs[256]={0}; int zc[256]={0}; for(int s=0;s<g->nseg;s++){ zs[g->reg[s]]+=mt[g->node[s]*13+12]; zc[g->reg[s]]++; }
        float best=-1e30f; for(int r=0;r<g->nreg;r++) if(zc[r] && zs[r]/zc[r]>best){ best=zs[r]/zc[r]; head=r; } }
    static const float cbody[3]={1,0.15f,0.15f}, chead[3]={1,0.95f,0.1f}, cveh[3]={0.3f,0.75f,1};
    int added=0;
    for(int s=0;s<g->nseg && nv+added+2<=cap;s++){ const float*M=mt+g->node[s]*13, *P=g->pts+s*6; const float*col=o->type==0?(g->reg[s]==head?chead:cbody):cveh;
        float q[2][2]; int okk=1;
        for(int e=0;e<2;e++){ const float*v=P+e*3; float w[3];
            for(int a=0;a<3;a++) w[a]=M[10+a]+M[0]*(v[0]*M[1+a]+v[1]*M[4+a]+v[2]*M[7+a]);   // pos + scale*(x*fwd + y*left + z*up)
            float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
            if(cw<0.05f){ okk=0; break; } q[e][0]=cx/cw; q[e][1]=cy/cw; }
        if(!okk) continue; float*d=verts+(nv+added)*5;
        d[0]=q[0][0]; d[1]=q[0][1]; d[2]=col[0]; d[3]=col[1]; d[4]=col[2]; d[5]=q[1][0]; d[6]=q[1][1]; d[7]=col[0]; d[8]=col[1]; d[9]=col[2]; added+=2; }
    return added?added:-1; }   // -1: posed but entirely behind the camera
static unsigned g_hb_prog, g_hb_vao, g_hb_vbo;
static void hb_draw_overlay(void){
    if(!hb_enabled() || !G.DrawArrays || !G.CreateShader) return;
    float vp[32]; unsigned s0=g_hb_vpseq; if(s0&1) return; memcpy(vp,g_hb_vp,sizeof vp); if(g_hb_vpseq!=s0) return;
    if(!vp[0] && !vp[1] && !vp[2]){ static double tn; double now=mono_s(); if(getenv("PELOAD_HITBOXES_LOG") && now-tn>5.0){ tn=now; (void)!write(2,"  [hitbox] no camera matrix captured yet\n",41); } return; }
    if(!g_hb_prog){
        const char*vs="#version 410 core\nlayout(location=0) in vec2 p; layout(location=1) in vec3 c; out vec3 vc; void main(){ vc=c; gl_Position=vec4(p,0.0,1.0); }\n";
        const char*fs="#version 410 core\nin vec3 vc; out vec4 o; void main(){ o=vec4(vc,1.0); }\n";
        unsigned v=G.CreateShader(GLE_VS),f=G.CreateShader(GLE_FS); G.ShaderSource(v,1,&vs,0); G.CompileShader(v); G.ShaderSource(f,1,&fs,0); G.CompileShader(f);
        g_hb_prog=G.CreateProgram(); G.AttachShader(g_hb_prog,v); G.AttachShader(g_hb_prog,f); G.LinkProgram(g_hb_prog); G.DeleteShader(v); G.DeleteShader(f);
        G.GenBuffers(1,&g_hb_vbo); }
    { static void*vctx; void*cur=CGLGetCurrentContext(); if(cur!=vctx){ vctx=cur; G.GenVertexArrays(1,&g_hb_vao); } }   // VAOs are per context
    static HbObj objs[512]; int n=hb_collect(objs,512);
    if(getenv("PELOAD_HITBOXES_LOG")){ static double tl; double now=mono_s(); if(now-tl>2.0){ tl=now; char m[400]; int best=-1; float bd=1e30f;
        for(int i=0;i<n;i++){ float dx=objs[i].c[0]-vp[28],dy=objs[i].c[1]-vp[29],dz=objs[i].c[2]-vp[30],d=dx*dx+dy*dy+dz*dz; if(d<bd && d>1.0f){ bd=d; best=i; } }
        int k=snprintf(m,sizeof m,"  [hitbox] objs %d cam (%.2f %.2f %.2f) row3 (%.3f %.3f %.3f %.3f)",n,vp[28],vp[29],vp[30],vp[12],vp[13],vp[14],vp[15]);
        if(best>=0){ const float*w=objs[best].c; float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
            k+=snprintf(m+k,sizeof m-k," nearest type %d at (%.2f %.2f %.2f) r %.2f dist %.1f -> ndc (%.2f %.2f) w %.2f",objs[best].type,w[0],w[1],w[2],objs[best].r,sqrtf(bd),cw?cx/cw:0,cw?cy/cw:0,cw); }
        m[k++]='\n'; (void)!write(2,m,k); } }
    { float bd=1e30f; int bi=-1; for(int i=0;i<n;i++){ if(objs[i].type) continue; float dx=objs[i].c[0]-vp[28],dy=objs[i].c[1]-vp[29],dz=objs[i].c[2]-vp[30],d=dx*dx+dy*dy+dz*dz;
          if(d>(objs[i].r+0.3f)*(objs[i].r+0.3f) && d<bd){ bd=d; bi=i; } }
      if(bi>=0){ memcpy(g_hb_tgt,objs[bi].c,12); memcpy(g_hb_cam,vp+28,12); memcpy(g_hb_fwd,vp+12,12); g_hb_tgt_ok=1; } else g_hb_tgt_ok=0; }
    if(!n) return;
    enum { HB_CAP=400000 }; static float verts[HB_CAP*5]; int nv=0; const float*cam=vp+28;   // cb0[7] = camera position
    for(int i=0;i<n;i++){ const HbObj*o=&objs[i];
        float dx=o->c[0]-cam[0], dy=o->c[1]-cam[1], dz=o->c[2]-cam[2]; if(dx*dx+dy*dy+dz*dz < (o->r+0.3f)*(o->r+0.3f)) continue;   // our own body
        if(!g_hb_spheres && (o->type==0||o->type==1||o->type==12)){ int a=hb_emit_hitbox(o,vp,verts,nv,HB_CAP); if(a>0){ nv+=a; continue; } if(a<0) continue; }
        static const float pal[6][3]={{1,0.15f,0.15f},{1,0.85f,0.1f},{1,0.4f,0},{0.2f,1,0.3f},{0.2f,0.9f,1},{1,0.3f,1}};
        const float*col = pal[o->type==0?0 : o->type==1?1 : o->type==12?2 : o->type==2?3 : o->type==3?4 : 5];
        for(int axis=0;axis<3;axis++){ float prev[2]; int pv=0;
            for(int sgi=0;sgi<=24;sgi++){ float a=(float)sgi*(6.2831853f/24.f), ca=cosf(a)*o->r, sa=sinf(a)*o->r; float w[3]={o->c[0],o->c[1],o->c[2]};
                if(axis==0){ w[0]+=ca; w[1]+=sa; } else if(axis==1){ w[0]+=ca; w[2]+=sa; } else { w[1]+=ca; w[2]+=sa; }
                float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
                if(cw<0.05f){ pv=0; continue; } float nx=cx/cw, ny=cy/cw;
                if(pv && nv+2<=HB_CAP){ float*q=verts+nv*5; q[0]=prev[0]; q[1]=prev[1]; q[2]=col[0]; q[3]=col[1]; q[4]=col[2];
                    q[5]=nx; q[6]=ny; q[7]=col[0]; q[8]=col[1]; q[9]=col[2]; nv+=2; }
                prev[0]=nx; prev[1]=ny; pv=1; } } }
    if(!nv) return;
    G.Disable(0x0B71/*DEPTH_TEST*/); G.Disable(0x0BE2/*BLEND*/); G.Disable(0x0C11/*SCISSOR*/);
    G.UseProgram(g_hb_prog); G.BindVertexArray(g_hb_vao); G.BindBuffer(0x8892/*ARRAY_BUFFER*/,g_hb_vbo);
    G.BufferData(0x8892,(long)nv*5*4,verts,0x88E0/*STREAM_DRAW*/);
    G.EnableVertexAttribArray(0); G.VertexAttribPointer(0,2,0x1406/*FLOAT*/,0,20,(void*)0);
    G.EnableVertexAttribArray(1); G.VertexAttribPointer(1,3,0x1406,0,20,(void*)8);
    G.DrawArrays(1/*LINES*/,0,nv); G.BindVertexArray(0); }
static void hitbox_start(void){
    if(getenv("PELOAD_OBJDUMP")){ pthread_t t; pthread_create(&t,0,objdump_thread,0); } }
