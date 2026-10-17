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
// Overlay features: each has a launch variable and a function key (wininput.c flips bit f of g_hb_toggle):
//   F2 PELOAD_HITBOXES   hitboxes (=all adds weapon/equipment/projectile spheres, =spheres: bounding spheres only)
//   F3 PELOAD_HB_TRACERS projectile tracers (trail + velocity)
//   F4 PELOAD_HB_INFO    player info: shield/health %, weapon, grenades, active camo, vehicle seat
//   F5 PELOAD_HB_ITEMS   weapons and equipment lying on the map, by tag name
//   F9 PELOAD_HB_VISIBLE visible only: hitboxes and labels only for objects the player can actually see
enum { HBF_HITBOX, HBF_TRACER, HBF_INFO, HBF_ITEMS, HBF_VISIBLE, HBF_N };
static int g_hbf_init, g_hbf_on[HBF_N], g_hb_all, g_hb_spheres;
static void hbf_init(void){ if(g_hbf_init) return; static const char*const env[HBF_N]={"PELOAD_HITBOXES","PELOAD_HB_TRACERS","PELOAD_HB_INFO","PELOAD_HB_ITEMS","PELOAD_HB_VISIBLE"};
    for(int f=0;f<HBF_N;f++){ const char*e=getenv(env[f]); g_hbf_on[f]=e&&*e&&strcmp(e,"0"); }
    const char*e=getenv("PELOAD_HITBOXES"); g_hb_all=e&&!strcasecmp(e,"all"); g_hb_spheres=e&&!strcasecmp(e,"spheres"); g_hbf_init=1; }
static int hb_feat(int f){ hbf_init(); return g_hbf_on[f] ^ ((g_hb_toggle>>f)&1); }
static int hb_overlay_on(void){ for(int f=0;f<HBF_N;f++) if(f!=HBF_VISIBLE && hb_feat(f)) return 1; return 0; }   // (visible-only alone draws nothing)
// The camera tracker (VS cb0 of the frame's first world draw) feeds the overlay and the teleport's "which body is
// mine"; it runs when either needs it.
static int stats_on(void);   // stats.c: the live stats service uses the camera to mark "you"
static int hb_enabled(void){ static int tp=-1; if(tp<0) tp=getenv("PELOAD_ALLOW_TELEPORT")||getenv("PELOAD_TEST_BOOT_PROMPT")?1:0; return tp || hb_overlay_on() || stats_on(); }
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
// Object datum fields (MCC halo3, from the evaluate functions of its HaloScript getters -- shim/docs/halo3-objects.md):
//   +0x00 definition tag datum   +0x10 parent object (the vehicle when seated)   +0x1c bounding centre, +0x28 radius
//   +0x50 origin   +0x5c forward   +0x68 up   +0x74 velocity   +0x80 angular velocity
//   +0xF4 health (body vitality)  +0xF8 shield vitality  +0x110 flags (bit 2: vitality zeroed; health/shield read as 0)
//   +0x136 node matrices {u16 size, u16 offset}
// unit (biped/vehicle/giant): +0x174 team index  +0x24E seat (-1 = on foot)  +0x262 current weapon slot (s8)
//   +0x268 weapon object handles[4]  +0x294 grenade counts[4] (frag, plasma, spike, firebomb)  +0x360 active camo
typedef struct { float c[3], r, o[3], vel[3]; int type; uint32_t handle; uint64_t p; uint32_t def; uint16_t nmsz, nmoff;
    float health, shield, camo; int team, seat, wslot, dead; uint32_t weapons[4], parent; int8_t gren[4]; int rcount; uint16_t roff; } HbObj;
// Local player's team (the biped the camera is inside) and the friend/enemy filter: F6 or PELOAD_HB_TEAMS=all|enemies|friends.
static int g_hb_myteam=-1, g_hb_teamfilter=-1;
static volatile int g_hb_local_slot=-1; static volatile uint32_t g_hb_local_unit=0xFFFFFFFFu;   // stats.c finds the local player (also in third person)
static int hb_is_friend(const HbObj*o){ return g_hb_myteam>=0 && o->type==0 && o->team==g_hb_myteam; }
static int hb_team_mode(void){   // 0 all, 1 enemies only, 2 friends only
    if(g_hb_teamfilter<0){ const char*e=getenv("PELOAD_HB_TEAMS"); g_hb_teamfilter = e&&!strcasecmp(e,"enemies")?1 : e&&!strcasecmp(e,"friends")?2 : 0; }
    return (g_hb_teamfilter + g_hb_teamcycle)%3; }
static int hb_team_shown(const HbObj*o){ if(o->type!=0) return 1;
    int f=hb_team_mode(); return f==0 || (f==1 && !hb_is_friend(o)) || (f==2 && hb_is_friend(o)); }
// PELOAD_INPUT_FACE (test helper for unattended overlay checks): steer the scripted pad toward the nearest other biped.
static float g_hb_tgt[3], g_hb_cam[3], g_hb_fwd[3]; static volatile int g_hb_tgt_ok;
int hb_face_target(float*rx,float*ry){ if(!g_hb_tgt_ok) return 0;
    float d[3]={g_hb_tgt[0]-g_hb_cam[0],g_hb_tgt[1]-g_hb_cam[1],g_hb_tgt[2]-g_hb_cam[2]}; float h=sqrtf(d[0]*d[0]+d[1]*d[1]); if(h<0.01f) return 0;
    float yaw_err=atan2f(d[1],d[0])-atan2f(g_hb_fwd[1],g_hb_fwd[0]); while(yaw_err>3.14159f) yaw_err-=6.28318f; while(yaw_err<-3.14159f) yaw_err+=6.28318f;
    float fh=sqrtf(g_hb_fwd[0]*g_hb_fwd[0]+g_hb_fwd[1]*g_hb_fwd[1]); float pitch_err=atan2f(d[2],h)-atan2f(g_hb_fwd[2],fh);
    float k=getenv("PELOAD_INPUT_FACE_GAIN")?atof(getenv("PELOAD_INPUT_FACE_GAIN")):1.5f, ps=getenv("PELOAD_INPUT_FACE_PITCH")?atof(getenv("PELOAD_INPUT_FACE_PITCH")):-1.0f;
    float x=-yaw_err*k, y=ps*pitch_err*k; *rx=x>0.6f?0.6f:x<-0.6f?-0.6f:x; *ry=y>0.6f?0.6f:y<-0.6f?-0.6f:y; return 1; }
static uint8_t g_hb_tab[8192*0x18]; static int g_hb_hi;   // last object-table snapshot (for handle -> datum lookups)
static uint64_t hb_handle_datum(uint32_t h){ int i=h&0xFFFF; if(h==0xFFFFFFFFu || i>=g_hb_hi) return 0; const uint8_t*e=g_hb_tab+i*0x18;
    if(*(const uint16_t*)e!=(uint16_t)(h>>16)) return 0; return *(const uint64_t*)(e+0x10); }
static int hb_collect(HbObj*out,int max){
    static uint64_t arr; static double tarr; double now=mono_s(); if(!arr || now-tarr>2.0){ arr=hb_object_array(); tarr=now; } if(!arr) return 0;
    uint8_t hdr[0x60]; if(!hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440){ arr=0; return 0; }
    int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48);
    if(esz!=0x18 || hi<=0 || hi>maxc || hi>8192) return 0;
    if(!hb_read(data,g_hb_tab,(size_t)hi*0x18)) return 0; g_hb_hi=hi;
    int fh=hb_feat(HBF_HITBOX), ft=hb_feat(HBF_TRACER), fi=hb_feat(HBF_INFO), fw=hb_feat(HBF_ITEMS);
    int n=0;
    for(int i=0;i<hi && n<max;i++){ const uint8_t*e=g_hb_tab+i*0x18; if(!*(const uint16_t*)e) continue; int t=e[3];
        int unit = t==0||t==1||t==12;
        int want = (unit && (fh||fi)) || (t==5 && (ft || (fh&&g_hb_all))) || ((t==2||t==3) && (fw || (fh&&g_hb_all))); if(!want) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); static uint8_t dh[0x380]; size_t rd=unit?0x380:0x140; if(!p || !hb_read(p,dh,rd)) continue;
        const float*od=(const float*)(dh+0x1c);
        HbObj*o=&out[n]; memset(o,0,sizeof *o); o->c[0]=od[0]; o->c[1]=od[1]; o->c[2]=od[2]; o->r=od[3]; memcpy(o->o,dh+0x50,12); memcpy(o->vel,dh+0x74,12);
        o->type=t; o->handle=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; o->p=p; o->def=*(const uint32_t*)dh; o->parent=*(const uint32_t*)(dh+0x10);
        o->nmsz=*(const uint16_t*)(dh+0x136); o->nmoff=*(const uint16_t*)(dh+0x138);
        { int16_t rs=*(const int16_t*)(dh+0x13a); if((*(const uint32_t*)(dh+4)>>11)&1) rs-=0x20; o->rcount=rs>0?rs/10:0; o->roff=*(const uint16_t*)(dh+0x13c); }
        o->dead=(*(const uint32_t*)(dh+0x110)>>2)&1; o->health=o->dead?0:*(const float*)(dh+0xF4); o->shield=o->dead?0:*(const float*)(dh+0xF8);
        if(unit){ o->team=*(const int32_t*)(dh+0x174); o->seat=*(const int16_t*)(dh+0x24E); o->wslot=*(const int8_t*)(dh+0x262);
            memcpy(o->weapons,dh+0x268,16); memcpy(o->gren,dh+0x294,4); o->camo=*(const float*)(dh+0x360); }
        if(!(o->r>0.001f && o->r<50.f) || o->c[0]!=o->c[0]) continue; n++; }
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
typedef struct { char path[1024]; int fd; uint32_t tagoff; uint64_t vbase, fsize; int ntags; uint32_t*tp; uint16_t*salt; uint32_t*grp; char*names; int32_t*noff; } HbMap;
static HbMap g_hbm[6]; static int g_hbm_n;
static char g_hb_mappaths[8][1024]; static int g_hb_nmappaths; static pthread_mutex_t g_hb_mapmx=PTHREAD_MUTEX_INITIALIZER;
static void hb_note_map(const char*p){     // CreateFileW: remember the scenario maps the game opens (newest first)
    if(!p) return; size_t n=strlen(p); if(n<5 || strcasecmp(p+n-4,".map")) return;   // always: F2 can enable the overlay later
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
    { // tag names: header +0x20 count, +0x24 names offset, +0x28 names size, +0x2C index offset; file offset = value + debug mask (+0x4CC[0])
      int32_t cnt=*(int32_t*)(h+0x20), sz=*(int32_t*)(h+0x28); uint32_t dm=*(uint32_t*)(h+0x4CC), no=(uint32_t)(*(uint32_t*)(h+0x24)+dm), io=(uint32_t)(*(uint32_t*)(h+0x2C)+dm);
      if(cnt==m->ntags && sz>0 && sz<(64<<20)){ m->names=malloc((size_t)sz+1); m->noff=malloc((size_t)cnt*4);
          if(!m->names||!m->noff || pread(m->fd,m->names,(size_t)sz,(off_t)no)!=sz || pread(m->fd,m->noff,(size_t)cnt*4,(off_t)io)!=cnt*4){ free(m->names); free(m->noff); m->names=0; m->noff=0; }
          else m->names[sz]=0; } }
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
typedef struct { uint32_t def; int ok, nseg, nreg, maxnode; int16_t*node; uint8_t*reg, *perm; float*pts;
    int nhreg; int8_t hcreg[32], hnperm[32], hcperm[32][64]; } HbGeom;
static HbGeom g_hbg[96]; static int g_hbg_n;
static int hbm_block(HbMap*m,const uint8_t*d,int off,int esz,uint8_t**out,int maxn){   // tag block {count, p32} -> malloc'd elements
    int32_t c=*(const int32_t*)(d+off); uint32_t p=*(const uint32_t*)(d+off+4); *out=0; if(c<=0 || c>maxn || !p) return 0;
    uint8_t*b=malloc((size_t)c*esz); if(!b || !hbm_read(m,(uint64_t)p<<2,b,(size_t)c*esz)){ free(b); return 0; } *out=b; return c; }
static void hbg_build(HbGeom*g,HbMap*m,int ti){
    uint8_t ob[0x60], hl[0xB0], co[0x44]; int cap=0;
    if(!hbm_read(m,(uint64_t)m->tp[ti]<<2,ob,sizeof ob)) return; uint32_t hd=*(uint32_t*)(ob+0x34+12); if(hd==0xFFFFFFFF || (hd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[hd&0xFFFF]<<2,hl,sizeof hl)) return; uint32_t cd=*(uint32_t*)(hl+0x10+12); if(cd==0xFFFFFFFF || (cd&0xFFFF)>=(uint32_t)m->ntags) return;
    if(!hbm_read(m,(uint64_t)m->tp[cd&0xFFFF]<<2,co,sizeof co)) return;
    uint8_t*regs; int nr=hbm_block(m,co,0x20,0x10,&regs,64);
    for(int r=0;r<nr && r<255;r++){ uint8_t*perms; int np=hbm_block(m,regs+r*0x10,4,0x28,&perms,64);
        for(int pi=0;pi<np;pi++){ uint8_t*bsps; int nb=hbm_block(m,perms+pi*0x28,4,0x64,&bsps,256);
            for(int b=0;b<nb;b++){ const uint8_t*bp=bsps+b*0x64; int16_t node=*(const int16_t*)bp; uint8_t*ed,*vx;
                int ne=hbm_block(m,bp,0x4C,0xC,&ed,4096), nvx=hbm_block(m,bp,0x58,0x10,&vx,4096);
                for(int e=0;e<ne;e++){ int16_t a=*(int16_t*)(ed+e*12), z=*(int16_t*)(ed+e*12+2); if(a<0||z<0||a>=nvx||z>=nvx) continue;
                    if(g->nseg>=cap){ cap=cap?cap*2:256; g->node=realloc(g->node,cap*2); g->reg=realloc(g->reg,cap); g->perm=realloc(g->perm,cap); g->pts=realloc(g->pts,cap*6*4); }
                    g->node[g->nseg]=node; g->reg[g->nseg]=(uint8_t)r; g->perm[g->nseg]=(uint8_t)pi; memcpy(g->pts+g->nseg*6,vx+a*16,12); memcpy(g->pts+g->nseg*6+3,vx+z*16,12); g->nseg++;
                    if(node>g->maxnode) g->maxnode=node; }
                free(ed); free(vx); }
            free(bsps); }
        free(perms); if(r+1>g->nreg) g->nreg=r+1; }
    free(regs);
    uint8_t*hr; int nh=hbm_block(m,hl,0xA0,0x14,&hr,32); g->nhreg=nh;   // model collision regions -> collision region / permutations
    for(int i=0;i<nh;i++){ g->hcreg[i]=(int8_t)hr[i*0x14+4]; uint8_t*hp; int np=hbm_block(m,hr+i*0x14,8,8,&hp,64); g->hnperm[i]=(int8_t)np;
        for(int q=0;q<np;q++) g->hcperm[i][q]=(int8_t)hp[q*8+5]; free(hp); }
    free(hr); g->ok=g->nseg>0; }
static const char* hb_tag_name_early(uint32_t def,HbMap*m,int ti){ (void)def; if(!m->names||ti>=m->ntags) return "?"; const char*f=m->names+m->noff[ti]; const char*b=strrchr(f,'\\'); return b?b+1:f; }
// The collision geometry of an object definition (datum index), from whichever recently opened map holds that tag.
static HbGeom* hb_geom(uint32_t def){
    for(int i=0;i<g_hbg_n;i++) if(g_hbg[i].def==def) return g_hbg[i].ok?&g_hbg[i]:0;
    if(g_hbg_n>=96) return 0; HbGeom*g=&g_hbg[g_hbg_n++]; memset(g,0,sizeof *g); g->def=def;
    char paths[8][1024]; int np; pthread_mutex_lock(&g_hb_mapmx); np=g_hb_nmappaths; memcpy(paths,g_hb_mappaths,sizeof paths); pthread_mutex_unlock(&g_hb_mapmx);
    for(int k=0;k<np && !g->ok;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        if(m->grp[ti]!='bipd' && m->grp[ti]!='vehi' && m->grp[ti]!='gint') continue; hbg_build(g,m,ti);
        char msg[300]; int kk=snprintf(msg,sizeof msg,"  [hitbox] definition %08x (%s): %d collision edges in %d regions (%d model regions), nodes 0-%d\n",def,hb_tag_name_early(def,m,ti),g->nseg,g->nreg,g->nhreg,g->maxnode); (void)!write(2,msg,kk); }
    return g->ok?g:0; }
// Append one object's posed collision edges as clip-space line vertices; returns vertices added (0 = no geometry).
// Why objects fall back to spheres: [0] drawn as hitboxes, [1] no collision geometry for the definition, [2] node
// block missing/too small, [3] node matrices unreadable or not set up (scale != 1). Logged with PELOAD_HITBOXES_LOG.
static unsigned long g_hbx_stat[4];
static void hbx_why(const HbObj*o,int why){ g_hbx_stat[why]++; static uint32_t seen[256]; static int ns; uint32_t key=o->def^((uint32_t)why<<30);
    for(int i=0;i<ns;i++) if(seen[i]==key) return; if(ns<256) seen[ns++]=key;
    static const char*const nm[4]={"","no collision geometry","node block missing","node matrices not set up"};
    char m[200]; int k=snprintf(m,sizeof m,"  [hitbox] type %d definition %08x drawn as a sphere: %s (nodes %u B at +0x%x)\n",o->type,o->def,nm[why],o->nmsz,o->nmoff); (void)!write(2,m,k); }
static const char* hb_tag_name(uint32_t def);
static int hb_emit_hitbox(const HbObj*o,const float*vp,float*verts,int nv,int cap){
    HbGeom*g=hb_geom(o->def); if(!g){ hbx_why(o,1); return 0; }
    int nn=o->nmsz/52; if(o->nmsz%52 || nn<=g->maxnode || nn>128 || o->nmoff<0x100){ hbx_why(o,2); return 0; }
    static float mt[128*13]; if(!hb_read(o->p+o->nmoff,mt,(size_t)nn*52) || fabsf(mt[0]-1.0f)>0.1f){ hbx_why(o,3); return 0; }
    g_hbx_stat[0]++;
    uint8_t sel[256]; memset(sel,0,sizeof sel);   // chosen collision permutation per collision region (default 0)
    if(g->nhreg>0 && o->rcount>0 && o->rcount<=32){ uint8_t pb[32]; if(hb_read(o->p+o->roff+(uint64_t)o->rcount*8,pb,(size_t)o->rcount)){
        for(int i=0;i<g->nhreg && i<o->rcount;i++){ int pi=pb[i]; if(pi>=g->hnperm[i]) pi=0; int cr=g->hcreg[i], cp=g->hnperm[i]?g->hcperm[i][pi]:0; if(cr>=0 && cr<256 && cp>=0) sel[cr]=(uint8_t)cp; }
        { static uint64_t seen[128]; static int ns; uint64_t key=o->def; for(int i=0;i<o->rcount&&i<8;i++) key=key*131+pb[i]; int f=0; for(int q=0;q<ns;q++) if(seen[q]==key) f=1;
          if(!f && ns<128){ seen[ns++]=key; char m[300]; int k=snprintf(m,sizeof m,"  [hitbox] %s %08x: %d model regions, permutation bytes",hb_tag_name(o->def),o->def,o->rcount);
              for(int i=0;i<o->rcount&&i<16;i++) k+=snprintf(m+k,sizeof m-k," %d",pb[i]); k+=snprintf(m+k,sizeof m-k," -> collision permutations");
              for(int r=0;r<g->nreg&&r<16;r++) k+=snprintf(m+k,sizeof m-k," r%d:%d",r,sel[r]); m[k++]='\n'; (void)!write(2,m,k); } } } }
    int head=-1;   // bipeds: the region whose nodes sit highest is the head
    if(o->type==0 && g->nreg>=2){ float zs[256]={0}; int zc[256]={0}; for(int s=0;s<g->nseg;s++){ if(g->perm[s]!=sel[g->reg[s]]) continue; zs[g->reg[s]]+=mt[g->node[s]*13+12]; zc[g->reg[s]]++; }
        float best=-1e30f; for(int r=0;r<g->nreg;r++) if(zc[r] && zs[r]/zc[r]>best){ best=zs[r]/zc[r]; head=r; } }
    // enemies red with a yellow head, teammates blue with a light-blue head (relative to the local player's team)
    static const float cbody[3]={1,0.15f,0.15f}, chead[3]={1,0.95f,0.1f}, fbody[3]={0.25f,0.5f,1}, fhead[3]={0.6f,0.95f,1}, cveh[3]={0.3f,0.75f,1};
    int friend_=hb_is_friend(o); const float*cb=friend_?fbody:cbody, *ch=friend_?fhead:chead;
    int added=0;
    for(int s=0;s<g->nseg && nv+added+2<=cap;s++){ if(g->perm[s]!=sel[g->reg[s]]) continue;
        const float*M=mt+g->node[s]*13, *P=g->pts+s*6; const float*col=o->type==0?(g->reg[s]==head?ch:cb):cveh;
        float q[2][2]; int okk=1;
        for(int e=0;e<2;e++){ const float*v=P+e*3; float w[3];
            for(int a=0;a<3;a++) w[a]=M[10+a]+M[0]*(v[0]*M[1+a]+v[1]*M[4+a]+v[2]*M[7+a]);   // pos + scale*(x*fwd + y*left + z*up)
            float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
            if(cw<0.05f){ okk=0; break; } q[e][0]=cx/cw; q[e][1]=cy/cw; }
        if(!okk) continue; float*d=verts+(nv+added)*5;
        d[0]=q[0][0]; d[1]=q[0][1]; d[2]=col[0]; d[3]=col[1]; d[4]=col[2]; d[5]=q[1][0]; d[6]=q[1][1]; d[7]=col[0]; d[8]=col[1]; d[9]=col[2]; added+=2; }
    return added?added:-1; }   // -1: posed but entirely behind the camera
// Short display name of an object definition (last path component of its tag name, '_' -> ' ', upper case).
static const char* hb_tag_name_l(uint32_t def);
static const char* hb_tag_name(uint32_t def){   // also called by the stats thread (stats.c)
    static pthread_mutex_t mx=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mx); const char*r=hb_tag_name_l(def); pthread_mutex_unlock(&mx); return r; }
static const char* hb_tag_name_l(uint32_t def){
    static struct { uint32_t def; char nm[32]; } cache[256]; static int nc;
    for(int i=0;i<nc;i++) if(cache[i].def==def) return cache[i].nm;
    char out[32]="?"; char paths[8][1024]; int np; pthread_mutex_lock(&g_hb_mapmx); np=g_hb_nmappaths; memcpy(paths,g_hb_mappaths,sizeof paths); pthread_mutex_unlock(&g_hb_mapmx);
    for(int k=0;k<np;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || !m->names || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        const char*full=m->names+m->noff[ti]; const char*b=strrchr(full,'\\'); b=b?b+1:full; int j=0;
        for(; b[j] && j<31; j++) out[j]=(char)(b[j]=='_'?' ':(b[j]>='a'&&b[j]<='z'?b[j]-32:b[j])); out[j]=0; break; }
    if(nc<256){ cache[nc].def=def; snprintf(cache[nc].nm,sizeof cache[nc].nm,"%s",out); return cache[nc++].nm; }
    static char last[32]; snprintf(last,sizeof last,"%s",out); return last; }
// Stroke font: each glyph is a list of segments "x1y1x2y2" on a 4x6 grid (y up), drawn as GL lines with the overlay.
static const char* hb_glyph(char c){
    switch(c){
    case '0': return "0040404646060600";  case '1': return "202626151030";  case '2': return "06464643430303000040";
    case '3': return "0646464040000343";  case '4': return "060303434640";  case '5': return "46060603034343404000";
    case '6': return "46060600004040434303";  case '7': return "06464620";  case '8': return "00404046460606000343";
    case '9': return "43030306064646404000";  case 'A': return "00040426264444400242";  case 'B': return "000606363645453333034342424131300000";
    case 'C': return "460606000040";  case 'D': return "000606363644444242303000";  case 'E': return "4606060000400333";
    case 'F': return "460606000333";  case 'G': return "46060600004040434323";  case 'H': return "000640460343";
    case 'I': return "064620260040";  case 'J': return "464040000002";  case 'K': return "000603460340";
    case 'L': return "06000040";  case 'M': return "0006062323464640";  case 'N': return "000606404046";
    case 'O': return "0040404646060600";  case 'P': return "0006064646434303";  case 'Q': return "00404046460606002241";
    case 'R': return "00060646464343031340";  case 'S': return "46060603034343404000";  case 'T': return "06462620";
    case 'U': return "060000404046";  case 'V': return "06202046";  case 'W': return "0600002323404046";
    case 'X': return "00460640";  case 'Y': return "062346232320";  case 'Z': return "064646000040";
    case '%': return "004605160616304131404131";  case ':': return "21222425";  case '-': return "0343";  case '.': return "2021";
    case '/': return "0046";  case '+': return "03432125";  case '?': return "05060646464444222221";  case '(': return "3616162020101034";
    default: return ""; } }
static float g_hb_px, g_hb_py;   // NDC size of one font-grid unit
static volatile int g_hb_text_px;   // label glyph height in window pixels (0 = not set yet)
static int hb_text(float*verts,int nv,int cap,float x,float y,const char*s,const float*col){   // x,y = NDC of the text's top-left
    for(; *s; s++, x+=5*g_hb_px){ char ch=*s; if(ch>='a'&&ch<='z') ch-=32; const char*g=hb_glyph(ch);
        for(; g[0]&&g[1]&&g[2]&&g[3] && nv+2<=cap; g+=4){ float*d=verts+nv*5;
            d[0]=x+(g[0]-'0')*g_hb_px; d[1]=y-(6-(g[1]-'0'))*g_hb_py; d[2]=col[0]; d[3]=col[1]; d[4]=col[2];
            d[5]=x+(g[2]-'0')*g_hb_px; d[6]=y-(6-(g[3]-'0'))*g_hb_py; d[7]=col[0]; d[8]=col[1]; d[9]=col[2]; nv+=2; } }
    return nv; }
static int hb_proj(const float*vp,const float*w,float*nx,float*ny){   // world point -> NDC; 0 if behind the camera
    float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
    if(cw<0.05f) return 0; *nx=cx/cw; *ny=cy/cw; return 1; }
static int hb_seg(float*verts,int nv,int cap,const float*vp,const float*a,const float*b,const float*col){
    float ax,ay,bx,by; if(nv+2>cap || !hb_proj(vp,a,&ax,&ay) || !hb_proj(vp,b,&bx,&by)) return nv; float*d=verts+nv*5;
    d[0]=ax; d[1]=ay; d[2]=col[0]; d[3]=col[1]; d[4]=col[2]; d[5]=bx; d[6]=by; d[7]=col[0]; d[8]=col[1]; d[9]=col[2]; return nv+2; }
// Projectile trails: positions seen on recent frames, per projectile handle; kept briefly after the projectile is gone.
typedef struct { uint32_t h; int n; float p[32][3]; double seen; } HbTrail;
static HbTrail g_hb_trails[256];
static void hb_trail_add(uint32_t h,const float*pos,double now){
    HbTrail*t=0,*old=&g_hb_trails[0]; for(int i=0;i<256;i++){ if(g_hb_trails[i].h==h && g_hb_trails[i].n){ t=&g_hb_trails[i]; break; } if(g_hb_trails[i].seen<old->seen) old=&g_hb_trails[i]; }
    if(!t){ t=old; t->h=h; t->n=0; }
    if(t->n){ const float*l=t->p[(t->n-1)&31]; float dx=pos[0]-l[0],dy=pos[1]-l[1],dz=pos[2]-l[2]; if(dx*dx+dy*dy+dz*dz<1e-6f){ t->seen=now; return; } }
    memcpy(t->p[t->n&31],pos,12); t->n++; t->seen=now; }
// ---- bullets: halo3 resolves most bullets inside the tick that fires them, so they never show up in the object
// table between frames. object_delete (halo3+0x342ed8, entry-hooked in peload.c: hbdel_install) sees every one:
// at deletion the datum still holds the impact point and velocity. Recorded here, drawn by the tracer feature.
// Projectile datum (from the bullet datums at deletion, 2026-10-16): +0x1c position (= impact at deletion), +0x74
// velocity, +0x1b0 distance travelled, +0x1dc speed, +0x1e8 start position (where it was fired from).
typedef struct { double t; uint32_t def, handle; float pos[3], vel[3]; uint8_t raw[0x200]; } HbImpact;
// ---- teleport (PELOAD_ALLOW_TELEPORT=1; F7 or PELOAD_TELEPORT_TEST=<seconds after gameplay>) -------------------------
// Moves the local player to the point reflection of its position through the map centre (the mean position of the
// weapons and equipment lying on the map; Halo multiplayer maps are mostly symmetric), turned to face the other way.
// Uses halo3's own teleport (halo3+0x259410: object, position*, forward*, up*, bool -- what the object_teleport script
// functions call), which has to run on the simulation thread: the request is queued here and executed by
// hb_engine_jobs() from QueryPerformanceCounter on that thread (identified as the thread that runs object_delete).
// Meant for matches this client hosts; on someone else's server the host would correct the position anyway.
static pthread_t g_hb_simthread; static volatile int g_hb_simthread_ok;
volatile int g_hb_job_pending;
volatile int g_hb_tp_slot=-1;   // stats page "Teleport next to them": that players-table slot; -1 = F7's map reflection
static const char* hb_tag_name(uint32_t def);
// Teleport next to another player (stats page "Teleport next to them"), the way the Reclaimer client teleports
// (shim/docs/teleport.md): host only, on the engine main thread at main_loop_body (between ticks), inside halo3's
// game-state lock; out of any vehicle first; the destination is beside the other player (right, left, behind, close
// behind) and must be inside the map structure (scenario_location_from_point) at knee and head height.
typedef int  (__attribute__((ms_abi)) *HbLockFn)(void);
typedef void (__attribute__((ms_abi)) *HbUnlockFn)(int*);
static int g_hb_tp_retry;
static void hb_tp_next(uint8_t*h3,uint8_t*tls,uint8_t*data,int hi,uint32_t me,int tps){
    char m[240]; int k;
    uint8_t*parr=*(uint8_t**)(tls+0x10); if(!parr || *(uint32_t*)(parr+0x2c)!=0x64407440) return;
    int32_t es=*(int32_t*)(parr+0x24), ph=*(int32_t*)(parr+0x3c); uint8_t*pd=*(uint8_t**)(parr+0x48); if(!pd || tps>=ph || es<0x30) return;
    uint8_t*rec=pd+(size_t)tps*es; uint32_t tu=*(uint32_t*)(rec+0x28); int ti=tu&0xFFFF;
    if(!*(uint16_t*)rec || tu==0xFFFFFFFFu || ti>=hi || tu==me){ (void)!write(2,"  [teleport] that player has no body right now\n",47); return; }
    uint8_t*te=data+(size_t)ti*0x18; uint8_t*tp=*(uint8_t**)(te+0x10); if(te[3]!=0 || *(uint16_t*)te!=(uint16_t)(tu>>16) || !tp) return;
    uint8_t*me_e=data+(size_t)(me&0xFFFF)*0x18; uint8_t*mp=*(uint8_t**)(me_e+0x10); if(!mp || me_e[3]!=0 || *(uint16_t*)me_e!=(uint16_t)(me>>16)) return;
    if((*(uint32_t*)(mp+0x110)>>2)&1){ (void)!write(2,"  [teleport] you are dead\n",26); return; }
    // in a vehicle: get out first, teleport on a later frame
    if(*(uint32_t*)(mp+0x10)!=0xFFFFFFFFu || *(int16_t*)(mp+0x24E)!=-1){
        if(g_hb_tp_retry++<3){ ((void(__attribute__((ms_abi))*)(uint32_t,int))(h3+0x3a3c4c))(me,0); g_hb_tp_slot=tps; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE);
            (void)!write(2,"  [teleport] leaving the vehicle first\n",39); }
        else { g_hb_tp_retry=0; (void)!write(2,"  [teleport] could not leave the vehicle\n",41); }
        return; }
    g_hb_tp_retry=0;
    const float*to=(const float*)(tp+0x50), *tf=(const float*)(tp+0x5c);
    float fx=tf[0], fy=tf[1], fl=sqrtf(fx*fx+fy*fy); if(fl<1e-4f){ fx=1; fy=0; } else { fx/=fl; fy/=fl; }
    const float cand[4][2]={{fy,-fx},{-fy,fx},{-fx,-fy},{-0.35f*fx,-0.35f*fy}};   // right, left, behind, close behind
    static const char*const cn[4]={"right","left","behind","close behind"};
    float pos[3]; int ok=-1;
    for(int c=0;c<4 && ok<0;c++){ pos[0]=to[0]+cand[c][0]; pos[1]=to[1]+cand[c][1]; pos[2]=to[2]+0.1f;
        if(fabsf(pos[0])>30000||fabsf(pos[1])>30000||fabsf(pos[2])>30000) continue;   // past 32768 the host deletes the unit
        uint16_t loc=0xFFFF; float p1[3]={pos[0],pos[1],pos[2]+0.3f}, p2[3]={pos[0],pos[1],pos[2]+1.2f};
        ((void(__attribute__((ms_abi))*)(uint16_t*,const float*))(h3+0x9bc78))(&loc,p1); if(loc==0xFFFF) continue;
        loc=0xFFFF; ((void(__attribute__((ms_abi))*)(uint16_t*,const float*))(h3+0x9bc78))(&loc,p2); if(loc==0xFFFF) continue;
        ok=c; }
    if(ok<0){ k=snprintf(m,sizeof m,"  [teleport] no open spot next to slot %d at (%.2f %.2f %.2f)\n",tps,to[0],to[1],to[2]); (void)!write(2,m,k); return; }
    float fwd[3]={to[0]-pos[0], to[1]-pos[1], 0}, l=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]); if(l>1e-4f){ fwd[0]/=l; fwd[1]/=l; } else { fwd[0]=fx; fwd[1]=fy; }
    float up[3]={0,0,1};
    k=snprintf(m,sizeof m,"  [teleport] next to slot %d at (%.2f %.2f %.2f): %s -> (%.2f %.2f %.2f)\n",tps,to[0],to[1],to[2],cn[ok],pos[0],pos[1],pos[2]); (void)!write(2,m,k);
    ((void(__attribute__((ms_abi))*)(uint32_t,const float*,const float*,const float*,uint8_t))(h3+0x259410))(me,pos,fwd,up,1);
    k=snprintf(m,sizeof m,"  [teleport] object %08x moved\n",me); (void)!write(2,m,k); }
static void hb_engine_jobs_locked(uint8_t*h3,void**tarr,uint32_t idx,uint8_t*arr);
static void hb_engine_jobs(void){
    // the simulation thread: its own TLS block holds the live object table (worker threads have no game state there)
    static int why_logged; uint8_t*h3=hb_halo3_base(); if(!h3) return; uint32_t idx=*(volatile uint32_t*)(h3+0xa39f9c); if(idx>=128) return;
    void**tarr; __asm__ volatile("movq %%gs:0x58,%0":"=r"(tarr)); if(!tarr || !tarr[idx]){ if(!(why_logged&1)){ why_logged|=1; (void)!write(2,"  [teleport] safe point reached on a thread without halo3 TLS\n",62); } return; }
    uint8_t*arr=*(uint8_t**)((uint8_t*)tarr[idx]+0x38); if(!arr || *(uint32_t*)(arr+0x2c)!=0x64407440 || memcmp(arr,"object",7)){ if(!(why_logged&2)){ why_logged|=2; (void)!write(2,"  [teleport] safe point thread has no object table\n",51); } return; }
    if(!__atomic_exchange_n(&g_hb_job_pending,0,__ATOMIC_ACQ_REL)) return;
    static const uint8_t pro[16]={0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    if(memcmp(h3+0x259410,pro,16)){ (void)!write(2,"  [teleport] halo3 teleport function not recognised: skipped\n",60); return; }
    // host only: on a client the host's update snaps the body back (and the old mid-tick client call crashed)
    { uint8_t*gg=*(uint8_t**)((uint8_t*)tarr[idx]+0x48); int st=gg?gg[0x11]:-1;
      if(st!=5 && st!=1){ char m[140]; int k=snprintf(m,sizeof m,"  [teleport] refused: this game is hosted elsewhere (simulation type %d); only the host can move players\n",st); (void)!write(2,m,k); g_hb_tp_slot=-1; return; } }
    // halo3's game-state lock (what it holds around every tick)
    if(!*(uint64_t*)(h3+0x8f4028) || *(int32_t*)((uint8_t*)tarr[idx]+0x18)<0 || *(int32_t*)((uint8_t*)tarr[idx]+0x18)>=32){ (void)!write(2,"  [teleport] engine not ready\n",30); return; }
    int tok=((HbLockFn)(h3+0x12c948))();
    hb_engine_jobs_locked(h3,tarr,idx,arr);
    ((HbUnlockFn)(h3+0x12c9a0))(&tok); }
static void hb_engine_jobs_locked(uint8_t*h3,void**tarr,uint32_t idx,uint8_t*arr){
    float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); if((s0&1) || g_hb_vpseq!=s0 || (!vp[28]&&!vp[29]&&!vp[30])){ (void)!write(2,"  [teleport] no camera yet\n",27); return; }
    // walk the object table directly (we are the thread that owns it): our body = the biped containing the camera;
    // map centre = mean of the weapons/equipment lying on the map
    uint8_t*data=*(uint8_t**)(arr+0x48); int hi=*(int32_t*)(arr+0x3c); if(!data || hi<=0 || hi>8192) return;
    uint32_t me=0xFFFFFFFFu; float mo[3]={0}; double cx=0,cy=0; int ni=0;
    for(int i=0;i<hi;i++){ uint8_t*e=data+(size_t)i*0x18; if(!*(uint16_t*)e) continue; uint8_t*p=*(uint8_t**)(e+0x10); if(!p) continue; int t=e[3];
        const float*c=(const float*)(p+0x1c), r=c[3]; const float*o=(const float*)(p+0x50);
        if(t==0){ float dx=c[0]-vp[28],dy=c[1]-vp[29],dz=c[2]-vp[30]; if(dx*dx+dy*dy+dz*dz<(r+0.3f)*(r+0.3f)){ me=((uint32_t)*(uint16_t*)e<<16)|(uint32_t)i; memcpy(mo,o,12); } }
        else if((t==2||t==3) && *(uint32_t*)(p+0x10)==0xFFFFFFFFu){ cx+=o[0]; cy+=o[1]; ni++; } }
    { // the local player: halo3's player mapping (TLS +0x110: output user 0 -> player handle at +0xB8 -> player +0x28
      // unit), right in third person and vehicles; else the stats thread's pick
      uint8_t*tls=(uint8_t*)tarr[idx]; uint8_t*pm=*(uint8_t**)(tls+0x110), *parr=*(uint8_t**)(tls+0x10); uint32_t lu=0xFFFFFFFFu;
      if(pm && parr && *(uint32_t*)(parr+0x2c)==0x64407440){ int32_t ph=*(int32_t*)(pm+0xB8), es=*(int32_t*)(parr+0x24), pn=*(int32_t*)(parr+0x3c); uint8_t*pd=*(uint8_t**)(parr+0x48);
          if(ph!=-1 && pd && (ph&0xFFFF)<pn) lu=*(uint32_t*)(pd+(size_t)(ph&0xFFFF)*es+0x28); }
      if(lu==0xFFFFFFFFu) lu=g_hb_local_unit;
      int li=lu&0xFFFF; if(lu!=0xFFFFFFFFu && li<hi){ uint8_t*e=data+(size_t)li*0x18; uint8_t*p=*(uint8_t**)(e+0x10);
          if(e[3]==0 && *(uint16_t*)e==(uint16_t)(lu>>16) && p){ me=lu; memcpy(mo,p+0x50,12); } } }
    if(me==0xFFFFFFFFu){ (void)!write(2,"  [teleport] no body at the camera (dead or in a vehicle?)\n",59); return; }
    int tps=g_hb_tp_slot; g_hb_tp_slot=-1;
    if(tps>=0){ hb_tp_next(h3,(uint8_t*)tarr[idx],data,hi,me,tps); return; }
    if(ni<4){ (void)!write(2,"  [teleport] too few map items to find the centre\n",50); return; }
    cx/=ni; cy/=ni; float rx=(float)(2*cx-mo[0]), ry=(float)(2*cy-mo[1]);
    // the reflected point itself can be inside geometry (maps are mirror-, not point-symmetric; a first try landed
    // below the far base's floor and crashed in the physics update): stand on the map item nearest to it instead
    float bd=1e30f, pos[3]={0}; uint32_t bdef=0;
    for(int i=0;i<hi;i++){ uint8_t*e=data+(size_t)i*0x18; if(!*(uint16_t*)e || !(e[3]==2||e[3]==3)) continue; uint8_t*p=*(uint8_t**)(e+0x10); if(!p || *(uint32_t*)(p+0x10)!=0xFFFFFFFFu) continue;
        const float*o=(const float*)(p+0x50); float dx=o[0]-rx, dy=o[1]-ry, d=dx*dx+dy*dy; if(d<bd){ bd=d; pos[0]=o[0]; pos[1]=o[1]; pos[2]=o[2]+0.5f; bdef=*(uint32_t*)p; } }
    float fwd[3]={-vp[12],-vp[13],0}, fl=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]); if(fl>1e-4f){ fwd[0]/=fl; fwd[1]/=fl; } else { fwd[0]=1; fwd[1]=0; }
    float up[3]={0,0,1};
    char m[300]; int k=snprintf(m,sizeof m,"  [teleport] map centre (%.2f %.2f) from %d items; from (%.2f %.2f %.2f), reflected (%.2f %.2f) -> on %s at (%.2f %.2f %.2f)\n",cx,cy,ni,mo[0],mo[1],mo[2],rx,ry,hb_tag_name(bdef),pos[0],pos[1],pos[2]); (void)!write(2,m,k);
    ((void(__attribute__((ms_abi))*)(uint32_t,const float*,const float*,const float*,uint8_t))(h3+0x259410))(me,pos,fwd,up,1);
    k=snprintf(m,sizeof m,"  [teleport] object %08x moved\n",me); (void)!write(2,m,k); }
// PELOAD_TELEPORT_TEST=<seconds>: one teleport that long after gameplay starts (checked once per frame at Present)
static void hb_teleport_tick(void){ static int fired, on=-1; if(on<0) on=getenv("PELOAD_TELEPORT_TEST")&&getenv("PELOAD_ALLOW_TELEPORT")?1:0; extern double g_gameplay_t0;
    if(on && !fired && g_gameplay_t0 && mono_s()-g_gameplay_t0>atof(getenv("PELOAD_TELEPORT_TEST"))){ fired=1; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); } }
static HbImpact g_hbimp[256]; static volatile unsigned g_hbimp_n;
// Every projectile's end, for the live stats' shots fired / hits (stats.c reads the ring 10 times a second): who fired
// it (datum +0xC0, the owner unit) and the biped it ended inside -- an estimated hit: the impact point lies within
// another biped's bounding sphere (centre +0x1c, radius +0x28). Bullets live less than a tick, so only this hook sees
// them all.
typedef struct { uint32_t owner, victim, def; } HbShot;
static HbShot g_hbshot[8192]; static volatile unsigned g_hbshot_n; volatile int g_hb_delhook;
static int stats_on(void);
void __attribute__((ms_abi)) hb_on_delete(uint32_t handle){
    if(!g_hb_simthread_ok){ g_hb_simthread=pthread_self(); g_hb_simthread_ok=1; }   // object_delete runs on the simulation thread
    if(handle==0xFFFFFFFFu) return; static uint32_t idx=0xFFFFFFFFu; uint8_t*h3=hb_halo3_base(); if(!h3) return;
    if(idx==0xFFFFFFFFu) idx=*(volatile uint32_t*)(h3+0xa39f9c); if(idx>=128) return;
    void**tarr; __asm__ volatile("movq %%gs:0x58,%0":"=r"(tarr)); if(!tarr || !tarr[idx]) return;   // the engine thread's own TLS
    uint8_t*arr=*(uint8_t**)((uint8_t*)tarr[idx]+0x38); if(!arr || *(uint32_t*)(arr+0x2c)!=0x64407440) return;
    uint8_t*data=*(uint8_t**)(arr+0x48); int i=handle&0xFFFF; if(!data || i>=*(int32_t*)(arr+0x20)) return;
    uint8_t*e=data+(size_t)i*0x18; if(e[3]!=5 || *(uint16_t*)e!=(uint16_t)(handle>>16)) return;   // projectiles only
    uint8_t*p=*(uint8_t**)(e+0x10); if(!p) return;
    if(stats_on()){ uint32_t own=*(uint32_t*)(p+0xC0), vic=0xFFFFFFFFu; const float*ip=(const float*)(p+0x1c); int hi=*(int32_t*)(arr+0x3c);
        if(own!=0xFFFFFFFFu && hi>0 && hi<=65536) for(int j=0;j<hi;j++){ uint8_t*b=data+(size_t)j*0x18; if(b[3]!=0 || !*(uint16_t*)b) continue;
            uint32_t bh=((uint32_t)*(uint16_t*)b<<16)|(uint32_t)j; if(bh==own) continue; uint8_t*bp=*(uint8_t**)(b+0x10); if(!bp) continue;
            const float*c=(const float*)(bp+0x1c); float r=c[3]+0.1f, dx=ip[0]-c[0], dy=ip[1]-c[1], dz=ip[2]-c[2];
            if(r>0 && r<5 && dx*dx+dy*dy+dz*dz<=r*r){ vic=bh; break; } }
        unsigned q=__atomic_load_n(&g_hbshot_n,__ATOMIC_RELAXED)&8191; g_hbshot[q].owner=own; g_hbshot[q].victim=vic; g_hbshot[q].def=*(uint32_t*)p;
        __atomic_store_n(&g_hbshot_n,g_hbshot_n+1,__ATOMIC_RELEASE); }   // one writer: the simulation thread
    unsigned k=__atomic_fetch_add(&g_hbimp_n,1,__ATOMIC_RELAXED)&255; HbImpact*r=&g_hbimp[k];
    r->def=*(uint32_t*)p; r->handle=handle; memcpy(r->pos,p+0x1c,12); memcpy(r->vel,p+0x74,12); memcpy(r->raw,p,sizeof r->raw);
    { double tv=mono_s(); __atomic_store(&r->t,&tv,__ATOMIC_RELEASE); }
    static int dumped; const char*dd=getenv("PELOAD_HB_DELDUMP"); if(dd && dumped<atoi(dd)){ dumped++; char m[700];
        for(int o=0;o<0x200;o+=0x40){ int q=snprintf(m,sizeof m,"  [hbdel] %08x +%03x:",r->def,o);
            for(int j=0;j<0x40;j+=4){ float f=*(float*)(p+o+j); uint32_t u=*(uint32_t*)(p+o+j); q+= (f>-100000.f&&f<100000.f&&(f>0.0005f||f<-0.0005f)) ? snprintf(m+q,sizeof m-q," %9.3f",f) : snprintf(m+q,sizeof m-q," %9x",u); }
            m[q++]='\n'; (void)!write(2,m,q); } } }
static const float g_hb_teamcol[8][3]={{1,0.3f,0.25f},{0.35f,0.6f,1},{0.35f,1,0.35f},{1,0.6f,0.15f},{0.75f,0.4f,1},{1,0.9f,0.2f},{0.75f,0.55f,0.35f},{1,0.55f,0.8f}};
static unsigned g_hb_prog, g_hb_vao, g_hb_vbo;
// ---- visible only (F9 / PELOAD_HB_VISIBLE / the stats page) -------------------------------------------------------
// Each frame (render thread, at Present: hb_vis_tick) the frame's depth buffer -- the one the camera draw used,
// g_hb_dsv -- is blitted down to 480x270 and read back through a ring of three pixel-pack buffers, so the CPU reads a
// copy two frames old and never waits for the GPU. An object counts as visible when at its centre, near its top or near
// its bottom the scene is not nearer than the object's front (its centre moved toward the camera by radius + 0.3 units).
static D3DObj* g_hb_dsv;   // the depth buffer of the frame's camera draw (hb_note_dsv)
enum { HBV_W=480, HBV_H=270 };
static float g_hbv_depth[HBV_W*HBV_H], g_hbv_vp[16]; static volatile unsigned g_hbv_seq; static volatile int g_hbv_ok;
static void hb_vis_tick(void){
    if(!hb_overlay_on() || !hb_feat(HBF_VISIBLE)){ g_hbv_ok=0; return; }
    D3DObj*ds=g_hb_dsv; if(!ds || !ds->gltex || !ds->gldsatt || !G.BlitFramebuffer || !G.MapBufferRange || !G.ReadPixels) return;
    static unsigned fbo, tex, pbo[3]; static int idx, filled[3]; static float vps[3][16];
    if(!fbo){ TEXCACHE_INVALIDATE(); G.GenTextures(1,&tex); G.BindTexture(GLE_TEX2D,tex);
        G.TexImage2D(GLE_TEX2D,0,0x8CAD/*DEPTH32F_STENCIL8*/,HBV_W,HBV_H,0,0x84F9/*DEPTH_STENCIL*/,0x8DAD/*FLOAT_32_UNSIGNED_INT_24_8_REV*/,0);
        G.TexParameteri(GLE_TEX2D,0x2801,0x2600); G.TexParameteri(GLE_TEX2D,0x2800,0x2600);
        G.GenFramebuffers(1,&fbo); G.BindFramebuffer(GLE_FBO,fbo); G.FramebufferTexture2D(GLE_FBO,0x821A/*DEPTH_STENCIL_ATTACHMENT*/,GLE_TEX2D,tex,0);
        if(G.DrawBuffer) G.DrawBuffer(0); if(G.ReadBuffer) G.ReadBuffer(0);
        G.GenBuffers(3,pbo); for(int i=0;i<3;i++){ G.BindBuffer(0x88EB/*PIXEL_PACK*/,pbo[i]); G.BufferData(0x88EB,HBV_W*HBV_H*4,0,0x88E1/*STREAM_READ*/); }
        G.BindBuffer(0x88EB,0); G.BindFramebuffer(GLE_FBO,0); }
    float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); if((s0&1) || g_hb_vpseq!=s0) return;
    unsigned src=d3dgl_fbo_for(0,0,ds);
    G.BindFramebuffer(0x8CA8/*READ*/,src); G.BindFramebuffer(0x8CA9/*DRAW*/,fbo);
    G.BlitFramebuffer(0,0,(int)ds->w,(int)ds->h,0,0,HBV_W,HBV_H,0x100/*DEPTH_BUFFER_BIT*/,0x2600/*NEAREST*/);
    G.BindFramebuffer(0x8CA8,fbo); G.BindBuffer(0x88EB,pbo[idx]); G.ReadPixels(0,0,HBV_W,HBV_H,0x1902/*DEPTH_COMPONENT*/,0x1406/*FLOAT*/,0);
    memcpy(vps[idx],vp,64); filled[idx]=1;
    int rd=(idx+1)%3;   // the oldest copy: two frames old, long done on the GPU
    if(filled[rd]){ G.BindBuffer(0x88EB,pbo[rd]); const void*m=G.MapBufferRange(0x88EB,0,HBV_W*HBV_H*4,0x1/*MAP_READ*/);
        if(m){ g_hbv_seq++; __atomic_thread_fence(__ATOMIC_SEQ_CST); memcpy(g_hbv_depth,m,sizeof g_hbv_depth); memcpy(g_hbv_vp,vps[rd],64);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); g_hbv_seq++; g_hbv_ok=1; G.UnmapBuffer(0x88EB); } }
    G.BindBuffer(0x88EB,0); G.BindFramebuffer(GLE_FBO,0); idx=(idx+1)%3; }
static int hb_vis(const HbObj*o){
    if(!g_hbv_ok) return 1; unsigned s0=g_hbv_seq; if(s0&1) return 1;
    float vp[16]; memcpy(vp,g_hbv_vp,64); float f[3]={vp[12],vp[13],vp[14]}, fl=sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if(fl<1e-6f) return 1;
    f[0]/=fl; f[1]/=fl; f[2]/=fl; float r=o->r>0.05f&&o->r<20?o->r:0.5f; int any=0, vis=0;
    const float dz[3]={0,0.6f*r,-0.4f*r};
    for(int k=0;k<3 && !vis;k++){ float p[3]={o->c[0],o->c[1],o->c[2]+dz[k]};
        float cx=p[0]*vp[0]+p[1]*vp[1]+p[2]*vp[2]+vp[3], cy=p[0]*vp[4]+p[1]*vp[5]+p[2]*vp[6]+vp[7], cw=p[0]*vp[12]+p[1]*vp[13]+p[2]*vp[14]+vp[15];
        if(cw<0.05f) continue; float nx=cx/cw, ny=cy/cw; if(nx<-1||nx>1||ny<-1||ny>1) continue; any=1;
        float q[3]={p[0]-f[0]*(r+0.3f),p[1]-f[1]*(r+0.3f),p[2]-f[2]*(r+0.3f)};
        float qz=q[0]*vp[8]+q[1]*vp[9]+q[2]*vp[10]+vp[11], qw=q[0]*vp[12]+q[1]*vp[13]+q[2]*vp[14]+vp[15];
        if(qw<0.05f){ vis=1; break; }   // right in front of the camera: nothing can be in between
        float thr=qz/qw; int col=(int)((nx*0.5f+0.5f)*HBV_W), row=(int)((d3dgl_flipy()?(0.5f-ny*0.5f):(ny*0.5f+0.5f))*HBV_H);
        if(col<0) col=0; if(col>=HBV_W) col=HBV_W-1; if(row<0) row=0; if(row>=HBV_H) row=HBV_H-1;
        if(g_hbv_depth[row*HBV_W+col]<=thr) vis=1; }   // reverse-Z: larger = nearer; scene not nearer than our front
    if(g_hbv_seq!=s0) return 1;
    return vis || !any; }   // off screen: leave it to the projection (nothing gets drawn there anyway)
static void hb_draw_overlay(void){
    if(!hb_overlay_on() || !G.DrawArrays || !G.CreateShader) return;
    float vp[32]; unsigned s0=g_hb_vpseq; if(s0&1) return; memcpy(vp,g_hb_vp,sizeof vp); if(g_hb_vpseq!=s0) return;
    if(!vp[0] && !vp[1] && !vp[2]){ static double tn; double now=mono_s(); if(getenv("PELOAD_HITBOXES_LOG") && now-tn>5.0){ tn=now; (void)!write(2,"  [hitbox] no camera matrix captured yet\n",41); } return; }
    if(!g_hb_prog){
        const char*vs="#version 410 core\nlayout(location=0) in vec2 p; layout(location=1) in vec3 c; out vec3 vc; void main(){ vc=c; gl_Position=vec4(p,0.0,1.0); }\n";
        const char*fs="#version 410 core\nin vec3 vc; out vec4 o; void main(){ o=vec4(vc,1.0); }\n";
        unsigned v=G.CreateShader(GLE_VS),f=G.CreateShader(GLE_FS); G.ShaderSource(v,1,&vs,0); G.CompileShader(v); G.ShaderSource(f,1,&fs,0); G.CompileShader(f);
        g_hb_prog=G.CreateProgram(); G.AttachShader(g_hb_prog,v); G.AttachShader(g_hb_prog,f); G.LinkProgram(g_hb_prog); G.DeleteShader(v); G.DeleteShader(f);
        G.GenBuffers(1,&g_hb_vbo); }
    { static void*vctx; void*cur=CGLGetCurrentContext(); if(cur!=vctx){ vctx=cur; G.GenVertexArrays(1,&g_hb_vao); } }   // VAOs are per context
    int fh=hb_feat(HBF_HITBOX), ft=hb_feat(HBF_TRACER), fi=hb_feat(HBF_INFO), fw=hb_feat(HBF_ITEMS), fv=hb_feat(HBF_VISIBLE); double now=mono_s();

    static HbObj objs[1024]; int n=hb_collect(objs,1024);
    if(getenv("PELOAD_HITBOXES_LOG")){ static double tl; if(now-tl>3.0){ tl=now; int cnt[16]={0}; for(int i=0;i<n;i++) if(objs[i].type<16) cnt[objs[i].type]++;
        char m[300]; int k=snprintf(m,sizeof m,"  [hitbox] cam (%.1f %.1f %.1f) objects: bipeds %d vehicles %d weapons %d equipment %d projectiles %d | hitboxes %lu, spheres: no-geometry %lu no-nodes %lu nodes-not-set %lu\n",
            vp[28],vp[29],vp[30],cnt[0],cnt[1],cnt[2],cnt[3],cnt[5],g_hbx_stat[0],g_hbx_stat[1],g_hbx_stat[2],g_hbx_stat[3]); (void)!write(2,m,k); memset(g_hbx_stat,0,sizeof g_hbx_stat); } }
    { float bd=1e30f; int bi=-1; for(int i=0;i<n;i++){ if(objs[i].type) continue; float dx=objs[i].c[0]-vp[28],dy=objs[i].c[1]-vp[29],dz=objs[i].c[2]-vp[30],d=dx*dx+dy*dy+dz*dz;
          if(d>(objs[i].r+0.3f)*(objs[i].r+0.3f) && d<bd){ bd=d; bi=i; } }
      if(bi>=0){ memcpy(g_hb_tgt,objs[bi].c,12); memcpy(g_hb_cam,vp+28,12); memcpy(g_hb_fwd,vp+12,12); g_hb_tgt_ok=1; } else g_hb_tgt_ok=0;
      const char*fp=getenv("PELOAD_INPUT_FACE_POINT"); float pt[3];   // "x,y,z": aim at a fixed world point instead
      if(fp && sscanf(fp,"%f,%f,%f",&pt[0],&pt[1],&pt[2])==3){ memcpy(g_hb_tgt,pt,12); memcpy(g_hb_cam,vp+28,12); memcpy(g_hb_fwd,vp+12,12); g_hb_tgt_ok=1; } }
    // label text: glyphs g_hb_text_px window pixels tall (PELOAD_HB_TEXT_PX, default 7; the stats page's Text -/+)
    { if(g_hb_text_px<=0){ const char*e=getenv("PELOAD_HB_TEXT_PX"); g_hb_text_px=e?atoi(e):7; if(g_hb_text_px<4) g_hb_text_px=4; if(g_hb_text_px>60) g_hb_text_px=60; }
      float W=(float)(g_win_pw>0?g_win_pw:1920), H=(float)(g_win_ph>0?g_win_ph:1080), u=g_hb_text_px/6.0f; g_hb_px=2.0f*u/W; g_hb_py=2.0f*u/H; }
    // PELOAD_HB_SELFDUMP=<file>: every 0.25 s append {time, camera forward[3], camera pos[3], our unit datum 0x800 B,
    // our player datum 0x400 B} -- raw material for locating aim / crouch / name / score fields offline.
    { const char*sd=getenv("PELOAD_HB_SELFDUMP"); static double ts; if(sd && now-ts>0.25){ ts=now;
        for(int i=0;i<n;i++){ const HbObj*o=&objs[i]; if(o->type) continue; float dx=o->c[0]-vp[28],dy=o->c[1]-vp[29],dz=o->c[2]-vp[30];
            if(dx*dx+dy*dy+dz*dz >= (o->r+0.3f)*(o->r+0.3f)) continue;
            static uint8_t rec[8+24+0x800+0x400]; memset(rec,0,sizeof rec); memcpy(rec,&now,8); memcpy(rec+8,vp+12,12); memcpy(rec+20,vp+28,12);
            hb_read(o->p,rec+32,0x800);
            uint8_t*h3=hb_halo3_base(); uint32_t idx=0; void**arrs[1025]; int na=0;
            if(h3 && hb_read((uint64_t)(h3+0xa39f9c),&idx,4) && idx<128){ arrs[na++]=g_tls_array; pthread_mutex_lock(&g_tlsm); for(int q=0;q<g_ntarrs&&na<1025;q++) arrs[na++]=g_tarrs[q]; pthread_mutex_unlock(&g_tlsm);
                for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)arrs[q][idx], parr=0; uint8_t hdr[0x60]; if(!blk||!hb_read(blk+0x10,&parr,8)||!parr||!hb_read(parr,hdr,sizeof hdr)||*(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
                    int32_t es=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t pd=*(uint64_t*)(hdr+0x48); if(es<=0x28||es>0x10000||hi<=0||hi>64) continue;
                    for(int pl=0;pl<hi;pl++){ uint32_t uh=0; if(hb_read(pd+(uint64_t)pl*es+0x28,&uh,4) && uh==o->handle){ hb_read(pd+(uint64_t)pl*es,rec+32+0x800,(size_t)(es<0x400?es:0x400));
                            static int once; if(!once){ once=1; char m[160]; int k=snprintf(m,sizeof m,"  [selfdump] players array: element 0x%x, %d slots; ours = slot %d\n",es,hi,pl); (void)!write(2,m,k); } break; } }
                    break; } }
            FILE*f=fopen(sd,"ab"); if(f){ fwrite(rec,1,sizeof rec,f); fclose(f); } break; } } }
    // our team: the biped the camera is inside (first person), else the local player the stats thread tracks (third
    // person, vehicles); kept while neither is found (dead, respawning)
    { int found=0; for(int i=0;i<n;i++){ const HbObj*o=&objs[i]; if(o->type) continue; float dx=o->c[0]-vp[28],dy=o->c[1]-vp[29],dz=o->c[2]-vp[30];
          if(dx*dx+dy*dy+dz*dz < (o->r+0.3f)*(o->r+0.3f)){ g_hb_myteam=o->team; found=1; break; } }
      uint32_t lu=g_hb_local_unit; if(!found && lu!=0xFFFFFFFFu) for(int i=0;i<n;i++) if(objs[i].type==0 && objs[i].handle==lu){ g_hb_myteam=objs[i].team; break; } }
    enum { HB_CAP=400000 }; static float verts[HB_CAP*5]; int nv=0; const float*cam=vp+28;   // cb0[7] = camera position
    static const float cwhite[3]={1,1,1}, cgrey[3]={0.7f,0.7f,0.7f}, ctrace[3]={1,0.95f,0.55f}, cvel[3]={1,0.5f,0.1f}, citem[3]={0.3f,1,0.45f}, cequip[3]={0.3f,0.9f,1};
    for(int i=0;i<n;i++){ const HbObj*o=&objs[i]; int unit=o->type==0||o->type==1||o->type==12;
        float dx=o->c[0]-cam[0], dy=o->c[1]-cam[1], dz=o->c[2]-cam[2]; int self = dx*dx+dy*dy+dz*dz < (o->r+0.3f)*(o->r+0.3f) || (o->type==0 && o->handle==g_hb_local_unit);   // our own body
        if(o->type==5 && ft){ hb_trail_add(o->handle,o->c,now);
            if(getenv("PELOAD_HITBOXES_LOG")){ static uint32_t seen[64]; static int ns; int f=0; for(int q=0;q<ns;q++) if(seen[q]==o->def) f=1;
                if(!f && ns<64){ seen[ns++]=o->def; char m[160]; int k=snprintf(m,sizeof m,"  [hitbox] projectile type seen: %s (%08x) speed %.2f\n",hb_tag_name(o->def),o->def,sqrtf(o->vel[0]*o->vel[0]+o->vel[1]*o->vel[1]+o->vel[2]*o->vel[2])); (void)!write(2,m,k); } }
            continue; }
        if(fv && o->type!=5 && !hb_vis(o)) continue;   // visible only: hidden behind the world
        if((o->type==2||o->type==3) && fw && o->parent==0xFFFFFFFFu){   // lying on the map (not held)
            float a[3]={o->c[0]-0.15f,o->c[1],o->c[2]}, b[3]={o->c[0]+0.15f,o->c[1],o->c[2]}, c2[3]={o->c[0],o->c[1]-0.15f,o->c[2]}, d2[3]={o->c[0],o->c[1]+0.15f,o->c[2]};
            const float*col=o->type==2?citem:cequip; nv=hb_seg(verts,nv,HB_CAP,vp,a,b,col); nv=hb_seg(verts,nv,HB_CAP,vp,c2,d2,col);
            float tx,ty; if(hb_proj(vp,o->c,&tx,&ty)){ const char*nm=hb_tag_name(o->def); nv=hb_text(verts,nv,HB_CAP,tx-strlen(nm)*2.5f*g_hb_px,ty+8*g_hb_py,nm,col); }
            if(!(fh&&g_hb_all)) continue; }
        if(self || !hb_team_shown(o)) continue;
        if(unit && fh && !g_hb_spheres){ int a=hb_emit_hitbox(o,vp,verts,nv,HB_CAP); if(a>0) nv+=a; if(a!=0) goto info; }
        if(fh && (unit || g_hb_all)){
            static const float pal[6][3]={{1,0.5f,0.5f},{1,0.85f,0.1f},{1,0.4f,0},{0.2f,1,0.3f},{0.2f,0.9f,1},{1,0.3f,1}};   // biped fallback sphere is pink, not hitbox red
            const float*col = pal[o->type==0?0 : o->type==1?1 : o->type==12?2 : o->type==2?3 : o->type==3?4 : 5];
            for(int axis=0;axis<3;axis++){ float prev[3]; int pv=0;
                for(int sgi=0;sgi<=24;sgi++){ float an=(float)sgi*(6.2831853f/24.f), ca=cosf(an)*o->r, sa=sinf(an)*o->r; float w[3]={o->c[0],o->c[1],o->c[2]};
                    if(axis==0){ w[0]+=ca; w[1]+=sa; } else if(axis==1){ w[0]+=ca; w[2]+=sa; } else { w[1]+=ca; w[2]+=sa; }
                    if(pv) nv=hb_seg(verts,nv,HB_CAP,vp,prev,w,col); memcpy(prev,w,12); pv=1; } } }
      info:
        if(fi && o->type==0){   // player info above the head: shield/health %, held weapon, grenades, camo, vehicle
            float top[3]={o->c[0],o->c[1],o->c[2]+o->r+0.12f}, tx,ty; if(!hb_proj(vp,top,&tx,&ty)) continue;
            const float*tc = o->team>=0&&o->team<8 ? g_hb_teamcol[o->team] : cwhite;
            char l1[48], l2[48], l3[48]; l3[0]=0;
            if(o->dead) snprintf(l1,sizeof l1,"DEAD");
            else snprintf(l1,sizeof l1,"S%d H%d",(int)(o->shield*100.f+0.5f),(int)(o->health*100.f+0.5f));
            uint32_t wh=(o->wslot>=0&&o->wslot<4)?o->weapons[o->wslot]:0xFFFFFFFFu; uint64_t wd=hb_handle_datum(wh); uint32_t wdef=0; if(wd) hb_read(wd,&wdef,4);
            snprintf(l2,sizeof l2,"%s",wd?hb_tag_name(wdef):"-");
            int g=o->gren[0]+o->gren[1]+o->gren[2]+o->gren[3]; int k3=0;
            if(g>0) k3+=snprintf(l3+k3,sizeof l3-k3,"G%d ",g);
            if(o->camo>0.05f) k3+=snprintf(l3+k3,sizeof l3-k3,"CAMO %d ",(int)(o->camo*100.f+0.5f));
            if(o->seat>=0) k3+=snprintf(l3+k3,sizeof l3-k3,"VEH");
            float lh=8*g_hb_py; float y=ty+3*lh;
            nv=hb_text(verts,nv,HB_CAP,tx-strlen(l1)*2.5f*g_hb_px,y,l1,tc); y-=lh;
            nv=hb_text(verts,nv,HB_CAP,tx-strlen(l2)*2.5f*g_hb_px,y,l2,cwhite); y-=lh;
            if(l3[0]) nv=hb_text(verts,nv,HB_CAP,tx-strlen(l3)*2.5f*g_hb_px,y,l3,o->camo>0.05f?cequip:cgrey); } }
    if(ft){ for(int t=0;t<256;t++){ HbTrail*tr=&g_hb_trails[t]; if(!tr->n) continue; if(now-tr->seen>0.6){ tr->n=0; continue; }   // gone: keep 0.6 s
            int cnt=tr->n<32?tr->n:32; for(int k=1;k<cnt;k++) nv=hb_seg(verts,nv,HB_CAP,vp,tr->p[(tr->n-1-k)&31],tr->p[(tr->n-k)&31],ctrace); }
        for(int i=0;i<n;i++){ const HbObj*o=&objs[i]; if(o->type!=5) continue; float sp=sqrtf(o->vel[0]*o->vel[0]+o->vel[1]*o->vel[1]+o->vel[2]*o->vel[2]); if(sp<1e-4f) continue;
            float e[3]={o->c[0]+o->vel[0]/sp*1.5f,o->c[1]+o->vel[1]/sp*1.5f,o->c[2]+o->vel[2]/sp*1.5f}; nv=hb_seg(verts,nv,HB_CAP,vp,o->c,e,cvel); } }   // 1.5 units along the velocity
    if(ft){ unsigned nimp=__atomic_load_n(&g_hbimp_n,__ATOMIC_ACQUIRE); for(unsigned k=0;k<256 && k<nimp;k++){ HbImpact*r=&g_hbimp[(nimp-1-k)&255];
            double t; __atomic_load(&r->t,&t,__ATOMIC_ACQUIRE); if(now-t>0.5) continue;
            // projectile datum: +0x1e8 start position (muzzle), +0x1b0 distance travelled -- the tracer runs start -> impact
            const float*st=(const float*)(r->raw+0x1e8); float trav=*(const float*)(r->raw+0x1b0);
            float ddx=r->pos[0]-st[0], ddy=r->pos[1]-st[1], ddz=r->pos[2]-st[2], dd=sqrtf(ddx*ddx+ddy*ddy+ddz*ddz);
            float a[3]={st[0],st[1],st[2]};
            if(!(trav>0.0f && fabsf(dd-trav)<0.25f*trav+0.5f)){   // start not recorded: 3 units back along the velocity
                float sp=sqrtf(r->vel[0]*r->vel[0]+r->vel[1]*r->vel[1]+r->vel[2]*r->vel[2]); memcpy(a,r->pos,12); if(sp>1e-5f) for(int q=0;q<3;q++) a[q]-=r->vel[q]/sp*3.0f; }
            nv=hb_seg(verts,nv,HB_CAP,vp,a,r->pos,ctrace);
            float x0[3]={r->pos[0]-0.05f,r->pos[1],r->pos[2]}, x1[3]={r->pos[0]+0.05f,r->pos[1],r->pos[2]}; nv=hb_seg(verts,nv,HB_CAP,vp,x0,x1,cvel); } }
    if(!nv) return;
    G.Disable(0x0B71/*DEPTH_TEST*/); G.Disable(0x0BE2/*BLEND*/); G.Disable(0x0C11/*SCISSOR*/);
    G.UseProgram(g_hb_prog); G.BindVertexArray(g_hb_vao); G.BindBuffer(0x8892/*ARRAY_BUFFER*/,g_hb_vbo);
    G.BufferData(0x8892,(long)nv*5*4,verts,0x88E0/*STREAM_DRAW*/);
    G.EnableVertexAttribArray(0); G.VertexAttribPointer(0,2,0x1406/*FLOAT*/,0,20,(void*)0);
    G.EnableVertexAttribArray(1); G.VertexAttribPointer(1,3,0x1406,0,20,(void*)8);
    G.DrawArrays(1/*LINES*/,0,nv); G.BindVertexArray(0); }
// ---- frame capture (F8 or the stats page's Capture button): the frame, its depth and the camera ------------------
// For chasing rendering glitches at a spot a player found. At the next Present (render thread, hb_capture_tick) this
// writes ~/reclaimer_cap_<n>.bmp (the frame as rendered: internal resolution, HUD included),
// ~/reclaimer_cap_<n>_depth.bmp (distance, log grey scale; black = nothing drawn; magenta = "crack" pixels, i.e.
// pixels much farther than both neighbours on a row or column: background showing through a one-pixel gap),
// ~/reclaimer_cap_<n>_depth.f32 (raw window depth, w*h floats, top row first like the images) and
// ~/reclaimer_cap_<n>.txt (camera position, view direction, view-projection rows, depth statistics).
// The depth buffer is the one the frame's first world draw (the camera draw, hb_capture_view) used.
volatile int g_hb_cap_req; static unsigned long g_hb_ncap;
static void hb_note_dsv(void*ds){ g_hb_dsv=(D3DObj*)ds; }
// GL row 0 of the game's targets is the D3D top row (d3dgl_flipy) and bmp_write_rgba writes GL order (row 0 = bottom):
// flip so the files come out upright
static void hb_cap_flip(void*px,size_t pitch,int h){ if(!d3dgl_flipy()) return; unsigned char*t=malloc(pitch); if(!t) return; unsigned char*b=px;
    for(int y=0;y<h/2;y++){ memcpy(t,b+(size_t)y*pitch,pitch); memcpy(b+(size_t)y*pitch,b+(size_t)(h-1-y)*pitch,pitch); memcpy(b+(size_t)(h-1-y)*pitch,t,pitch); } free(t); }
static void hb_capture_tick(void){
    if(!__atomic_exchange_n(&g_hb_cap_req,0,__ATOMIC_ACQ_REL)) return;
    char m[600]; int k;
    if(!G.ReadPixels || !g_d3d_backbuf || !g_d3d_backbuf->glfbo){ (void)!write(2,"  [capture] no frame to capture yet\n",36); return; }
    const char*home=getenv("HOME"); if(!home) home="/tmp";
    unsigned long n=g_hb_ncap++; char base[400]; snprintf(base,sizeof base,"%s/reclaimer_cap_%d_%02lu",home,(int)getpid(),n);
    char p[480];
    int w=g_d3dgl_present_vw, h=g_d3dgl_present_vh;
    if(w>0 && h>0 && (long)w*h<=(64<<20)){ unsigned char*cap=calloc(1,(size_t)w*h*4);
        if(cap){ G.BindFramebuffer(GLE_FBO,g_d3d_backbuf->glfbo); if(G.ReadBuffer) G.ReadBuffer(GLE_CA0); G.ReadPixels(0,0,w,h,GLE_RGBA,GLE_UBYTE,cap);
            hb_cap_flip(cap,(size_t)w*4,h); snprintf(p,sizeof p,"%s.bmp",base); bmp_write_rgba(p,w,h,cap); free(cap); } }
    float vp[32]={0}; for(int t=0;t<100;t++){ unsigned s0=g_hb_vpseq; if(s0&1) continue; memcpy(vp,g_hb_vp,sizeof vp); if(g_hb_vpseq==s0) break; }
    const float*eye=vp+28; float fx=vp[12],fy=vp[13],fz=vp[14], fl=sqrtf(fx*fx+fy*fy+fz*fz); if(fl>0){ fx/=fl; fy/=fl; fz/=fl; }
    double yaw=atan2(fy,fx)*180/M_PI, pitch=asin(fz>1?1:fz<-1?-1:fz)*180/M_PI;
    // depth: grey = log distance (reverse-Z: window depth ~ near/distance), magenta = crack pixels
    D3DObj*ds=g_hb_dsv; long cracks=0, drawn=0; float dmin=1e30f, dmax=0; int dw=0, dh=0;
    if(ds && ds->gltex && ds->gldsatt){ dw=(int)(ds->w>4096?4096:ds->w); dh=(int)(ds->h>4096?4096:ds->h);
        float*d=dw>0&&dh>0?malloc((size_t)dw*dh*4):0; unsigned char*img=d?calloc(1,(size_t)dw*dh*4):0;
        if(d && img){ unsigned fbo=d3dgl_fbo_for(0,0,ds); G.BindFramebuffer(GLE_FBO,fbo); G.ReadPixels(0,0,dw,dh,0x1902/*DEPTH_COMPONENT*/,0x1406/*FLOAT*/,d);
            hb_cap_flip(d,(size_t)dw*4,dh); snprintf(p,sizeof p,"%s_depth.f32",base); FILE*f=fopen(p,"wb"); if(f){ fwrite(d,4,(size_t)dw*dh,f); fclose(f); }
            for(long i=0;i<(long)dw*dh;i++){ float v=d[i]; if(v>0){ drawn++; if(v<dmin) dmin=v; if(v>dmax) dmax=v; } }
            double lmin=log(dmin>0&&dmin<1e30f?dmin:1e-7), lmax=log(dmax>0?dmax:1); if(lmax-lmin<1e-6) lmax=lmin+1e-6;
            for(int y=0;y<dh;y++) for(int x=0;x<dw;x++){ long i=(long)y*dw+x; float v=d[i]; unsigned char*o=img+i*4; o[3]=255;
                if(!(v>0)) continue;
                int crack=0;
                if(x>0 && x<dw-1){ float a=d[i-1], b=d[i+1]; if(a>0 && b>0 && v<0.5f*a && v<0.5f*b) crack=1; }
                if(!crack && y>0 && y<dh-1){ float a=d[i-dw], b=d[i+dw]; if(a>0 && b>0 && v<0.5f*a && v<0.5f*b) crack=1; }
                if(crack){ cracks++; o[0]=255; o[1]=0; o[2]=255; continue; }
                unsigned char g=(unsigned char)(40+215*((log(v)-lmin)/(lmax-lmin))); o[0]=o[1]=o[2]=g; }
            snprintf(p,sizeof p,"%s_depth.bmp",base); bmp_write_rgba(p,dw,dh,img); }
        free(d); free(img); }
    G.BindFramebuffer(GLE_FBO,0);
    snprintf(p,sizeof p,"%s.txt",base); FILE*f=fopen(p,"w");
    if(f){ fprintf(f,"camera %.4f %.4f %.4f\nforward %.5f %.5f %.5f\nyaw_deg %.2f\npitch_deg %.2f\n",eye[0],eye[1],eye[2],fx,fy,fz,yaw,pitch);
        for(int r=0;r<4;r++) fprintf(f,"viewproj_row%d %.9g %.9g %.9g %.9g\n",r,vp[r*4],vp[r*4+1],vp[r*4+2],vp[r*4+3]);
        fprintf(f,"frame %dx%d\ndepth %dx%d fmt %u drawn %ld min %.9g max %.9g cracks %ld\n",w,h,dw,dh,ds?ds->fmt:0,drawn,drawn?dmin:0,dmax,cracks);
        extern double g_gameplay_t0; fprintf(f,"match_time %.1f\n",g_gameplay_t0?mono_s()-g_gameplay_t0:0.0); fclose(f); }
    k=snprintf(m,sizeof m,"  [capture] %s.{bmp,txt}%s: camera (%.2f %.2f %.2f) yaw %.1f pitch %.1f, depth %dx%d, %ld crack pixels\n",
        base,ds?" + _depth.{bmp,f32}":"",eye[0],eye[1],eye[2],yaw,pitch,dw,dh,cracks); (void)!write(2,m,k); }
static void hitbox_start(void){
    if(getenv("PELOAD_OBJDUMP")){ pthread_t t; pthread_create(&t,0,objdump_thread,0); } }
