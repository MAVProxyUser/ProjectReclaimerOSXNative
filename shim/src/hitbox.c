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
static void* objdump_thread(void*a){ (void)a; extern double g_gameplay_t0; while(!g_gameplay_t0) usleep(200000);
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
static int g_hb_on=-1, g_hb_all;
static int hb_enabled(void){ if(g_hb_on<0){ const char*e=getenv("PELOAD_HITBOXES"); g_hb_on=e&&*e&&strcmp(e,"0"); g_hb_all=e&&!strcasecmp(e,"all"); } return g_hb_on; }
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
typedef struct { float c[3], r, o[3]; int type; } HbObj;
static int hb_collect(HbObj*out,int max){
    static uint64_t arr; static double tarr; double now=mono_s(); if(!arr || now-tarr>2.0){ arr=hb_object_array(); tarr=now; } if(!arr) return 0;
    uint8_t hdr[0x60]; if(!hb_read(arr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440){ arr=0; return 0; }
    int32_t maxc=*(int32_t*)(hdr+0x20), esz=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t data=*(uint64_t*)(hdr+0x48);
    if(esz!=0x18 || hi<=0 || hi>maxc || hi>8192) return 0;
    static uint8_t tab[8192*0x18]; if(!hb_read(data,tab,(size_t)hi*0x18)) return 0;
    int n=0;
    for(int i=0;i<hi && n<max;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e) continue; int t=e[3];
        int want = t==0||t==1||t==12 || (g_hb_all && (t==2||t==3||t==5)); if(!want) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); float od[0x18]; if(!p || !hb_read(p+0x1c,od,0x40)) continue;   // +0x1c..+0x5c
        HbObj*o=&out[n]; o->c[0]=od[0]; o->c[1]=od[1]; o->c[2]=od[2]; o->r=od[3]; o->o[0]=od[13]; o->o[1]=od[14]; o->o[2]=od[15]; o->type=t;  // origin at +0x50 = od[(0x50-0x1c)/4]
        if(!(o->r>0.01f && o->r<50.f) || o->c[0]!=o->c[0]) continue; n++; }
    return n; }
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
    if(!n) return;
    static float verts[512*3*24*2*5]; int nv=0; const float*cam=vp+28;   // cb0[7] = camera position
    for(int i=0;i<n;i++){ const HbObj*o=&objs[i];
        float dx=o->c[0]-cam[0], dy=o->c[1]-cam[1], dz=o->c[2]-cam[2]; if(dx*dx+dy*dy+dz*dz < (o->r+0.3f)*(o->r+0.3f)) continue;   // our own body
        static const float pal[6][3]={{1,0.15f,0.15f},{1,0.85f,0.1f},{1,0.4f,0},{0.2f,1,0.3f},{0.2f,0.9f,1},{1,0.3f,1}};
        const float*col = pal[o->type==0?0 : o->type==1?1 : o->type==12?2 : o->type==2?3 : o->type==3?4 : 5];
        for(int axis=0;axis<3;axis++){ float prev[2]; int pv=0;
            for(int sgi=0;sgi<=24;sgi++){ float a=(float)sgi*(6.2831853f/24.f), ca=cosf(a)*o->r, sa=sinf(a)*o->r; float w[3]={o->c[0],o->c[1],o->c[2]};
                if(axis==0){ w[0]+=ca; w[1]+=sa; } else if(axis==1){ w[0]+=ca; w[2]+=sa; } else { w[1]+=ca; w[2]+=sa; }
                float cx=w[0]*vp[0]+w[1]*vp[1]+w[2]*vp[2]+vp[3], cy=w[0]*vp[4]+w[1]*vp[5]+w[2]*vp[6]+vp[7], cw=w[0]*vp[12]+w[1]*vp[13]+w[2]*vp[14]+vp[15];
                if(cw<0.05f){ pv=0; continue; } float nx=cx/cw, ny=cy/cw;
                if(pv && nv+2<=(int)(sizeof verts/sizeof verts[0]/5)){ float*q=verts+nv*5; q[0]=prev[0]; q[1]=prev[1]; q[2]=col[0]; q[3]=col[1]; q[4]=col[2];
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
