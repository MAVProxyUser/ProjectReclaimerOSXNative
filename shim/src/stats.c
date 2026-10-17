// stats.c — live game state over HTTP (PELOAD_STATS_HTTP=<port>). Included by peload.c after hitbox.c.
// A loader thread snapshots the engine's object table 10 times a second -- the fields the overlay reads (hitbox.c,
// shim/docs/halo3-objects.md) -- and serves it on 127.0.0.1:<port> (PELOAD_STATS_ADDR=<ip> listens elsewhere):
//   /stats.json  the latest snapshot          /events  Server-Sent Events, one snapshot per 100 ms
//   /            a page that shows /events as a live table
// It is what this client's engine holds: Halo 3 replicates every player's biped (position, health, shield, camo,
// weapons) to every client. Off by default; independent of the overlay (F2-F6).
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int g_st_port=-1;
static int stats_on(void){ if(g_st_port<0){ const char*e=getenv("PELOAD_STATS_HTTP"); g_st_port=e?atoi(e):0; if(g_st_port==1) g_st_port=8765; if(g_st_port<0||g_st_port>65535) g_st_port=0; } return g_st_port>0; }
static pthread_mutex_t g_st_mx=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_st_cv=PTHREAD_COND_INITIALIZER;
static char* g_st_json; static size_t g_st_len; static unsigned long g_st_seq;

typedef struct { char*b; size_t n, cap; } StBuf;
static void sb_put(StBuf*s,const char*fmt,...){ va_list ap; for(;;){ size_t room=s->cap-s->n; va_start(ap,fmt); int k=vsnprintf(s->b?s->b+s->n:0,room,fmt,ap); va_end(ap);
        if(k<0) return; if((size_t)k<room){ s->n+=(size_t)k; return; } size_t nc=(s->cap?s->cap*2:16384); while(nc<s->n+(size_t)k+1) nc*=2; char*nb=realloc(s->b,nc); if(!nb) return; s->b=nb; s->cap=nc; } }
static void sb_str(StBuf*s,const char*v){ sb_put(s,"\""); for(const unsigned char*p=(const unsigned char*)v; p&&*p; p++){
        if(*p=='"'||*p=='\\') sb_put(s,"\\%c",*p); else if(*p<0x20) sb_put(s,"\\u%04x",*p); else sb_put(s,"%c",*p); } sb_put(s,"\""); }
static void sb_f3(StBuf*s,const float*v){ sb_put(s,"[%.2f,%.2f,%.2f]",v[0],v[1],v[2]); }

// The engine's players table (TLS block +0x10, a data array like the object table): +0x28 unit handle, +0x58 name.
typedef struct { uint32_t unit; int slot; char name[64]; uint64_t rec; } StPlayer;
static uint64_t g_st_tls;   // the engine thread's halo3 TLS block (st_players found it)
static int st_players(StPlayer*out,int max){
    uint8_t*h3=hb_halo3_base(); uint32_t idx=0; if(!h3 || !hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) return 0;
    void**arrs[1025]; int na=0; arrs[na++]=g_tls_array; pthread_mutex_lock(&g_tlsm); for(int q=0;q<g_ntarrs&&na<1025;q++) arrs[na++]=g_tarrs[q]; pthread_mutex_unlock(&g_tlsm);
    for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)arrs[q][idx], parr=0, oarr=0; uint8_t hdr[0x60];
        if(!blk || !hb_read(blk+0x38,&oarr,8) || !oarr) continue;   // the engine thread's block (it also holds the object table)
        if(!hb_read(blk+0x10,&parr,8) || !parr || !hb_read(parr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
        int32_t es=*(int32_t*)(hdr+0x24), hi=*(int32_t*)(hdr+0x3c); uint64_t pd=*(uint64_t*)(hdr+0x48); if(es<0x98 || es>0x10000 || hi<=0 || hi>256) continue;
        int n=0; for(int i=0;i<hi && n<max;i++){ uint8_t rec[0x98]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue;
            StPlayer*p=&out[n]; p->unit=*(uint32_t*)(rec+0x28); p->slot=i; p->rec=pd+(uint64_t)i*es; int j=0; const uint16_t*w=(const uint16_t*)(rec+0x58);
            for(int c=0;c<32 && w[c] && j<60;c++){ uint16_t u=w[c]; if(u<0x80) p->name[j++]=(char)u;
                else if(u<0x800){ p->name[j++]=(char)(0xC0|(u>>6)); p->name[j++]=(char)(0x80|(u&63)); }
                else { p->name[j++]=(char)(0xE0|(u>>12)); p->name[j++]=(char)(0x80|((u>>6)&63)); p->name[j++]=(char)(0x80|(u&63)); } }
            p->name[j]=0; n++; }
        g_st_tls=blk; return n; }
    return 0; }

// ---- game variant and player traits (layout: shim/docs/halo3-variant-traits.md) -------------------------------------
// The running variant is game_globals (TLS block +0x48) + 0x1608 (0x4C4 bytes), only while game_globals+0x10 == 2
// (multiplayer). A trait set is 0x1C bytes; 0 = "Unchanged" in a variant's sets (weapons: -2). Each player's record
// holds its resolved active traits at +0x1FC, recomputed every tick (base -> mode role -> powerup -> respawn traits).
// Names and value labels follow the playlist option keys (shim/docs/game-options.md).
static const char*const tl_res[]={"Unchanged","10%","50%","90%","100%","110%","150%","200%","300%","500%","1000%","2000%","Invulnerable","Invulnerable (incl. assassinations)"};
static const char*const tl_rech[]={"Unchanged","-25%","-10%","-5%","Disabled","50%","90%","100%","110%","200%"};
static const char*const tl_vamp[]={"Unchanged","Off","10%","25%","50%","100%"};
static const char*const tl_head[]={"Unchanged","Immune","Vulnerable"};
static const char*const tl_shield[]={"Unchanged","None","Normal","2x","3x","4x"};
static const char*const tl_gren[]={"Unchanged","Map default","None"};
static const char*const tl_dmg[]={"Unchanged","0%","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%","Instant kill"};
static const char*const tl_regen[]={"Unchanged","On","Off","On, with firebombs"};
static const char*const tl_ammo[]={"Unchanged","Normal","Infinite","Bottomless clip"};
static const char*const tl_pick[]={"Unchanged","Allowed","Disabled"};
static const char*const tl_speed[]={"Unchanged","25%","50%","75%","90%","100%","110%","125%","150%","200%","300%"};
static const char*const tl_grav[]={"Unchanged","50%","75%","100%","150%","200%"};
static const char*const tl_veh[]={"Unchanged","None","Passenger only","Full use"};
static const char*const tl_camo[]={"Unchanged","Off","Poor","Good","Invisible"};
static const char*const tl_wp[]={"Unchanged","Off","Allies","Everyone"};
static const char*const tl_aura[]={"Unchanged","Off","Team color","Black","White"};
static const char*const tl_radar[]={"Unchanged","Off","Allies only","Normal","Enhanced"};
static const char*const tl_range[]={"Unchanged","10m","15m","25m","50m","75m","100m","150m"};
static const char*const tl_weap[]={"Battle rifle","Assault rifle","Plasma pistol","Spiker","SMG","Carbine","Energy sword","Magnum","Needler",
    "Plasma rifle","Rocket launcher","Shotgun","Sniper rifle","Brute shot","Unarmed","Beam rifle","Spartan laser","No weapon","Gravity hammer",
    "Mauler","Flamethrower","Missile pod","Sentinel beam","Fuel rod gun","Silenced SMG","Automag","Brute plasma rifle","Machine gun turret",
    "Plasma turret","Golf club"};
#define TL(a) a,(int)(sizeof a/sizeof*a)
typedef struct { const char*key; int off, sz; const char*const*lab; int nlab; int def; } StTraitF;   // sz: 1 u8, 2 u16, -1 i8 weapon, 0 colour
static const StTraitF g_trait_f[]={
    {"resistance",0x00,1,TL(tl_res),4},{"recharge",0x01,1,TL(tl_rech),7},{"vampirism",0x02,1,TL(tl_vamp),1},{"headshots",0x03,1,TL(tl_head),2},
    {"shields",0x04,1,TL(tl_shield),2},{"grenades",0x08,2,TL(tl_gren),1},{"primary_weapon",0x0A,-1,TL(tl_weap),-1},{"secondary_weapon",0x0B,-1,TL(tl_weap),-1},
    {"damage",0x0C,1,TL(tl_dmg),6},{"grenade_regeneration",0x0D,1,TL(tl_regen),2},{"ammo",0x0E,1,TL(tl_ammo),1},{"pickup",0x0F,1,TL(tl_pick),1},
    {"speed",0x10,1,TL(tl_speed),5},{"gravity",0x11,1,TL(tl_grav),3},{"vehicles",0x12,1,TL(tl_veh),3},{"camouflage",0x14,1,TL(tl_camo),1},
    {"waypoint",0x15,1,TL(tl_wp),2},{"aura",0x16,1,TL(tl_aura),1},{"color",0x17,0,0,0,1},{"radar",0x18,2,TL(tl_radar),3},{"radar_range",0x1A,2,TL(tl_range),3}};
// active=0: a variant's trait set, listing the fields it changes; active=1: a player's resolved traits, listing the
// fields that differ from Halo 3's defaults
static void st_traits(StBuf*s,const uint8_t*t,int active){
    sb_put(s,"{"); int first=1;
    for(size_t i=0;i<sizeof g_trait_f/sizeof*g_trait_f;i++){ const StTraitF*f=&g_trait_f[i]; int v; char buf[48]; const char*lab=0;
        if(f->sz==2) v=*(const uint16_t*)(t+f->off); else if(f->sz==-1) v=*(const int8_t*)(t+f->off); else v=t[f->off];
        if(f->sz==-1){ if(active?v==f->def:v==-2) continue;
            if(v==-3) lab="Random"; else if(v==-1) lab="Map default"; else if(v==-2) lab="Unchanged"; else if(v>=0 && v<f->nlab) lab=f->lab[v]; }
        else { if(active?v==f->def:v==0) continue;
            if(f->lab && v>=0 && v<f->nlab) lab=f->lab[v]; else if(!f->lab){ if(v==0) lab="Unchanged"; else if(v==1) lab="Off"; else { snprintf(buf,sizeof buf,"Color %d",v-2); lab=buf; } } }
        if(!lab){ snprintf(buf,sizeof buf,"%d",v); lab=buf; }
        sb_put(s,"%s\"%s\":",first?"":",",f->key); sb_str(s,lab); first=0; }
    sb_put(s,"}"); }
static uint8_t g_st_var[0x4C4]; static int g_st_var_ok;   // this snapshot's variant (for the players' powerup durations)
static void st_variant(StBuf*s){
    g_st_var_ok=0; uint64_t gg=0; uint8_t mode=0;
    if(!g_st_tls || !hb_read(g_st_tls+0x48,&gg,8) || !gg || !hb_read(gg+0x10,&mode,1) || mode!=2 || !hb_read(gg+0x1608,g_st_var,sizeof g_st_var)) return;
    const uint8_t*v=g_st_var; int32_t eng=*(const int32_t*)v; if(eng<1 || eng>12 || eng==11) return; g_st_var_ok=1;
    static const char*const en[13]={"","Capture the Flag","Slayer","Oddball","King of the Hill","Forge","VIP","Juggernaut","Territories","Assault","Infection","","Gun Game"};
    char nm[64]; int j=0; for(int c=0;c<16 && j<60;c++){ uint16_t u=*(const uint16_t*)(v+0x14+c*2); if(!u) break;
        if(u<0x80) nm[j++]=(char)u; else if(u<0x800){ nm[j++]=(char)(0xC0|(u>>6)); nm[j++]=(char)(0x80|(u&63)); } else { nm[j++]=(char)(0xE0|(u>>12)); nm[j++]=(char)(0x80|((u>>6)&63)); nm[j++]=(char)(0x80|(u&63)); } }
    nm[j]=0;
    static const char*const tc[3]={"Disabled","Enabled","Balancing only"}; uint16_t soc=*(const uint16_t*)(v+0x12C), tch=*(const uint16_t*)(v+0x12E); uint32_t mo=*(const uint32_t*)(v+0x130);
    sb_put(s,",\"variant\":{\"engine\":%d,\"mode\":",eng); sb_str(s,en[eng]); sb_put(s,",\"name\":"); sb_str(s,nm);
    sb_put(s,",\"rounds\":{\"teams\":%s,\"team_changing\":\"%s\",\"friendly_fire\":%s,\"betrayal_booting\":%s,\"reset_players\":%s,\"reset_map\":%s,\"minutes\":%d,\"limit\":%d,\"early_victory\":%d}",
        v[0x104]&1?"true":"false",tch<3?tc[tch]:"?",soc&1?"true":"false",soc&2?"true":"false",v[0x104]&2?"true":"false",v[0x104]&4?"true":"false",v[0x105],v[0x106],v[0x107]);
    sb_put(s,",\"respawn\":{\"lives\":%d,\"team_lives\":%d,\"seconds\":%d,\"suicide\":%d,\"betrayal\":%d,\"growth\":%d,\"traits_duration\":%d,\"synchronize\":%s,\"on_kills\":%s}",
        v[0x109],v[0x10A],v[0x10B],v[0x10C],v[0x10D],v[0x10E],v[0x10F],v[0x108]&2?"true":"false",v[0x108]&8?"true":"false");
    sb_put(s,",\"map\":{\"grenades\":\"%s\",\"indestructible_vehicles\":%s}",mo&1?"Map default":"None",mo&2?"true":"false");
    sb_put(s,",\"powerups\":{\"red_duration\":%d,\"blue_duration\":%d,\"yellow_duration\":%d}",v[0x1A8],v[0x1A9],v[0x1AA]);
    { int sw=0; switch(eng){ case 2: case 6: case 7: sw=0x1B4; break; case 3: case 4: sw=0x1B8; break; case 1: case 9: sw=0x1BC; break; case 12: sw=0x1BE; break; }
      if(sw) sb_put(s,",\"score_to_win\":%d",*(const int16_t*)(v+sw)); }
    if(eng==2){ static const char*const pk[12]={"assist","death","suicide","betrayal","leader_kill","elimination","assassination","headshot","beatdown","sticky","splatter","spree"};
        sb_put(s,",\"points\":{\"kill\":%d",*(const int16_t*)(v+0x1B6)); for(int k=0;k<12;k++) sb_put(s,",\"%s\":%d",pk[k],(int8_t)v[0x1B8+k]); sb_put(s,"}"); }
    // trait sets: the base ones, then this mode's role sets
    typedef struct { const char*n; int off; } TS; static const TS base[5]={{"player",0x134},{"respawn_traits",0x110},{"red_powerup",0x154},{"blue_powerup",0x170},{"yellow_powerup",0x18C}};
    TS role[8]; int nr=0;
    switch(eng){ case 1: role[nr++]=(TS){"flag_carrier",0x1C0}; break; case 2: role[nr++]=(TS){"leader",0x1C4}; break; case 3: role[nr++]=(TS){"ball_carrier",0x1C4}; break;
        case 4: role[nr++]=(TS){"hill_occupant",0x1C0}; break; case 5: role[nr++]=(TS){"editor",0x1B8}; break;
        case 6: role[nr++]=(TS){"vip_team",0x1C6}; role[nr++]=(TS){"vip_proximity",0x1E2}; role[nr++]=(TS){"vip",0x1FE}; break;
        case 7: role[nr++]=(TS){"juggernaut",0x1C4}; break; case 8: role[nr++]=(TS){"defender",0x1BC}; role[nr++]=(TS){"attacker",0x1D8}; break;
        case 9: role[nr++]=(TS){"bomb_carrier",0x1C8}; role[nr++]=(TS){"bomb_arming",0x1E4}; break;
        case 10: role[nr++]=(TS){"infected",0x1C0}; role[nr++]=(TS){"initial_infected",0x1DC}; role[nr++]=(TS){"safe_haven",0x1F8}; role[nr++]=(TS){"last_survivor",0x214}; break;
        case 12: role[nr++]=(TS){"final_tier",0x2D0}; role[nr++]=(TS){"leader",0x2EC}; break; }
    sb_put(s,",\"traits\":{"); for(int k=0;k<5;k++){ sb_put(s,"%s\"%s\":",k?",":"",base[k].n); st_traits(s,v+base[k].off,0); }
    for(int k=0;k<nr;k++){ sb_put(s,",\"%s\":",role[k].n); st_traits(s,v+role[k].off,0); }
    sb_put(s,"}}"); }
// A player's active traits and what is driving them right now (a powerup, respawn traits)
static void st_player_traits(StBuf*s,uint64_t rec){
    uint8_t r[0x228]; if(!rec || !hb_read(rec,r,sizeof r)) return;
    sb_put(s,",\"traits\":"); st_traits(s,r+0x1FC,1);
    sb_put(s,",\"effects\":["); int first=1;
    uint64_t t=0; int32_t now=0; float spt=0;
    if(g_st_tls && hb_read(g_st_tls+0xc8,&t,8) && t && hb_read(t+0xC,&now,4) && hb_read(t+0x8,&spt,4) && spt>0 && spt<1 && g_st_var_ok){
        static const char*const pn[3]={"red powerup","blue powerup","yellow powerup"}; int best=-1; int32_t bt=0;
        for(int k=0;k<3;k++){ int32_t tk=*(const int32_t*)(r+0x218+k*4); if(tk>0 && tk>bt){ bt=tk; best=k; } }
        if(best>=0){ double left=g_st_var[0x1A8+best]-(now-bt)*(double)spt; if(left>0){ sb_put(s,"{\"what\":\"%s\",\"left\":%.1f}",pn[best],left); first=0; } }
        int16_t rt=*(const int16_t*)(r+0x1D4); if(rt>0 && !r[0x1D6]){ sb_put(s,"%s{\"what\":\"respawn traits\",\"left\":%.1f}",first?"":",",rt*(double)spt); first=0; } }
    sb_put(s,"]"); }

static void st_tag(StBuf*s,uint32_t def){ sb_str(s,hb_tag_name(def)); }

// ---- the network session: everyone connected (also in the lobby, where the players table is empty) + voice mute -----
// (shim/docs/reclaimer-voice-mute.md) Halo 3's session membership: sess = *(u64*)(halo3+0x1e250d0). Reclaimer patches
// halo3 for 128 players and then keeps the records in an extended area (flag byte exe+0x27f7210 in 0.9.18):
//   player valid mask sess+0x126c (16 / 128 bits); player i at sess+0x1270+i*0xc8 (extended sess+0x1493c0+i*0xc8):
//     +0x04 u64 identifier (0 = none), +0x0c i32 peer index, +0x98 UTF-16 name[16]
//   peer valid mask sess+0x54 (17 / 128 bits); peer p at sess+0x58+p*0x110 (extended sess+0x1381c0+p*0x110):
//     +0x00 u64 peer id (= the client's voice id), +0x08 u32 state (8 = joined)
// The client's voice mute is one global Mutex<Vec<peer id>> changed by set_muted(id, bool) (Win64; exe+0x66bea0 in
// 0.9.18, found by signature); its own scoreboard Mute buttons use the same list, so both stay in sync. Mutes last
// until the client exits. The call runs on a guest thread: the halo3 main loop's frame hook (st_voice_jobs).
typedef struct { char name[64]; int idx, peer; uint64_t voice; int muted; } StSess;
static uint8_t* g_st_setmuted; static uint64_t g_st_mutedvec; static int g_st_layout=-1;   // -1 unknown, 0 normal, 1 extended
static int sig_parse(const char*s,uint8_t*b,uint8_t*m,int cap); static int64_t sig_find(uint8_t*base,const uint8_t*b,const uint8_t*m,int n);
static int entry_hook(uint8_t*site,const uint8_t*pro,int len,void*fn);   // peload.c
// Who is talking: an entry hook on the client's voice packet handler (voice_recv(state, sender peer id, payload, len);
// exe+0x684080 in 0.9.18, by signature; its first 19 bytes are position-independent) stamps the sender's last packet.
static pthread_mutex_t g_talk_mx=PTHREAD_MUTEX_INITIALIZER; static struct { uint64_t id; double t; } g_talk[128];
static void __attribute__((ms_abi)) st_on_voice(void*state,uint64_t sender){ (void)state; double now=mono_s(); int o=0;
    pthread_mutex_lock(&g_talk_mx); for(int i=0;i<128;i++){ if(g_talk[i].id==sender){ o=i; break; } if(g_talk[i].t<g_talk[o].t) o=i; }
    g_talk[o].id=sender; g_talk[o].t=now; pthread_mutex_unlock(&g_talk_mx); }
static double st_talk_age(uint64_t id){ double t=0; pthread_mutex_lock(&g_talk_mx); for(int i=0;i<128;i++) if(g_talk[i].id==id){ t=g_talk[i].t; break; } pthread_mutex_unlock(&g_talk_mx);
    return t>0?mono_s()-t:1e9; }
// Runs at exe load (single-threaded, before the entry point), only with the stats page on.
static void st_voice_install(void){ if(!stats_on() || !g_base) return;
    uint8_t b[96],m[96]; int n; int64_t r; char msg[220]; int k;
    n=sig_parse("55 56 57 53 48 83 EC 38 48 8D 6C 24 30 48 C7 45 00 FE FF FF FF 48 89 CE B1 01 31 C0 F0 0F B0 0D ?? ?? ?? ?? 0F 85",b,m,96);
    r=sig_find(g_base,b,m,n); if(r>=0){ g_st_setmuted=g_base+r; g_st_mutedvec=(uint64_t)(g_st_setmuted+0x24+*(int32_t*)(g_st_setmuted+0x20)); }
    n=sig_parse("55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 18 01 00 00 48 8D AC 24 80 00 00 00 0F 29 B5 80 00 00 00 48 C7 45 78 FE FF FF FF 4D 89 CE 4C 89 C7 49 89 D7 48 89 CB",b,m,96);
    int64_t rv=sig_find(g_base,b,m,n); int hooked=rv>=0 && entry_hook(g_base+rv,b,19,(void*)st_on_voice);
    k=snprintf(msg,sizeof msg,"  [stats] voice: mute %s, talking indicator %s\n",g_st_setmuted?"available":"unavailable (set_muted not found)",hooked?"hooked":"unavailable (voice_recv not found)");
    (void)!write(2,msg,k); }
static void st_voice_init(void){ static int done; if(done || !g_base) return; done=1;
    // 0.9.18: set_muted at its known place -> the client's expanded-session flag is at its known place too; other
    // builds pick the layout by which one holds valid records
    if(g_st_setmuted==g_base+0x66bea0){ uint8_t x=0; if(hb_read((uint64_t)(g_base+0x27f7210),&x,1) && x<=1) g_st_layout=x; } }
static int st_session(StSess*out,int max,uint32_t*state){
    uint8_t*h3=hb_halo3_base(); if(!h3) return 0; st_voice_init();
    uint64_t sess=0; if(!hb_read((uint64_t)(h3+0x1e250d0),&sess,8) || !sess) return 0;
    if(state) hb_read(sess+0x67b70,state,4);
    if(g_st_setmuted==g_base+0x66bea0){ uint8_t x=0; if(hb_read((uint64_t)(g_base+0x27f7210),&x,1) && x<=1) g_st_layout=x; }   // set once Reclaimer patches halo3
    uint64_t muted[256]; int nm=0; { uint64_t ptr=0, len=0; if(g_st_mutedvec && hb_read(g_st_mutedvec+0x10,&ptr,8) && hb_read(g_st_mutedvec+0x18,&len,8) && len<=256 && ptr>1 && len) if(hb_read(ptr,muted,len*8)) nm=(int)len; }
    int best=0;
    for(int lay=1;lay>=0;lay--){ if(g_st_layout>=0 && lay!=g_st_layout) continue;
        uint8_t pm[16]={0}, qm[16]={0}; if(!hb_read(sess+0x126c,pm,lay?16:4) || !hb_read(sess+0x54,qm,lay?16:4)) continue;
        int np=lay?128:16, nq=lay?128:17; uint64_t pbase=sess+(lay?0x1493c0:0x1270), qbase=sess+(lay?0x1381c0:0x58); int n=0;
        for(int i=0;i<np && n<max;i++){ if(!((pm[i>>3]>>(i&7))&1)) continue; uint8_t r[0xc8]; if(!hb_read(pbase+(uint64_t)i*0xc8,r,sizeof r) || !*(uint64_t*)(r+4)) continue;
            int pi=*(int32_t*)(r+0xc); if(pi<0 || pi>=nq || !((qm[pi>>3]>>(pi&7))&1)) continue;
            uint8_t q[0x10]; if(!hb_read(qbase+(uint64_t)pi*0x110,q,sizeof q)) continue;
            StSess*e=&out[n]; e->idx=i; e->peer=pi; e->voice=*(uint64_t*)q; e->muted=0; for(int k=0;k<nm;k++) if(muted[k]==e->voice){ e->muted=1; break; }
            int j=0, ok=1; const uint16_t*w=(const uint16_t*)(r+0x98);
            for(int c=0;c<16 && w[c] && j<60;c++){ uint16_t u=w[c]; if(u<0x20){ ok=0; break; }
                if(u<0x80) e->name[j++]=(char)u; else if(u<0x800){ e->name[j++]=(char)(0xC0|(u>>6)); e->name[j++]=(char)(0x80|(u&63)); }
                else { e->name[j++]=(char)(0xE0|(u>>12)); e->name[j++]=(char)(0x80|((u>>6)&63)); e->name[j++]=(char)(0x80|(u&63)); } }
            if(!ok || !j) continue; e->name[j]=0; n++; }
        if(n){ if(g_st_layout<0) g_st_layout=lay; return n; } best=n; }
    return best; }
// mute requests from the page, run on the halo3 main loop (hb_frame_hook)
static pthread_mutex_t g_vq_mx=PTHREAD_MUTEX_INITIALIZER; static struct { uint64_t id; uint8_t on; } g_vq[256]; static int g_vqn; volatile int g_st_voice_pending;
static void st_voice_jobs(void){
    uint64_t id[256]; uint8_t on[256]; int n;
    pthread_mutex_lock(&g_vq_mx); n=g_vqn; for(int i=0;i<n;i++){ id[i]=g_vq[i].id; on[i]=g_vq[i].on; } g_vqn=0; __atomic_store_n(&g_st_voice_pending,0,__ATOMIC_RELEASE); pthread_mutex_unlock(&g_vq_mx);
    for(int i=0;i<n && g_st_setmuted;i++){ ((void(__attribute__((ms_abi))*)(uint64_t,uint8_t))g_st_setmuted)(id[i],on[i]);
        char m[96]; int k=snprintf(m,sizeof m,"  [stats] voice %016llx %s\n",(unsigned long long)id[i],on[i]?"muted":"unmuted"); (void)!write(2,m,k); } }
static int st_voice_request(uint64_t id,int on){ if(!g_st_setmuted || !id) return 0;
    pthread_mutex_lock(&g_vq_mx); int ok=g_vqn<256; if(ok){ g_vq[g_vqn].id=id; g_vq[g_vqn].on=(uint8_t)!!on; g_vqn++; __atomic_store_n(&g_st_voice_pending,1,__ATOMIC_RELEASE); } pthread_mutex_unlock(&g_vq_mx); return ok; }

// ---- death heatmap + footprint, per map, kept across matches -------------------------------------------------------
// footprint: how often a live player stood in each 0.5-unit cell (it draws the map's playable layout over time);
// deaths: where each player died. Saved to ~/Library/Application Support/ReclaimerMac/heatmaps/<map>.bin every 30 s.
#define HM_CELL 0.5f
typedef struct { int32_t x,y; uint32_t n; } HmCell;
typedef struct { float x,y,z; int32_t team, slot; char name[48]; uint32_t match; float t; } HmDeath;
static struct { char map[64]; HmCell*cells; uint32_t cap, used; HmDeath*d; uint32_t nd, dcap, match, maxmatch; int dirty; double saved, match_t0; } g_hm;
static pthread_mutex_t g_hm_mx=PTHREAD_MUTEX_INITIALIZER;
static void hm_path(const char*map,char*out,size_t n){ const char*h=getenv("HOME"); snprintf(out,n,"%s/Library/Application Support/ReclaimerMac/heatmaps/%s.bin",h?h:"/tmp",map); }
static void hm_cell_add(int32_t x,int32_t y,uint32_t n){
    if(g_hm.used*10>=g_hm.cap*7){ uint32_t oc=g_hm.cap, nc=oc?oc*2:65536; HmCell*nt=calloc(nc,sizeof *nt), *ot=g_hm.cells; if(!nt) return;
        g_hm.cells=nt; g_hm.cap=nc; g_hm.used=0; for(uint32_t i=0;i<oc;i++) if(ot[i].n) hm_cell_add(ot[i].x,ot[i].y,ot[i].n); free(ot); }
    uint32_t m=g_hm.cap-1, i=((uint32_t)x*73856093u ^ (uint32_t)y*19349663u)&m;
    for(;;i=(i+1)&m){ HmCell*c=&g_hm.cells[i]; if(!c->n){ c->x=x; c->y=y; c->n=n; g_hm.used++; return; } if(c->x==x && c->y==y){ c->n+=n; return; } } }
static void hm_save(void){ if(!g_hm.map[0] || !g_hm.dirty) return; char p[1024]; hm_path(g_hm.map,p,sizeof p);
    char dir[1024]; snprintf(dir,sizeof dir,"%s",p); char*sl=strrchr(dir,'/'); if(sl){ *sl=0; mkdir(dir,0755); }
    char tmp[1100]; snprintf(tmp,sizeof tmp,"%s.tmp",p); FILE*f=fopen(tmp,"wb"); if(!f) return;
    float cell=HM_CELL; fwrite("RHM2",1,4,f); fwrite(&cell,4,1,f); fwrite(&g_hm.used,4,1,f); fwrite(&g_hm.nd,4,1,f); fwrite(&g_hm.maxmatch,4,1,f);
    for(uint32_t i=0;i<g_hm.cap;i++) if(g_hm.cells[i].n) fwrite(&g_hm.cells[i],sizeof(HmCell),1,f);
    fwrite(g_hm.d,sizeof(HmDeath),g_hm.nd,f); int ok=!ferror(f); fclose(f); if(ok) rename(tmp,p); else unlink(tmp); g_hm.dirty=0; }
static void hm_switch(const char*map){   // (g_hm_mx held)
    if(!strcmp(map,g_hm.map)) return; hm_save();
    free(g_hm.cells); free(g_hm.d); memset(&g_hm,0,sizeof g_hm); snprintf(g_hm.map,sizeof g_hm.map,"%s",map); if(!map[0]) return;
    char p[1024]; hm_path(map,p,sizeof p); FILE*f=fopen(p,"rb"); uint32_t nc=0,nd=0;
    if(f){ char mg[4]; float cell=0; if(fread(mg,1,4,f)==4 && !memcmp(mg,"RHM2",4) && fread(&cell,4,1,f)==1 && cell==HM_CELL && fread(&nc,4,1,f)==1 && fread(&nd,4,1,f)==1 && fread(&g_hm.maxmatch,4,1,f)==1 && nc<(1u<<24) && nd<(1u<<22)){
            for(uint32_t i=0;i<nc;i++){ HmCell c; if(fread(&c,sizeof c,1,f)!=1) break; hm_cell_add(c.x,c.y,c.n); }
            g_hm.d=malloc((nd?nd:1)*sizeof(HmDeath)); g_hm.dcap=nd?nd:1; g_hm.nd=g_hm.d?(uint32_t)fread(g_hm.d,sizeof(HmDeath),nd,f):0; }
        fclose(f); }
    g_hm.match=++g_hm.maxmatch; g_hm.match_t0=mono_s(); g_hm.saved=mono_s(); g_hm.dirty=0;
    char m[1200]; int k=snprintf(m,sizeof m,"  [stats] heatmap %s: %u cells, %u deaths from earlier matches\n",map,g_hm.used,g_hm.nd); (void)!write(2,m,k); }
// the map being played: switched to when there is something to record (menus open campaign maps for scenery)
static char g_hm_cur[64];
static void hm_death(const float*pos,int team,int slot,const char*name){   // (g_hm_mx held)
    hm_switch(g_hm_cur);
    if(g_hm.nd>=g_hm.dcap){ uint32_t nc=g_hm.dcap?g_hm.dcap*2:256; HmDeath*nd=realloc(g_hm.d,nc*sizeof *nd); if(!nd) return; g_hm.d=nd; g_hm.dcap=nc; }
    HmDeath*d=&g_hm.d[g_hm.nd++]; memset(d,0,sizeof *d); d->x=pos[0]; d->y=pos[1]; d->z=pos[2]; d->team=team; d->slot=slot; snprintf(d->name,sizeof d->name,"%s",name?name:"");
    d->match=g_hm.match; d->t=(float)(mono_s()-g_hm.match_t0); g_hm.dirty=1; }
static void hm_json(StBuf*s){ pthread_mutex_lock(&g_hm_mx);
    sb_put(s,"{\"map\":"); sb_str(s,g_hm.map); sb_put(s,",\"cell\":%.2f,\"match\":%u,\"cells\":[",HM_CELL,g_hm.match);
    int first=1; for(uint32_t i=0;i<g_hm.cap;i++) if(g_hm.cells[i].n){ sb_put(s,"%s%d,%d,%u",first?"":",",g_hm.cells[i].x,g_hm.cells[i].y,g_hm.cells[i].n); first=0; }
    sb_put(s,"],\"deaths\":["); for(uint32_t i=0;i<g_hm.nd;i++){ HmDeath*d=&g_hm.d[i]; sb_put(s,"%s[%.2f,%.2f,%.2f,%d,",i?",":"",d->x,d->y,d->z,d->team); sb_str(s,d->name); sb_put(s,",%u,%.0f,%d]",d->match,d->t,d->slot); }
    sb_put(s,"]}"); pthread_mutex_unlock(&g_hm_mx); }

// ---- per-player tracking: deaths, lives, distance, top speed, projectiles fired ----------------------------------
typedef struct { char name[64]; int slot, team, deaths, shots, hits, alive; uint32_t unit; double life_start, longest, dist; float pos[3], top_speed; } StTrack;
static StTrack g_trk[256]; static int g_ntrk; static char g_trk_map[64];
// keyed by player slot + name (names repeat: everyone who never set one is "Player")
static StTrack* st_track(const StPlayer*p){ for(int i=0;i<g_ntrk;i++) if(g_trk[i].slot==p->slot && !strcmp(g_trk[i].name,p->name)) return &g_trk[i];
    if(g_ntrk>=256) return 0; StTrack*t=&g_trk[g_ntrk++]; memset(t,0,sizeof *t); snprintf(t->name,sizeof t->name,"%s",p->name); t->slot=p->slot; t->team=-1; t->unit=0xFFFFFFFFu; return t; }
// bipeds seen last snapshot (death = a biped going from alive to dead), projectiles already counted
typedef struct { uint32_t h; int alive, ti; } StBip;
static StBip g_bip[2048]; static int g_nbip;
static struct { uint32_t h; double t; } g_proj[8192];

// Page controls (POST /control?do=<action>): the same switches as the function keys -- hitboxes (F2), tracers (F3),
// info (F4), items (F5), teams (F6: all -> enemies -> friends), teleport (F7, only with PELOAD_ALLOW_TELEPORT) and
// capture (F8). A POST with a custom header, which another web site's page cannot send to this port without a CORS
// preflight we never answer. PELOAD_STATS_READONLY=1 turns the controls off.
static int st_controls_ok(void){ static int ro=-1; if(ro<0){ const char*e=getenv("PELOAD_STATS_READONLY"); ro=e&&*e&&strcmp(e,"0")?1:0; } return !ro; }
static int st_control(const char*act,const char*qs){
    char m[96]; int k=0;
    static const char*const feat[HBF_N]={"hitboxes","tracers","info","items"};
    for(int f=0;f<HBF_N;f++) if(!strcmp(act,feat[f])){ __atomic_fetch_xor(&g_hb_toggle,1<<f,__ATOMIC_ACQ_REL);
        k=snprintf(m,sizeof m,"  [stats] page: %s %s\n",feat[f],hb_feat(f)?"on":"off"); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"teams")){ static const char*const tm[3]={"all","enemies only","friends only"}; __atomic_fetch_add(&g_hb_teamcycle,1,__ATOMIC_ACQ_REL);
        k=snprintf(m,sizeof m,"  [stats] page: showing %s\n",tm[hb_team_mode()]); (void)!write(2,m,k); return 1; }
    if(!strcmp(act,"teleport")){ if(!getenv("PELOAD_ALLOW_TELEPORT")) return 0; __atomic_store_n(&g_hb_job_pending,1,__ATOMIC_RELEASE); (void)!write(2,"  [teleport] page: requested\n",29); return 1; }
    if(!strcmp(act,"muteall") || !strcmp(act,"unmuteall")){ static StSess ss[128]; int n=st_session(ss,128,0), ok=0;
        for(int i=0;i<n;i++) if(ss[i].muted!=!strcmp(act,"muteall")) ok+=st_voice_request(ss[i].voice,!strcmp(act,"muteall"));
        char m[80]; int k=snprintf(m,sizeof m,"  [stats] page: %s (%d players)\n",act,ok); (void)!write(2,m,k); return n>0; }
    if(!strcmp(act,"mute") || !strcmp(act,"unmute")){ const char*v=strstr(qs,"voice="); unsigned long long id=v?strtoull(v+6,0,16):0;
        return st_voice_request(id,!strcmp(act,"mute")); }
    if(!strcmp(act,"burst")){ __atomic_store_n(&g_hb_cap_req,8,__ATOMIC_RELEASE); (void)!write(2,"  [capture] page: capturing the next 8 frames\n",47); return 1; }
    if(!strcmp(act,"capture")){ __atomic_store_n(&g_hb_cap_req,1,__ATOMIC_RELEASE); (void)!write(2,"  [capture] page: capturing the next frame\n",43); return 1; }
    return 0; }
static void st_snapshot(StBuf*s){
    static const char* teams[8]={"red","blue","green","orange","purple","gold","brown","pink"};
    extern double g_gameplay_t0; double now=mono_s();
    sb_put(s,"{\"time\":%.2f,\"in_match\":%s",g_gameplay_t0?now-g_gameplay_t0:0.0,g_gameplay_t0?"true":"false");
    char m[64]="";
    { char mp[1024]=""; pthread_mutex_lock(&g_hb_mapmx);   // newest map the game opened from halo3/maps (not a cache file)
      for(int k=0;k<g_hb_nmappaths;k++) if(strstr(g_hb_mappaths[k],"/halo3/maps/")){ snprintf(mp,sizeof mp,"%s",g_hb_mappaths[k]); break; }
      if(!mp[0] && g_hb_nmappaths) snprintf(mp,sizeof mp,"%s",g_hb_mappaths[0]);
      pthread_mutex_unlock(&g_hb_mapmx);
      const char*b=strrchr(mp,'/'); b=b?b+1:mp; snprintf(m,sizeof m,"%s",b); char*dot=strrchr(m,'.'); if(dot)*dot=0; sb_put(s,",\"map\":"); sb_str(s,m); }
    { static int tp=-1; if(tp<0) tp=getenv("PELOAD_ALLOW_TELEPORT")?1:0; static const char*const tm[3]={"all","enemies","friends"};
      sb_put(s,",\"overlay\":{\"hitboxes\":%s,\"tracers\":%s,\"info\":%s,\"items\":%s,\"teams\":\"%s\",\"teleport\":%s,\"controls\":%s}",
          hb_feat(HBF_HITBOX)?"true":"false",hb_feat(HBF_TRACER)?"true":"false",hb_feat(HBF_INFO)?"true":"false",hb_feat(HBF_ITEMS)?"true":"false",
          tm[hb_team_mode()],tp?"true":"false",st_controls_ok()?"true":"false"); }
    if(strcmp(m,g_trk_map)){ snprintf(g_trk_map,sizeof g_trk_map,"%s",m); g_ntrk=0; g_nbip=0; memset(g_proj,0,sizeof g_proj); g_hb_local_slot=-1; }   // new map: new match stats
    pthread_mutex_lock(&g_hm_mx); snprintf(g_hm_cur,sizeof g_hm_cur,"%s",m); if(g_hm.dirty && now-g_hm.saved>30){ hm_save(); g_hm.saved=now; } pthread_mutex_unlock(&g_hm_mx);
    static uint8_t tab[8192*0x18]; int hi=0; uint64_t arr=hb_object_array(); uint8_t hdr[0x60];
    if(arr && hb_read(arr,hdr,sizeof hdr) && *(uint32_t*)(hdr+0x2c)==0x64407440 && *(int32_t*)(hdr+0x24)==0x18){
        hi=*(int32_t*)(hdr+0x3c); if(hi<0||hi>8192||hi>*(int32_t*)(hdr+0x20) || !hb_read(*(uint64_t*)(hdr+0x48),tab,(size_t)hi*0x18)) hi=0; }
    #define ST_DATUM(h) ((((h)&0xFFFF)<(uint32_t)hi && (h)!=0xFFFFFFFFu && *(uint16_t*)(tab+((h)&0xFFFF)*0x18)==(uint16_t)((h)>>16)) ? *(uint64_t*)(tab+((h)&0xFFFF)*0x18+0x10) : 0)
    #define ST_TYPE(h) (tab[((h)&0xFFFF)*0x18+3])
    StPlayer pl[256]; int np=st_players(pl,256);
    float cam[3]={0}, fwd[3]={0}; int camok=0; { float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); if(!(s0&1) && g_hb_vpseq==s0 && (vp[28]||vp[29]||vp[30])){ memcpy(cam,vp+28,12);
        float l=sqrtf(vp[12]*vp[12]+vp[13]*vp[13]+vp[14]*vp[14]); if(l>0){ fwd[0]=vp[12]/l; fwd[1]=vp[13]/l; fwd[2]=vp[14]/l; camok=1; } } }
    // The local player (stats "you", the overlay's friend/enemy split). Halo 3's player mapping (TLS +0x110; layout in
    // shim/docs/halo3-variant-traits.md section 6): output user 0..3 -> player handle at +0xB8 -- what the HUD and
    // cameras use, right in third person, vehicles and while dead. Else a player whose machine index (+0x1A) is the
    // local machine (players globals TLS +0x40, +0x7C). Last resort: the biped the camera is inside / just ahead on
    // the view ray. The slot is kept while none of these answers.
    int mapped=-1;
    { uint64_t pm=0; int32_t ou[4]; if(g_st_tls && hb_read(g_st_tls+0x110,&pm,8) && pm && hb_read(pm+0xB8,ou,16))
          for(int u=0;u<4 && mapped<0;u++) if(ou[u]!=-1) for(int k=0;k<np;k++) if(pl[k].slot==(ou[u]&0xFFFF)){ mapped=pl[k].slot; break; }
      uint64_t pg=0; int32_t lm=-1;
      if(mapped<0 && g_st_tls && hb_read(g_st_tls+0x40,&pg,8) && pg && hb_read(pg+0x7C,&lm,4) && lm>=0)
          for(int k=0;k<np;k++){ int16_t mi=-1; if(hb_read(pl[k].rec+0x1A,&mi,2) && mi==lm){ mapped=pl[k].slot; break; } } }
    if(mapped>=0) g_hb_local_slot=mapped;
    else if(camok){ uint32_t best=0xFFFFFFFFu; float bd=1e9f; int inside=0;
        for(int k=0;k<np;k++){ uint32_t h=pl[k].unit; uint64_t p=ST_DATUM(h); float c[4]; if(!p || ST_TYPE(h)!=0 || !hb_read(p+0x1c,c,16)) continue;
            float v[3]={c[0]-cam[0],c[1]-cam[1],c[2]-cam[2]}, d2=v[0]*v[0]+v[1]*v[1]+v[2]*v[2], r=c[3];
            if(d2<(r+0.3f)*(r+0.3f)){ best=h; inside=1; break; }
            float a=v[0]*fwd[0]+v[1]*fwd[1]+v[2]*fwd[2], perp2=d2-a*a; if(a>0.3f && a<6.0f && perp2<(r+0.4f)*(r+0.4f) && a<bd){ bd=a; best=h; } }
        (void)inside; if(best!=0xFFFFFFFFu) for(int k=0;k<np;k++) if(pl[k].unit==best){ g_hb_local_slot=pl[k].slot; break; } }
    g_hb_local_unit=0xFFFFFFFFu; for(int k=0;k<np;k++) if(pl[k].slot==g_hb_local_slot){ g_hb_local_unit=pl[k].unit; break; }
    // deaths: every biped, alive -> dead (the body stays a while, so 10 Hz catches it); the player it belonged to.
    // Positions are the world-space bounding centre (+0x1c): the origin (+0x50) is vehicle-relative when seated.
    { StBip nb[2048]; int nn=0;
      for(int i=0;i<hi && nn<2048;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=0) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x178]; if(!p || !hb_read(p,d,sizeof d)) continue;
          uint32_t h=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; int alive=!((*(const uint32_t*)(d+0x110)>>2)&1), ti=-1;
          for(int k=0;k<np;k++) if(pl[k].unit==h){ StTrack*t=st_track(&pl[k]); if(t) ti=(int)(t-g_trk); break; }
          int prev=-1; for(int b=0;b<g_nbip;b++) if(g_bip[b].h==h){ prev=b; break; }
          if(prev>=0){ if(ti<0) ti=g_bip[prev].ti;
              if(g_bip[prev].alive && !alive){ StTrack*t=ti>=0?&g_trk[ti]:0; int team=*(const int32_t*)(d+0x174);
                  if(t){ t->deaths++; t->alive=0; if(now-t->life_start>t->longest) t->longest=now-t->life_start; }
                  if(t || getenv("PELOAD_STATS_ALL_BIPEDS")){ pthread_mutex_lock(&g_hm_mx); hm_death((const float*)(d+0x1c),team,t?t->slot:-1,t?t->name:""); pthread_mutex_unlock(&g_hm_mx); } } }
          nb[nn].h=h; nb[nn].alive=alive; nb[nn].ti=ti; nn++; }
      memcpy(g_bip,nb,(size_t)nn*sizeof *nb); g_nbip=nn; }
    // shots: every projectile that ended since the last snapshot (hitbox.c's object_delete hook), with the biped it
    // hit if any; without the hook, count the projectiles seen in flight (misses most bullets)
    if(g_hb_delhook){ static unsigned seen; unsigned n=__atomic_load_n(&g_hbshot_n,__ATOMIC_ACQUIRE); if(n-seen>8192) seen=n-8192;
        for(;seen!=n;seen++){ const HbShot*r=&g_hbshot[seen&8191];
            for(int k=0;k<np;k++) if(pl[k].unit==r->owner){ StTrack*t=st_track(&pl[k]); if(t){ t->shots++; if(r->victim!=0xFFFFFFFFu) t->hits++; } break; } } }
    else for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=5) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint32_t own=0;
        uint32_t h=((uint32_t)*(const uint16_t*)e<<16)|(uint32_t)i; uint32_t slot=(h*2654435761u)>>19, j=slot; int known=0;
        for(int q=0;q<16;q++,j=(j+1)&8191){ if(g_proj[j].h==h && now-g_proj[j].t<60){ known=1; break; } if(!g_proj[j].h || now-g_proj[j].t>=60) break; }
        if(known) continue; g_proj[j].h=h; g_proj[j].t=now;
        if(p && hb_read(p+0xC0,&own,4)) for(int k=0;k<np;k++) if(pl[k].unit==own){ StTrack*t=st_track(&pl[k]); if(t) t->shots++; break; } }
    // players: the engine's player table (a player between lives has no live biped)
    st_variant(s);   // the game's options and trait sets (st_players found the TLS block)
    static StSess ss[128]; uint32_t sst=0; int nss=st_session(ss,128,&sst);
    sb_put(s,",\"session\":["); for(int k=0;k<nss;k++){ sb_put(s,"%s{\"name\":",k?",":""); sb_str(s,ss[k].name);
        sb_put(s,",\"index\":%d,\"voice\":\"%016llx\",\"muted\":%s,\"talking\":%s}",ss[k].idx,(unsigned long long)ss[k].voice,ss[k].muted?"true":"false",st_talk_age(ss[k].voice)<0.5?"true":"false"); }
    sb_put(s,"],\"session_state\":%u,\"mute\":%s",sst,g_st_setmuted&&st_controls_ok()?"true":"false");
    sb_put(s,",\"players\":["); int first=1;
    for(int k=0;k<np;k++){ StTrack*t=st_track(&pl[k]); uint32_t h=pl[k].unit; uint64_t p=ST_DATUM(h); static uint8_t d[0x380];
        int have = p && ST_TYPE(h)==0 && hb_read(p,d,sizeof d); int dead = !have || ((*(const uint32_t*)(d+0x110)>>2)&1);
        // world-space centre (the origin at +0x50 is vehicle-relative when seated)
        const float*o=have?(const float*)(d+0x1c):(t?t->pos:(const float[3]){0,0,0}); float spd=0; int team=have?*(const int32_t*)(d+0x174):(t?t->team:-1);
        if(have){ const float*v=(const float*)(d+0x74); spd=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
        if(t){ if(have && !dead){ if(!t->alive || t->unit!=h){ t->alive=1; t->life_start=now; }
                   else { float dx=o[0]-t->pos[0],dy=o[1]-t->pos[1],dz=o[2]-t->pos[2],dd=sqrtf(dx*dx+dy*dy+dz*dz); if(dd<20) t->dist+=dd; }
                   if(spd>t->top_speed && spd<50) t->top_speed=spd; memcpy(t->pos,o,12);
                   if(g_gameplay_t0){ pthread_mutex_lock(&g_hm_mx); hm_switch(g_hm_cur); hm_cell_add((int32_t)floorf(o[0]/HM_CELL),(int32_t)floorf(o[1]/HM_CELL),1); g_hm.dirty=1; pthread_mutex_unlock(&g_hm_mx); } }
               else if(t->alive && !have){ t->alive=0; }   // biped gone without us seeing it die
               t->unit=h; if(team>=0) t->team=team; }
        int you=pl[k].slot==g_hb_local_slot;
        sb_put(s,"%s{\"name\":",first?"":","); first=0; sb_str(s,pl[k].name); sb_put(s,",\"slot\":%d",pl[k].slot);
        sb_put(s,",\"handle\":\"%08x\",\"team\":%d,\"team_name\":\"%s\",\"you\":%s,\"alive\":%s,\"health\":%.3f,\"shield\":%.3f,\"camo\":%.3f",
               h,team,(team>=0&&team<8)?teams[team]:"none",you?"true":"false",dead?"false":"true",dead?0:*(const float*)(d+0xF4),dead?0:*(const float*)(d+0xF8),have?*(const float*)(d+0x360):0);
        sb_put(s,",\"weapon\":");
        if(have && !dead){ int ws=*(const int8_t*)(d+0x262); const uint32_t*wp=(const uint32_t*)(d+0x268); uint64_t wd=(ws>=0&&ws<4)?ST_DATUM(wp[ws]):0; uint32_t def=0; if(wd && hb_read(wd,&def,4)) st_tag(s,def); else sb_put(s,"null");
            sb_put(s,",\"weapons\":["); for(int w=0,fw=1;w<4;w++){ uint64_t wd2=ST_DATUM(wp[w]); uint32_t df=0; if(!wd2 || !hb_read(wd2,&df,4)) continue; sb_put(s,"%s",fw?"":","); fw=0; st_tag(s,df); } sb_put(s,"]");
            const int8_t*gr=(const int8_t*)(d+0x294); sb_put(s,",\"grenades\":{\"frag\":%d,\"plasma\":%d,\"spike\":%d,\"firebomb\":%d}",gr[0],gr[1],gr[2],gr[3]);
            uint32_t parent=*(const uint32_t*)(d+0x10); sb_put(s,",\"vehicle\":"); { uint64_t vd=parent!=0xFFFFFFFFu?ST_DATUM(parent):0; uint32_t df=0; if(vd && hb_read(vd,&df,4)){ st_tag(s,df); sb_put(s,",\"seat\":%d",*(const int16_t*)(d+0x24E)); } else sb_put(s,"null"); }
            const float*fw=(const float*)(d+0x5c); sb_put(s,",\"facing\":%.0f",atan2f(fw[1],fw[0])*57.2958f); }
        else sb_put(s,"null,\"weapons\":[],\"grenades\":{\"frag\":0,\"plasma\":0,\"spike\":0,\"firebomb\":0},\"vehicle\":null");
        sb_put(s,",\"pos\":"); sb_f3(s,o); sb_put(s,",\"speed\":%.2f",spd);
        if(t) sb_put(s,",\"deaths\":%d,\"hits\":%d,\"accuracy\":%.3f,\"shots\":%d,\"life\":%.1f,\"longest_life\":%.1f,\"distance\":%.1f,\"top_speed\":%.2f",t->deaths,t->hits,t->shots?(double)t->hits/t->shots:0.0,t->shots,t->alive?now-t->life_start:0.0,
                     t->longest>(t->alive?now-t->life_start:0)?t->longest:(t->alive?now-t->life_start:0),t->dist,t->top_speed);
        if(g_st_var_ok) st_player_traits(s,pl[k].rec);
        { const StSess*vs=0; for(int q=0;q<nss && !vs;q++) if(ss[q].idx==pl[k].slot && !strcmp(ss[q].name,pl[k].name)) vs=&ss[q];
          for(int q=0;q<nss && !vs;q++) if(!strcmp(ss[q].name,pl[k].name)) vs=&ss[q];
          if(vs) sb_put(s,",\"voice\":\"%016llx\",\"muted\":%s,\"talking\":%s",(unsigned long long)vs->voice,vs->muted?"true":"false",st_talk_age(vs->voice)<0.5?"true":"false"); }
        sb_put(s,"}"); }
    sb_put(s,"]");
    // vehicles and projectiles in flight
    sb_put(s,",\"vehicles\":["); first=1;
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=1) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x120]; if(!p || !hb_read(p,d,sizeof d)) continue;
        int dead=(*(const uint32_t*)(d+0x110)>>2)&1; sb_put(s,"%s{\"type\":",first?"":","); first=0; st_tag(s,*(const uint32_t*)d);
        sb_put(s,",\"health\":%.3f,\"shield\":%.3f,\"pos\":",dead?0:*(const float*)(d+0xF4),dead?0:*(const float*)(d+0xF8)); sb_f3(s,(const float*)(d+0x50)); sb_put(s,"}"); }
    sb_put(s,"],\"projectiles\":["); first=1;
    for(int i=0;i<hi;i++){ const uint8_t*e=tab+i*0x18; if(!*(const uint16_t*)e || e[3]!=5) continue; uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0xC8]; if(!p || !hb_read(p,d,sizeof d)) continue;
        uint32_t own=*(const uint32_t*)(d+0xC0); const char*on=0; for(int k=0;k<np;k++) if(pl[k].unit==own){ on=pl[k].name; break; }
        const float*v=(const float*)(d+0x74); sb_put(s,"%s{\"type\":",first?"":","); first=0; st_tag(s,*(const uint32_t*)d);
        sb_put(s,",\"owner\":"); sb_str(s,on?on:""); sb_put(s,",\"pos\":"); sb_f3(s,(const float*)(d+0x50)); sb_put(s,",\"speed\":%.1f}",sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2])); }
    sb_put(s,"]}");
    #undef ST_DATUM
    #undef ST_TYPE
}
static void* st_snap_thread(void*a){ (void)a; pthread_setname_np("shim-stats-snap");
    for(;;){ StBuf s={0}; st_snapshot(&s); if(s.b){ pthread_mutex_lock(&g_st_mx); free(g_st_json); g_st_json=s.b; g_st_len=s.n; g_st_seq++; pthread_cond_broadcast(&g_st_cv); pthread_mutex_unlock(&g_st_mx); }
        usleep(100000); }
    return 0; }

// the page: shim/src/stats_page.html, embedded at build time
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char g_st_page[]={
#embed "stats_page.html"
,0};
#pragma clang diagnostic pop
static int st_send(int fd,const char*b,size_t n){ while(n){ ssize_t k=send(fd,b,n,0); if(k<=0){ if(k<0&&errno==EINTR) continue; return 0; } b+=k; n-=(size_t)k; } return 1; }
static void* st_conn(void*a){ int fd=(int)(intptr_t)a; pthread_setname_np("shim-stats-http");
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one); setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
    char req[2048]; ssize_t n=recv(fd,req,sizeof req-1,0); if(n<=0){ close(fd); return 0; } req[n]=0;
    char meth[8]="", path[256]="/", qs[128]=""; sscanf(req,"%7s %255s",meth,path); char*q=strchr(path,'?'); if(q){ snprintf(qs,sizeof qs,"%s",q+1); *q=0; }
    char h[512];
    if(!strcmp(path,"/control")){
        // POST + X-Reclaimer-Control header (see st_controls_ok); ?do=<action>
        char act[32]=""; const char*d=strstr(qs,"do="); if(d) sscanf(d+3,"%31[a-z]",act);
        int hdr=strcasestr(req,"\r\nX-Reclaimer-Control:")!=0, code=!strcmp(meth,"POST")&&hdr?(st_controls_ok()?(st_control(act,qs)?200:400):403):403;
        const char*body=code==200?"{\"ok\":true}":code==400?"{\"ok\":false,\"error\":\"unknown or disabled action\"}":!st_controls_ok()?"{\"ok\":false,\"error\":\"controls are off\"}":"{\"ok\":false,\"error\":\"needs POST with an X-Reclaimer-Control header\"}";
        int k=snprintf(h,sizeof h,"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",code,code==200?"OK":code==400?"Bad Request":"Forbidden",strlen(body),body);
        st_send(fd,h,(size_t)k); }
    else if(strcmp(meth,"GET")){ const char*na="HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,na,strlen(na)); }
    else if(!strcmp(path,"/events")){
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\n\r\n");
        if(st_send(fd,h,(size_t)k)) for(unsigned long seen=0;;){ pthread_mutex_lock(&g_st_mx); while(g_st_seq==seen) pthread_cond_wait(&g_st_cv,&g_st_mx);
                seen=g_st_seq; size_t len=g_st_len; char*j=malloc(len+16); if(j){ memcpy(j,"data: ",6); memcpy(j+6,g_st_json,len); memcpy(j+6+len,"\n\n",2); } pthread_mutex_unlock(&g_st_mx);
                int ok=j && st_send(fd,j,len+8); free(j); if(!ok) break; } }
    else if(!strcmp(path,"/heat.json")){
        StBuf b={0}; hm_json(&b);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",b.n);
        if(st_send(fd,h,(size_t)k) && b.n) st_send(fd,b.b,b.n); free(b.b); }
    else if(!strcmp(path,"/stats.json")){
        pthread_mutex_lock(&g_st_mx); size_t len=g_st_len; char*j=malloc(len+1); if(j && g_st_json) memcpy(j,g_st_json,len); else len=0; pthread_mutex_unlock(&g_st_mx);
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",len);
        if(st_send(fd,h,(size_t)k) && len) st_send(fd,j,len); free(j); }
    else if(!strcmp(path,"/")){
        // PELOAD_STATS_PAGE=<file>: serve the page from disk on every request (play.sh sets it in a source checkout, so
        // page edits show on a browser refresh); otherwise the copy embedded at build time
        const char*pg=g_st_page; size_t pn=sizeof g_st_page-1; char*disk=0; const char*pf=getenv("PELOAD_STATS_PAGE");
        if(pf && *pf){ FILE*f=fopen(pf,"rb"); if(f){ fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                if(n>0 && n<(8<<20) && (disk=malloc((size_t)n)) && fread(disk,1,(size_t)n,f)==(size_t)n){ pg=disk; pn=(size_t)n; } fclose(f); } }
        int k=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",pn);
        if(st_send(fd,h,(size_t)k)) st_send(fd,pg,pn); free(disk); }
    else { const char*nf="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; st_send(fd,nf,strlen(nf)); }
    close(fd); return 0; }
static void* st_listen_thread(void*a){ (void)a; pthread_setname_np("shim-stats-listen");
    // only the process that runs the engine serves (the client's launcher process inherits the variable too)
    while(!hb_halo3_base()) usleep(500000);
    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons((uint16_t)g_st_port);
    const char*ad=getenv("PELOAD_STATS_ADDR"); sa.sin_addr.s_addr=inet_addr(ad&&*ad?ad:"127.0.0.1");
    char m[160]; int k;
    if(ls<0 || bind(ls,(struct sockaddr*)&sa,sizeof sa) || listen(ls,16)){ k=snprintf(m,sizeof m,"  [stats] cannot listen on %s:%d (%s)\n",ad&&*ad?ad:"127.0.0.1",g_st_port,strerror(errno)); (void)!write(2,m,k); if(ls>=0) close(ls); return 0; }
    k=snprintf(m,sizeof m,"  [stats] live game stats on http://%s:%d/ (also /stats.json, /events)\n",ad&&*ad?ad:"127.0.0.1",g_st_port); (void)!write(2,m,k);
    pthread_t t; pthread_create(&t,0,st_snap_thread,0); pthread_detach(t);
    for(;;){ int fd=accept(ls,0,0); if(fd<0){ if(errno==EINTR) continue; usleep(100000); continue; } pthread_t c; if(pthread_create(&c,0,st_conn,(void*)(intptr_t)fd)) close(fd); else pthread_detach(c); }
    return 0; }
static void stats_start(void){ if(!stats_on()) return; pthread_t t; pthread_create(&t,0,st_listen_thread,0); pthread_detach(t); }

// ---- PELOAD_TEST_BOOT_PROMPT=1: open halo3's native "boot the betrayer?" popup on demand (test hook) --------------
// halo3+0xfcc20 shows the popup while a player is dead (player +0x28 unit = NONE) and its record holds a betrayer
// handle at +0x1F0 (and the game variant has betrayal booting on). When the local player (the one whose biped held the
// camera; or PELOAD_TEST_BOOT_PROMPT=<its name>) is dead and has no betrayer, this names the first other player as the
// betrayer, once per death.
static void* st_bootprompt_thread(void*a){ (void)a; pthread_setname_np("shim-test-bootprompt"); int local=-1; uint32_t done_for=0xFFFFFFFFu;
    for(;;){ usleep(200000);
        uint8_t*h3=hb_halo3_base(); uint32_t idx=0; if(!h3 || !hb_read((uint64_t)(h3+0xa39f9c),&idx,4) || idx>=128) continue;
        void**arrs[1025]; int na=0; arrs[na++]=g_tls_array; pthread_mutex_lock(&g_tlsm); for(int q=0;q<g_ntarrs&&na<1025;q++) arrs[na++]=g_tarrs[q]; pthread_mutex_unlock(&g_tlsm);
        uint64_t pd=0; int32_t es=0, hi=0;
        for(int q=0;q<na;q++){ uint64_t blk=(uint64_t)arrs[q][idx], parr=0, oarr=0; uint8_t hdr[0x60];
            if(!blk || !hb_read(blk+0x38,&oarr,8) || !oarr || !hb_read(blk+0x10,&parr,8) || !parr || !hb_read(parr,hdr,sizeof hdr) || *(uint32_t*)(hdr+0x2c)!=0x64407440) continue;
            es=*(int32_t*)(hdr+0x24); hi=*(int32_t*)(hdr+0x3c); pd=*(uint64_t*)(hdr+0x48); break; }
        if(!pd || es<0x200 || hi<=0 || hi>256) continue;
        // the local player: its biped contains the camera
        float vp[32]; unsigned s0=g_hb_vpseq; memcpy(vp,g_hb_vp,sizeof vp); int camok=!(s0&1) && g_hb_vpseq==s0 && (vp[28]||vp[29]||vp[30]);
        uint64_t arr=hb_object_array(); uint8_t ohdr[0x60]; uint64_t odata=0; int ohi=0;
        if(arr && hb_read(arr,ohdr,sizeof ohdr) && *(uint32_t*)(ohdr+0x2c)==0x64407440){ odata=*(uint64_t*)(ohdr+0x48); ohi=*(int32_t*)(ohdr+0x3c); }
        const char*want=getenv("PELOAD_TEST_BOOT_PROMPT"); int byname=want && strcmp(want,"1");
        for(int i=0;i<hi && camok && odata && !byname;i++){ uint8_t rec[0x30]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue; uint32_t u=*(uint32_t*)(rec+0x28);
            if(u==0xFFFFFFFFu || (int)(u&0xFFFF)>=ohi) continue; uint8_t e[0x18]; uint64_t p=0; float c[4];
            if(!hb_read(odata+(uint64_t)(u&0xFFFF)*0x18,e,sizeof e) || *(uint16_t*)e!=(uint16_t)(u>>16) || !(p=*(uint64_t*)(e+0x10)) || !hb_read(p+0x1c,c,16)) continue;
            float dx=c[0]-vp[28],dy=c[1]-vp[29],dz=c[2]-vp[30]; if(dx*dx+dy*dy+dz*dz<(c[3]+0.3f)*(c[3]+0.3f)){ if(local!=i){ local=i; char m[80]; int k=snprintf(m,sizeof m,"  [bootprompt] local player = slot %d\n",i); (void)!write(2,m,k); } break; } }
        // PELOAD_TEST_BOOT_PROMPT=<name>: the local player by name (works before the first spawn, while still dead)
        { const char*nm=getenv("PELOAD_TEST_BOOT_PROMPT"); if(nm && strcmp(nm,"1")) for(int i=0;i<hi;i++){ uint8_t rec[0x98]; if(!hb_read(pd+(uint64_t)i*es,rec,sizeof rec) || !*(uint16_t*)rec) continue;
              char n8[40]; int j=0; const uint16_t*w=(const uint16_t*)(rec+0x58); for(int c=0;c<32 && w[c] && j<38;c++) n8[j++]=(char)(w[c]<128?w[c]:'?'); n8[j]=0;
              if(!strcmp(n8,nm)){ if(local!=i){ local=i; char m[80]; int k=snprintf(m,sizeof m,"  [bootprompt] local player %s = slot %d\n",nm,i); (void)!write(2,m,k); } break; } } }
        if(local<0) continue;
        uint8_t*me=(uint8_t*)(uintptr_t)(pd+(uint64_t)local*es); uint32_t unit=*(volatile uint32_t*)(me+0x28), betrayer=*(volatile uint32_t*)(me+0x1F0);
        if(unit!=0xFFFFFFFFu){ done_for=0xFFFFFFFFu; continue; }   // alive
        if(betrayer!=0xFFFFFFFFu || done_for==1) continue;
        for(int i=0;i<hi;i++){ if(i==local) continue; uint16_t salt=*(volatile uint16_t*)(uintptr_t)(pd+(uint64_t)i*es); if(!salt) continue;
            uint32_t h=((uint32_t)salt<<16)|(uint32_t)i; *(volatile uint32_t*)(me+0x1F0)=h; done_for=1;
            char m[120]; int k=snprintf(m,sizeof m,"  [bootprompt] local player (slot %d) is dead: betrayer set to player %08x\n",local,h); (void)!write(2,m,k); break; } }
    return 0; }
static void bootprompt_test_start(void){ if(!getenv("PELOAD_TEST_BOOT_PROMPT")) return; pthread_t t; pthread_create(&t,0,st_bootprompt_thread,0); pthread_detach(t); }
