#!/bin/bash
# Offline test of the spawn / pickup events (backlog 47; no game): extracts the REAL st_spawns code from shim/src/stats.c
# (from "// Spawns and pickups" up to st_snapshot, plus st_sentence), compiles it natively with stubs for the engine
# reads (an in-memory object table, player records and a fake map's tag names, all real stock tag paths) and drives it
# snapshot by snapshot (100 ms apart), checking the events it emits:
#   match start: a weapon burst = one summary line; vehicles and power-ups one by one with (x, y) + pos; a vehicle's
#   attached turret, the deployed auto turret / bubble shield, grenades and the permanent grav lift left out; an object
#   attached right after its creation (a loadout weapon) is no spawn and no pickup; a weapon's first holder = picked up
#   (team from the unit); respawns later; power-ups taken via the player's pickup tick (gone the same snapshot, or
#   still there), a first sight of a tick = nothing; slot reuse; unknown tags; a map change starts over.
#   bash shim/tools/spawn-test.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/spawntest.XXXXXX"); trap 'rm -rf "$T"' EXIT
cut_block(){ awk -v s="$2" -v e="$3" 'index($0,s)==1{on=1} on && index($0,e)==1 && !(index($0,s)==1){exit} on{print}' "$1"; }
{
cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
// ---- stubs for what st_spawns reads (hitbox.c / stats.c / peload.c) ----
typedef struct { uint32_t unit; int slot; char name[64]; uint64_t rec; } StPlayer;
typedef struct { char path[1024]; int fd; uint32_t tagoff; uint64_t vbase, fsize; int ntags; uint32_t*tp; uint16_t*salt; uint32_t*grp; char*names; int32_t*noff; } HbMap;
#define HB_NPATHS 32
static char g_hb_mappaths[HB_NPATHS][1024]; static int g_hb_nmappaths; static pthread_mutex_t g_hb_mapmx=PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned g_hb_mapgen; static double g_gameplay_t0=1;
static HbMap g_map; static char g_names[8192]; static int32_t g_noff[64]; static uint16_t g_salt[64];
static int hb_current_map(char*out,size_t n){ snprintf(out,n,"%s","/maps/sidewinder.map"); return 1; }
static HbMap* hbm_get(const char*p){ return strcmp(p,"/maps/sidewinder.map")?0:&g_map; }
static const char* hb_tag_name(uint32_t def){ (void)def; return "?"; }
static int hb_read(uint64_t a,void*d,size_t n){ if(!a) return 0; memcpy(d,(const void*)(uintptr_t)a,n); return 1; }
static __thread const float*t_st_evpos;
typedef struct { char kind[12], a[64], how[64], verb[24], text[400]; int ta, haspos; float pos[3]; } Ev;
static Ev g_evs[256]; static int g_nev;
static void st_event_verb(double at,const char*kind,const char*a,const char*b,const char*how,const char*verb,int ta,int tb,int flags,const char*fmt,...){
    (void)at; (void)b; (void)tb; (void)flags; Ev*e=&g_evs[g_nev++&255]; memset(e,0,sizeof *e);
    snprintf(e->kind,sizeof e->kind,"%s",kind); snprintf(e->a,sizeof e->a,"%s",a?a:""); snprintf(e->how,sizeof e->how,"%s",how?how:""); snprintf(e->verb,sizeof e->verb,"%s",verb?verb:"");
    e->ta=ta; if(t_st_evpos){ memcpy(e->pos,t_st_evpos,12); e->haspos=1; }
    va_list ap; va_start(ap,fmt); vsnprintf(e->text,sizeof e->text,fmt,ap); va_end(ap); }
EOF
cut_block src/stats.c '// tag names (hb_tag_name: "GAUSS BULLET") in sentence case' 'static void st_death_event'
cut_block src/stats.c '// Spawns and pickups (backlog 47' 'static void st_snapshot(StBuf*s){'
cat <<'EOF'
// ---- the fake game ----
static const char*const TAGS[]={ 0,
 "objects\\weapons\\rifle\\battle_rifle\\battle_rifle",                 // 1
 "objects\\weapons\\rifle\\sniper_rifle\\sniper_rifle",                 // 2
 "objects\\equipment\\bubbleshield_equipment\\bubbleshield_equipment",  // 3
 "objects\\equipment\\bubbleshield\\bubbleshield",                      // 4 deployed
 "objects\\weapons\\grenade\\frag_grenade\\frag_grenade",               // 5
 "objects\\multi\\powerups\\powerup_red\\powerup_red",                  // 6
 "objects\\multi\\powerups\\powerup_blue\\powerup_blue",                // 7
 "objects\\vehicles\\wraith\\wraith",                                   // 8
 "objects\\vehicles\\wraith\\turrets\\mortar\\mortar",                  // 9 attached
 "objects\\equipment\\autoturret\\autoturret",                          // 10 deployed (vehi)
 "objects\\vehicles\\warthog\\warthog_snow",                            // 11
 "objects\\levels\\dlc\\sidewinder\\hornet_lite\\hornet_lite",          // 12
 "objects\\equipment\\gravlift_permanent\\gravlift_permanent",          // 13
 "objects\\characters\\masterchief\\mp_masterchief\\mp_masterchief",    // 14 (biped)
 "objects\\powerups\\shotgun_ammo\\shotgun_ammo" };                     // 15 ammo pack: a weapon-kind item
#define NT ((int)(sizeof TAGS/sizeof *TAGS))
static uint32_t DEF(int ti){ return (uint32_t)(0x1000+ti)<<16 | (uint32_t)ti; }
static uint8_t tab[8192*0x18]; static uint8_t dat[64][0x400]; static int hi;
static void obj(int i,int salt,int type,int ti,uint32_t parent,float x,float y,float z){   // create / rewrite object slot i
    uint8_t*e=tab+i*0x18; memset(e,0,0x18); *(uint16_t*)e=(uint16_t)salt; e[3]=(uint8_t)type; *(uint64_t*)(e+0x10)=(uint64_t)(uintptr_t)dat[i];
    memset(dat[i],0,sizeof dat[i]); *(uint32_t*)dat[i]=ti>0?DEF(ti):0xDEAD0063u; *(uint32_t*)(dat[i]+0x10)=parent; float p[3]={x,y,z}; memcpy(dat[i]+0x50,p,12); memcpy(dat[i]+0x1c,p,12);
    if(i>=hi) hi=i+1; }
static void del(int i){ memset(tab+i*0x18,0,0x18); }
static void parent(int i,uint32_t p){ *(uint32_t*)(dat[i]+0x10)=p; }
static uint32_t H(int i){ return (uint32_t)*(uint16_t*)(tab+i*0x18)<<16 | (uint32_t)i; }
static uint8_t rec[4][0x300]; static StPlayer pl[4]; static int np;
static double now=100; static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static int base;   // events before the current step
static void snap(void){ base=g_nev; st_spawns(tab,hi,pl,np,"sidewinder",now); now+=0.1;
    for(int k=base;k<g_nev;k++){ Ev*e=&g_evs[k&255]; printf("    %-8s %-10s %s",e->kind,e->verb,e->text); if(e->haspos) printf("   pos [%.2f,%.2f,%.2f]",e->pos[0],e->pos[1],e->pos[2]); if(e->ta>=0) printf("   team %d",e->ta); printf("\n"); } }
static Ev* find(const char*kind,const char*text){ for(int k=base;k<g_nev;k++) if(!strcmp(g_evs[k&255].kind,kind) && !strcmp(g_evs[k&255].text,text)) return &g_evs[k&255]; return 0; }
static int nnew(void){ return g_nev-base; }
int main(void){
    int off=0; for(int i=0;i<NT;i++){ g_noff[i]=off; const char*s=TAGS[i]?TAGS[i]:""; strcpy(g_names+off,s); off+=(int)strlen(s)+1; g_salt[i]=(uint16_t)(0x1000+i); }
    g_map.ntags=NT; g_map.names=g_names; g_map.noff=g_noff; g_map.salt=g_salt; g_hb_nmappaths=1; strcpy(g_hb_mappaths[0],"/maps/sidewinder.map");
    // two players: A (red, slot 0) and B (blue, slot 1); bipeds in object slots 60 / 61
    obj(60,0x5001,0,14,0xFFFFFFFFu,0,0,0); obj(61,0x5002,0,14,0xFFFFFFFFu,30,30,1); *(int32_t*)(dat[60]+0x174)=0; *(int32_t*)(dat[61]+0x174)=1;
    np=2; strcpy(pl[0].name,"Alpha"); pl[0].slot=0; pl[0].unit=H(60); pl[0].rec=(uint64_t)(uintptr_t)rec[0];
          strcpy(pl[1].name,"Bravo"); pl[1].slot=1; pl[1].unit=H(61); pl[1].rec=(uint64_t)(uintptr_t)rec[1];
    printf("== match start\n");
    for(int i=0;i<6;i++) obj(i,0x100+i,2,1,0xFFFFFFFFu,(float)i,1,2);      // 6 battle rifles
    obj(6,0x106,2,2,0xFFFFFFFFu,-40,-20,5); obj(7,0x107,2,2,0xFFFFFFFFu,40,-20,5);   // 2 sniper rifles
    obj(8,0x108,3,3,0xFFFFFFFFu,1,2,3); obj(9,0x109,3,3,0xFFFFFFFFu,4,5,6);  // 2 bubble shields (the item)
    obj(10,0x10a,3,5,0xFFFFFFFFu,0,0,0); obj(11,0x10b,3,5,0xFFFFFFFFu,0,0,0); // 2 frag grenades: left out
    obj(12,0x10c,3,6,0xFFFFFFFFu,30.5f,29.5f,1); obj(13,0x10d,3,7,0xFFFFFFFFu,-5.25f,7.75f,0.5f);   // overshield near Bravo, camo
    obj(14,0x10e,1,8,0xFFFFFFFFu,-38.48f,-25.19f,-4.66f); obj(15,0x10f,1,9,H(14),0.1f,0,0.9f);    // a Wraith and its mortar (attached)
    obj(16,0x110,1,11,0xFFFFFFFFu,-19.56f,-29.95f,1); obj(17,0x111,1,12,0xFFFFFFFFu,27.04f,-26.66f,2);
    obj(18,0x112,1,10,0xFFFFFFFFu,3,3,3);   // a deployed auto turret: left out
    obj(19,0x113,3,4,0xFFFFFFFFu,3,4,5);    // a deployed bubble shield: left out
    obj(20,0x114,3,13,0xFFFFFFFFu,9,9,9);   // the permanent grav lift: left out
    obj(21,0x115,2,1,0xFFFFFFFFu,7,7,7);    // a loadout rifle caught before its attachment ...
    snap(); CHECK(nnew()==0);   // everything pending
    parent(21,H(60));           // ... attached a snapshot later: no spawn, no pickup
    snap();
    CHECK(find("weapon","10 weapons and equipment spawned: Battle rifle x6, Sniper rifle x2, Bubbleshield x2")!=0);
    Ev*w=find("vehicle","Wraith spawned at (-38.5, -25.2)"); CHECK(w && w->haspos && w->pos[2]<-4.6f && !strcmp(w->how,"Wraith") && !strcmp(w->verb,"spawned"));
    CHECK(find("vehicle","Warthog snow spawned at (-19.6, -30.0)")!=0); CHECK(find("vehicle","Hornet lite spawned at (27.0, -26.7)")!=0);
    Ev*o=find("powerup","Overshield spawned at (30.5, 29.5)"); CHECK(o && o->haspos);
    CHECK(find("powerup","Active camo spawned at (-5.2, 7.8)")!=0);   // (%.1f of -5.25 = -5.2: round-half-even of the binary value)
    CHECK(nnew()==6);   // summary + 3 vehicles + 2 power-ups: no mortar, auto turret, deployed shield, grenades, grav lift, loadout rifle
    printf("== a sniper rifle picked up, a respawned one, a rifle in Alpha's hands\n");
    parent(6,H(61)); snap(); Ev*p=find("weapon","Bravo picked up Sniper rifle"); CHECK(p && !strcmp(p->a,"Bravo") && p->ta==1 && !strcmp(p->verb,"picked up") && !p->haspos); CHECK(nnew()==1);
    parent(6,0xFFFFFFFFu); snap(); parent(6,H(60)); snap(); CHECK(nnew()==0);   // dropped and picked up again: not its first holder
    obj(22,0x116,2,2,0xFFFFFFFFu,40,-20,5); snap(); CHECK(nnew()==0); snap(); CHECK(find("weapon","Sniper rifle spawned at (40.0, -20.0)")!=0 && nnew()==1);
    obj(23,0x117,3,15,0xFFFFFFFFu,1,1,1); snap(); snap(); CHECK(find("weapon","Shotgun ammo spawned at (1.0, 1.0)")!=0);
    printf("== power-ups: Bravo takes the overshield (gone the same snapshot), Alpha the camo (still there)\n");
    *(int32_t*)(rec[0]+0x21C)=0; snap();   // (players already seen: ticks 0)
    *(int32_t*)(rec[1]+0x218)=4321; del(12); snap();
    Ev*u=find("powerup","Bravo picked up Overshield"); CHECK(u && u->ta==1 && u->haspos && u->pos[0]>30 && !strcmp(u->verb,"picked up")); CHECK(nnew()==1);
    obj(60,0x5001,0,14,0xFFFFFFFFu,-5,8,0.5f); *(int32_t*)(dat[60]+0x174)=0;   // Alpha stands on the camo
    *(int32_t*)(rec[0]+0x21C)=4400; snap(); u=find("powerup","Alpha picked up Active camo"); CHECK(u && u->ta==0 && u->haspos && u->pos[0]<-5);
    del(13); snap(); CHECK(nnew()==0);
    *(int32_t*)(rec[0]+0x220)=4500; snap(); u=find("powerup","Alpha picked up Custom power-up"); CHECK(u && !u->haspos);   // no yellow one on the map: named by colour, no position
    printf("== a new player whose tick is already set: nothing; the Wraith destroyed and respawned; a slot reused\n");
    np=3; strcpy(pl[2].name,"Charlie"); pl[2].slot=2; pl[2].unit=0xFFFFFFFFu; pl[2].rec=(uint64_t)(uintptr_t)rec[2]; *(int32_t*)(rec[2]+0x218)=999; snap(); CHECK(nnew()==0);
    del(14); del(15); snap(); obj(24,0x118,1,8,0xFFFFFFFFu,-38.5f,-25.2f,-4.6f); obj(25,0x119,1,9,H(24),0,0,1); snap(); snap();
    CHECK(find("vehicle","Wraith spawned at (-38.5, -25.2)")!=0 && nnew()==1);
    obj(2,0x200,2,2,0xFFFFFFFFu,5,5,5); snap(); snap(); CHECK(find("weapon","Sniper rifle spawned at (5.0, 5.0)")!=0);   // slot 2 reused (new salt)
    obj(26,0x11a,2,0,0xFFFFFFFFu,1,1,1); obj(27,0x11b,1,0,0xFFFFFFFFu,2,2,2); snap(); snap();   // tags the map doesn't name
    CHECK(find("weapon","Unknown item spawned at (1.0, 1.0)")!=0 && find("vehicle","Unknown vehicle spawned at (2.0, 2.0)")!=0);
    printf("== a map change: what lies there now is new again (the held sniper rifle and loadout rifle are not)\n");
    base=g_nev; st_spawns(tab,hi,pl,np,"riverworld",now); now+=0.1; CHECK(nnew()==0);
    snap(); CHECK(nnew()==0);   // ("sidewinder" again: another change, everything pending)
    snap(); CHECK(find("weapon","12 weapons and equipment spawned: Battle rifle x5, Sniper rifle x3, Bubbleshield x2, Shotgun ammo, Unknown item")!=0);
    CHECK(find("vehicle","Wraith spawned at (-38.5, -25.2)") && find("vehicle","Warthog snow spawned at (-19.6, -30.0)") && find("vehicle","Hornet lite spawned at (27.0, -26.7)") && find("vehicle","Unknown vehicle spawned at (2.0, 2.0)"));
    CHECK(nnew()==5);
    printf("%s\n",fails?"FAILED":"all spawn checks passed"); return fails!=0; }
EOF
} > "$T/t.c"
clang -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -o "$T/t" "$T/t.c"
PELOAD_SPAWN_LOG=${PELOAD_SPAWN_LOG:-} "$T/t"
