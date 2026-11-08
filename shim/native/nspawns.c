// nspawns.c -- weapon / vehicle / equipment / power-up spawns and pickups for the NATIVE app's stats page (included by
// nstats.c): the loader's stats.c st_spawns + hm_spawn + st_sentence, copied with only these changes: events go through the
// native st_event_verb (its first argument is the position: t_st_evpos), 16384 object slots (the app's table), the log switch
// is NHOOK_SPAWN_LOG, and "joined in progress" comes from the app's "[menus] joining the game in progress" line. The
// snapshot's "items" / "deployed" and the heatmap's spawn points (heatmaps/<map>.spawns, shared with the loader) come from here.
static __thread const float* t_st_evpos;   // the next event's position (spawns, pickups)
static void st_sentence(char*o,size_t on,const char*tag){ size_t j=0; if(!tag || !strcmp(tag,"?")){ o[0]=0; return; }
    for(const char*c=tag; *c && j+1<on; c++){ char ch=*c=='_'?' ':*c; o[j]=(char)(j==0?toupper((unsigned char)ch):tolower((unsigned char)ch)); j++; } o[j]=0; }
static int hm_spawn(int kind,const char*name,const float*pos){   // (g_hm_mx held) -> the point's index, -1 none
    hm_switch(g_hm_cur); if(!g_hm.map[0] || !isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return -1;
    for(uint32_t i=0;i<g_hm.nsp;i++){ HmSpawn*q=&g_hm.sp[i]; float dx=q->x-pos[0], dy=q->y-pos[1], dz=q->z-pos[2];
        if(q->kind==kind && dx*dx+dy*dy<2.25f && fabsf(dz)<3.0f && !strncmp(q->name,name,sizeof q->name-1)){ q->n++; q->match=g_hm.match; g_hm.dirty=1; return (int)i; } }
    if(g_hm.nsp>=32000) return -1;
    if(g_hm.nsp>=g_hm.spcap){ uint32_t nc=g_hm.spcap?g_hm.spcap*2:256; HmSpawn*ns=realloc(g_hm.sp,nc*sizeof *ns); if(!ns) return -1; g_hm.sp=ns; g_hm.spcap=nc; }
    HmSpawn*q=&g_hm.sp[g_hm.nsp++]; memset(q,0,sizeof *q); q->x=pos[0]; q->y=pos[1]; q->z=pos[2]; q->n=1; q->match=g_hm.match; q->kind=(uint8_t)kind;
    snprintf(q->name,sizeof q->name,"%s",name); g_hm.dirty=1; return (int)(g_hm.nsp-1); }
// Spawns and pickups (backlog 47; user, 2026-10-18: "add weapon spawns to the timeline", then "add vehicle spawns and
// powerup spawns to the timeline too (include the x/y coordinates)") -> events of kind "weapon", "vehicle" and "powerup"
// (feed, forensic archive). A spawn's text ends "at (x, y)" and the event carries "pos":[x,y,z] (datum +0x50: world
// space for an object without a parent). Datum: +0x00 definition, +0x10 parent, +0x50 position; unit +0x174 team.
// A new object is a spawn once it has been seen with NO parent in two snapshots in a row (100 ms apart: the snapshot
// reads the object table while the engine runs, so it can catch an object between its creation and its attachment --
// a respawn loadout's weapon, a vehicle's turret). Which tags count (full tag path, st_wclass; checked on the 27 stock
// multiplayer .map files, re-tools/h3map.py):
//  - weapon: a weapon (object type 2) or an equipment (3) a player can pick up. Its first holder = "picked up" (a
//    player's unit, or the vehicle they sit in). A dropped weapon keeps its object (no event). Left out: grenades (a dying
//    player drops them as new objects) and DEPLOYED equipment -- the eqip tags under objects\equipment\ whose name does
//    not end in "_equipment" (bubbleshield, tripmine, regenerator, ...: what a used *_equipment item spawns; maps place
//    and players carry the *_equipment ones), gravlift_permanent included. More than 8 weapon spawns in one snapshot
//    (match start, or joining a game in progress) are one summary line.
//  - vehicle: type 1 without a parent, every one (turrets on a vehicle have it as their parent and a parent-relative
//    position: MORTAR, ANTI INFANTRY, CHAINGUN SNOW ...; the deployed auto turret, objects\equipment\autoturret, is left out).
//  - powerup: equipment under objects\multi\powerups\ (powerup_red / _blue / _yellow) or named powerup*, every one.
//    Named Overshield / Active camo / Custom power-up by colour (red / blue / yellow; GUESSED from Halo 3's names and the
//    variant's default durations 5 / 30 / 30 s). Taken = the player record's pickup tick for that colour changed (player
//    +0x218 / +0x21C / +0x220, halo3-variant-traits.md section 4): "<player> picked up <name>", the power-up = the
//    nearest one of that colour within 5 units (still there, or gone within 1.5 s). Nothing is guessed when no tick moves.
// PELOAD_SPAWN_LOG=1 prints each spawn candidate's fate (spawned, attached, left out, power-up gone without a pickup tick).
// (sp: the heatmap spawn point it came from, -1 none; sp0: where it spawned; cur: where it is now -- its holder's place while
// carried; state: 0 at its spawn point, 1 elsewhere on the ground, 2 carried (a player, or a vehicle); holder: the carrying
// player's slot, -1 none -- the snapshot's "items", user 2026-10-25: "follow it accordingly with the icon")
static struct { uint16_t salt; uint8_t live, st, type; int8_t kind, color, state; int16_t sp, holder; uint32_t def, parent; float pos[3], sp0[3], cur[3]; } g_wt[HB_MAXOBJ];   // st: 1 spawned, 2 taken, 3 pending
static char g_wt_map[64];
static struct { char name[64]; int32_t tk[3]; } g_wpp[256];   // per player slot: the power-up pickup ticks last seen
static struct { int8_t color, taken; float pos[3]; double t; uint32_t def; } g_wpg[16]; static int g_nwpg;   // power-ups gone, not yet taken
typedef struct { uint32_t def; int8_t kind, color, dep; char nm[40]; } StWcls;   // kind -1 left out, 0 weapon, 1 vehicle, 2 powerup; dep: deployed equipment
static StWcls g_stwc[256]; static int g_nstwc; static unsigned g_stwc_gen;
static const char*const g_wkind[3]={"weapon","vehicle","powerup"};
static const char*const g_wpup[3]={"Overshield","Active camo","Custom power-up"};
// the full tag path of a definition datum (as in the map file: lower case, backslashes), searched like hb_tag_name
static int st_tag_path(uint32_t def,char*out,size_t on){
    out[0]=0; char paths[HB_NPATHS+1][1024]; int np=0; hb_current_map(paths[0],1024); if(paths[0][0]) np=1;
    { pthread_mutex_lock(&g_hb_mapmx); for(int k=0;k<g_hb_nmappaths;k++) if(strcmp(g_hb_mappaths[k],paths[0])) memcpy(paths[np++],g_hb_mappaths[k],1024); pthread_mutex_unlock(&g_hb_mapmx); }
    for(int k=0;k<np;k++){ HbMap*m=hbm_get(paths[k]); int ti=def&0xFFFF; if(!m || !m->names || ti>=m->ntags || m->salt[ti]!=(def>>16)) continue;
        snprintf(out,on,"%s",m->names+m->noff[ti]); return 1; }
    return 0; }
static const StWcls* st_wclass(uint32_t def,int type){
    if(g_stwc_gen!=g_hb_mapgen){ g_stwc_gen=g_hb_mapgen; g_nstwc=0; }   // (definition datums are per map)
    for(int i=0;i<g_nstwc;i++) if(g_stwc[i].def==def) return &g_stwc[i];
    static StWcls c; memset(&c,0,sizeof c); c.def=def; c.color=-1;
    char p[256]; int known=st_tag_path(def,p,sizeof p); for(char*q=p;*q;q++) *q=(char)tolower((unsigned char)*q);
    const char*b=strrchr(p,'\\'); b=b?b+1:p; size_t bl=strlen(b); int eq=!strncmp(p,"objects\\equipment\\",18), held=bl>10 && !strcmp(b+bl-10,"_equipment");
    if(type==1) c.kind=(eq || strstr(b,"camera"))?-1:1;   // not the deployed auto turret, nor security cameras (vehicle objects too: The Pit)
    else if(strstr(p,"grenade")) c.kind=-1;
    else if(strstr(p,"\\multi\\powerups\\") || !strncmp(b,"powerup",7)){ c.kind=2; c.color=strstr(b,"red")?0:strstr(b,"blue")?1:strstr(b,"yellow")?2:-1; }
    else if(type==3 && eq && !held){ c.kind=-1; c.dep=1; }   // (the map shows deployed equipment on its own: the snapshot's "deployed")
    else c.kind=0;
    if(c.kind==2 && c.color>=0) snprintf(c.nm,sizeof c.nm,"%s",g_wpup[c.color]);
    else if(known){ char t[64]; snprintf(t,sizeof t,"%.*s",(int)(held?bl-10:bl),b); st_sentence(c.nm,sizeof c.nm,t); }
    if(!c.nm[0]){ const char*h=hb_tag_name(def); if(h && strstr(h,"GRENADE")) c.kind=-1; st_sentence(c.nm,sizeof c.nm,h); }
    if(!c.nm[0]) snprintf(c.nm,sizeof c.nm,"%s",type==1?"Unknown vehicle":"Unknown item");
    if(known && g_nstwc<256) g_stwc[g_nstwc++]=c;   // (an unknown path is looked up again: the map file may not be read yet)
    return &c; }
static int g_wlog=-1;
static void st_wgone(int i,double now){   // tracked object i is gone (or its slot was reused)
    if(g_wt[i].kind==2 && g_wt[i].st==1){ if(g_nwpg==16){ memmove(g_wpg,g_wpg+1,15*sizeof *g_wpg); g_nwpg=15; } int k=g_nwpg++;
        g_wpg[k].color=g_wt[i].color; g_wpg[k].taken=0; memcpy(g_wpg[k].pos,g_wt[i].pos,12); g_wpg[k].t=now; g_wpg[k].def=g_wt[i].def; } }
static void st_spawns(const uint8_t*tab,int hi,const StPlayer*pl,int np,const char*map,double now){
    if(g_wlog<0) g_wlog=getenv("NHOOK_SPAWN_LOG")?1:0;
    if(strcmp(map,g_wt_map)){ memset(g_wt,0,sizeof g_wt); memset(g_wpp,0,sizeof g_wpp); g_nwpg=0; snprintf(g_wt_map,sizeof g_wt_map,"%s",map); }
    if(!g_gameplay_t0 || !hi) return;
    char lm[300]; int conf[1024], nconf=0, nwc=0;
    for(int i=0;i<hi && i<HB_MAXOBJ;i++){ const uint8_t*e=tab+i*0x18; uint16_t salt=*(const uint16_t*)e; int t=e[3];
        int here=salt && (t==1 || t==2 || t==3);
        if(g_wt[i].live && (!here || g_wt[i].salt!=salt)){ st_wgone(i,now); g_wt[i].live=0; }
        if(!here) continue;
        uint64_t p=*(const uint64_t*)(e+0x10); uint8_t d[0x5c]; if(!p || !hb_read(p,d,sizeof d)) continue;
        uint32_t def=*(const uint32_t*)d, par=*(const uint32_t*)(d+0x10);
        if(!g_wt[i].live){   // a new object: a spawn candidate when nothing holds it
            memset(&g_wt[i],0,sizeof g_wt[i]); g_wt[i].live=1; g_wt[i].salt=salt; g_wt[i].type=(uint8_t)t; g_wt[i].def=def; g_wt[i].parent=par; g_wt[i].kind=-1; g_wt[i].color=-1; g_wt[i].sp=-1; g_wt[i].holder=-1;
            if(par==0xFFFFFFFFu){ const StWcls*c=st_wclass(def,t);
                if(c->kind>=0){ g_wt[i].st=3; g_wt[i].kind=c->kind; g_wt[i].color=c->color; }
                else if(g_wlog && !strstr(c->nm,"renade")){ int k=snprintf(lm,sizeof lm,"  [spawn] left out: %s (object type %d, deployed equipment)\n",c->nm,t); (void)!write(2,lm,(size_t)k); } }
            continue; }
        if(g_wt[i].st==3){   // pending: still free a snapshot later = it spawned
            if(par!=0xFFFFFFFFu){ g_wt[i].st=0; g_wt[i].parent=par;
                if(g_wlog){ int k=snprintf(lm,sizeof lm,"  [spawn] attached right after creation, not a spawn: %s (parent %08x)\n",st_wclass(def,t)->nm,par); (void)!write(2,lm,(size_t)k); }
                continue; }
            g_wt[i].st=1; memcpy(g_wt[i].pos,d+0x50,12); memcpy(g_wt[i].sp0,d+0x50,12); memcpy(g_wt[i].cur,d+0x50,12); g_wt[i].state=1; g_wt[i].parent=par;
            // a dead player's dropped weapon: on a CLIENT of the native app it arrives as a new object (the host's own one keeps
            // its object) -- "Assault rifle spawned at" every death spot (2026-11-07, our local server, both clients). A weapon
            // that appears within 2 units of where a player died in the last 4 s is that drop: no event, no spawn point.
            static int keep_drops=-1; if(keep_drops<0) keep_drops=getenv("NHOOK_SPAWN_DROPS")!=0;   // (=1: the old "spawned" for them)
            if(g_wt[i].kind==0 && !keep_drops){ const float*wp=(const float*)(d+0x50); int drop=-1;
                for(int sl=0;sl<128 && drop<0;sl++){ const SbNow*w=&g_sbnow[sl]; if(w->died<=0 || now-w->died>4.0) continue;
                    float dx=w->pos[0]-wp[0], dy=w->pos[1]-wp[1], dz=w->pos[2]-wp[2]; if(dx*dx+dy*dy<4.0f && fabsf(dz)<3.0f) drop=sl; }
                if(drop>=0){ if(g_wlog){ int k=snprintf(lm,sizeof lm,"  [spawn] dropped by %s where they died, not a spawn: %s\n",g_sbnow[drop].name,st_wclass(def,t)->nm); (void)!write(2,lm,(size_t)k); }
                    continue; } }
            if(nconf<1024){ conf[nconf++]=i; if(g_wt[i].kind==0) nwc++; } continue; }
        if(g_wt[i].st==1 && par==0xFFFFFFFFu) memcpy(g_wt[i].pos,d+0x50,12);   // where it lies now (a power-up: where it is taken)
        if(g_wt[i].st==1 && g_wt[i].kind==0 && par!=0xFFFFFFFFu){   // a weapon picked up (the first time)
            g_wt[i].st=2; uint32_t holder=par; const char*who=0; int team=-1;
            for(int pass=0;pass<2 && !who;pass++){
                for(int k=0;k<np;k++) if(pl[k].unit==holder){ who=pl[k].name; uint64_t ud=0; uint16_t hs=(holder&0xFFFF)<(uint32_t)hi?*(const uint16_t*)(tab+(holder&0xFFFF)*0x18):0;   // (index checked before the read)
                        if((holder&0xFFFF)<(uint32_t)hi && hs==(uint16_t)(holder>>16)) ud=*(const uint64_t*)(tab+(holder&0xFFFF)*0x18+0x10); if(ud) hb_read(ud+0x174,&team,4); break; }
                if(!who && (holder&0xFFFF)<(uint32_t)hi){ uint64_t hd=*(const uint64_t*)(tab+(holder&0xFFFF)*0x18+0x10); uint32_t up=0xFFFFFFFFu; if(hd && hb_read(hd+0x10,&up,4)) holder=up; else break; } }
            const char*nm=st_wclass(def,t)->nm; if(who) st_event_verb(t_st_evpos,"weapon",who,0,nm,"picked up",team,-1,0,"%s picked up %s",who,nm); }
        if(g_wt[i].st==1 || g_wt[i].st==2){   // where it is now: on the ground, or with its outermost holder (a player's unit, a vehicle)
            if(par==0xFFFFFFFFu){ memcpy(g_wt[i].cur,d+0x50,12); g_wt[i].holder=-1; g_wt[i].state=1; }
            else { uint32_t h=par; int hold=-1; float rp[3]; int got=0;
                for(int depth=0;depth<4 && h!=0xFFFFFFFFu;depth++){
                    for(int k=0;k<np && hold<0;k++) if(pl[k].unit==h) hold=pl[k].slot;
                    int hx=(int)(h&0xFFFF); if(hx>=hi || *(const uint16_t*)(tab+hx*0x18)!=(uint16_t)(h>>16)) break;
                    uint64_t hp=*(const uint64_t*)(tab+hx*0x18+0x10); uint8_t hd[0x5c]; if(!hp || !hb_read(hp,hd,sizeof hd)) break;
                    uint32_t up=*(const uint32_t*)(hd+0x10); if(up==0xFFFFFFFFu){ memcpy(rp,hd+0x50,12); got=1; break; } h=up; }
                if(got && isfinite(rp[0]) && isfinite(rp[1])) memcpy(g_wt[i].cur,rp,12); g_wt[i].holder=(int16_t)hold; g_wt[i].state=2; }
            if(g_wt[i].state==1 && g_wt[i].sp>=0){ float dx=g_wt[i].cur[0]-g_wt[i].sp0[0], dy=g_wt[i].cur[1]-g_wt[i].sp0[1], dz=g_wt[i].cur[2]-g_wt[i].sp0[2];
                if(dx*dx+dy*dy<2.25f && fabsf(dz)<3.0f) g_wt[i].state=0; } }
        g_wt[i].parent=par; }
    // spawns confirmed in this snapshot: weapons in a burst = one line; vehicles and power-ups one by one.
    // A match's first weapons reach a CLIENT over several snapshots (the host's objects come in over ~1 s): "Excavator spawned
    // at", "Smg spawned at" ... then "17 weapons and equipment spawned" at a client's match start (2026-11-07). From a match's
    // first weapon spawn on they are gathered (sp_held) until 0.5 s pass without a new one (3 s at most) and go out together:
    // one line for more than 8 (a host's burst, all in one snapshot, goes out 0.5 s later). NHOOK_SPAWN_NOGATHER=1 = old.
    typedef struct { uint32_t def; uint8_t type; float pos[3]; } SpW; static SpW sp_held[512]; static int sp_nheld, sp_armed; static double sp_t0, sp_win, sp_last;
    static int nogather=-1; if(nogather<0) nogather=getenv("NHOOK_SPAWN_NOGATHER")!=0;
    if(g_gameplay_t0!=sp_t0){ sp_t0=g_gameplay_t0; sp_armed=!nogather; sp_win=0; sp_nheld=0; }
    if(sp_armed && nwc>0){ sp_armed=0; sp_win=now+3.0; }
    SpW ws[1536]; int nws=0;
    if(sp_win>0){ for(int q=0;q<nconf;q++){ int i=conf[q]; if(g_wt[i].kind!=0 || sp_nheld>=512) continue; SpW*h=&sp_held[sp_nheld++]; h->def=g_wt[i].def; h->type=g_wt[i].type; memcpy(h->pos,g_wt[i].pos,12); sp_last=now; }
        if(now>=sp_win || now-sp_last>=0.5){ sp_win=0; for(int k=0;k<sp_nheld;k++) ws[nws++]=sp_held[k]; sp_nheld=0; } }
    else for(int q=0;q<nconf && nws<1536;q++){ int i=conf[q]; if(g_wt[i].kind!=0) continue; ws[nws].def=g_wt[i].def; ws[nws].type=g_wt[i].type; memcpy(ws[nws].pos,g_wt[i].pos,12); nws++; }
    if(nws>8){ struct { char nm[40]; int n; } sum[24]; int ns=0;
        for(int q=0;q<nws;q++){ const char*nm=st_wclass(ws[q].def,ws[q].type)->nm; int k=0;
            for(; k<ns; k++) if(!strcmp(sum[k].nm,nm)) break; if(k==ns && ns<24){ snprintf(sum[ns].nm,sizeof sum[ns].nm,"%s",nm); sum[ns].n=0; ns++; } if(k<ns) sum[k].n++; }
        char txt[380]; int k=snprintf(txt,sizeof txt,"%d weapons and equipment spawned:",nws);
        for(int q=0;q<ns && k<(int)sizeof txt-48;q++){ k+=snprintf(txt+k,sizeof txt-(size_t)k,"%s %s",q?",":"",sum[q].nm); if(sum[q].n>1) k+=snprintf(txt+k,sizeof txt-(size_t)k," x%d",sum[q].n); }
        st_event_verb(t_st_evpos,"weapon",0,0,"","spawned",-1,-1,0,"%s",txt); }
    else for(int q=0;q<nws;q++){ const char*nm=st_wclass(ws[q].def,ws[q].type)->nm; const float*ps=ws[q].pos;
        t_st_evpos=ps; st_event_verb(t_st_evpos,"weapon",0,0,nm,"spawned",-1,-1,0,"%s spawned at (%.1f, %.1f)",nm,ps[0],ps[1]); t_st_evpos=0; }
    for(int q=0;q<nconf;q++){ int i=conf[q], kd=g_wt[i].kind; if(kd==0) continue; const char*nm=st_wclass(g_wt[i].def,g_wt[i].type)->nm; const float*ps=g_wt[i].pos;
        t_st_evpos=ps; st_event_verb(t_st_evpos,g_wkind[kd],0,0,nm,"spawned",-1,-1,0,"%s spawned at (%.1f, %.1f)",nm,ps[0],ps[1]); t_st_evpos=0; }
    // spawn points for the heatmap (hm_spawn), burst weapons included. After joining a game in progress, the first spawns seen
    // (the first 5 s of them) are whatever lies around -- dropped weapons, parked vehicles --, not spawn points: left out;
    // what respawns later is recorded. A match we see start records its first spawns (the map's real layout).
    if(nconf){ static double skip_until; if(g_sp_join){ g_sp_join=0; skip_until=now+5.0;
            if(g_wlog){ int k=snprintf(lm,sizeof lm,"  [spawn] joined in progress: the first 5 s of spawns are not spawn points\n"); (void)!write(2,lm,(size_t)k); } }
        if(now>=skip_until){ pthread_mutex_lock(&g_hm_mx);
            for(int q=0;q<nconf;q++){ int i=conf[q], kd=g_wt[i].kind; const char*nm=st_wclass(g_wt[i].def,g_wt[i].type)->nm;
                g_wt[i].sp=(int16_t)hm_spawn(kd==1?1:kd==2?3:g_wt[i].type==3?2:0,nm,g_wt[i].pos); if(g_wt[i].sp>=0) g_wt[i].state=0; }
            pthread_mutex_unlock(&g_hm_mx); } }
    // power-ups taken: a player's pickup tick for a colour moved (the first sight of a player only remembers it)
    for(int k=0;k<np;k++){ int32_t tk[3]; if(!pl[k].rec || !hb_read(pl[k].rec+0x218,tk,12)) continue; int s=pl[k].slot&255;
        if(strcmp(g_wpp[s].name,pl[k].name)){ snprintf(g_wpp[s].name,sizeof g_wpp[s].name,"%s",pl[k].name); memcpy(g_wpp[s].tk,tk,12); continue; }
        for(int c=0;c<3;c++){ if(tk[c]<=0 || tk[c]==g_wpp[s].tk[c]) continue;
            float me[3]={0,0,0}; int team=-1, mine=0; uint32_t h=pl[k].unit; if(h!=0xFFFFFFFFu && (int)(h&0xFFFF)<hi && *(const uint16_t*)(tab+(h&0xFFFF)*0x18)==(uint16_t)(h>>16)){
                uint64_t ud=*(const uint64_t*)(tab+(h&0xFFFF)*0x18+0x10); if(ud && hb_read(ud+0x1c,me,12)){ mine=1; hb_read(ud+0x174,&team,4); } }
            int bi=-1, bg=-1; float bd=25.0f;   // the nearest power-up of that colour within 5 units: one still there, or one gone in the last 1.5 s
            for(int i=0;i<hi && i<HB_MAXOBJ && mine;i++) if(g_wt[i].live && g_wt[i].kind==2 && g_wt[i].st==1 && (g_wt[i].color==c || g_wt[i].color<0)){
                float dx=g_wt[i].pos[0]-me[0], dy=g_wt[i].pos[1]-me[1], dz=g_wt[i].pos[2]-me[2], dd=dx*dx+dy*dy+dz*dz; if(dd<bd){ bd=dd; bi=i; } }
            for(int g=0;g<g_nwpg && mine;g++) if(!g_wpg[g].taken && now-g_wpg[g].t<1.5 && (g_wpg[g].color==c || g_wpg[g].color<0)){
                float dx=g_wpg[g].pos[0]-me[0], dy=g_wpg[g].pos[1]-me[1], dz=g_wpg[g].pos[2]-me[2], dd=dx*dx+dy*dy+dz*dz; if(dd<bd){ bd=dd; bg=g; bi=-1; } }
            const char*nm=g_wpup[c]; const float*ps=0;
            if(bg>=0){ g_wpg[bg].taken=1; ps=g_wpg[bg].pos; nm=st_wclass(g_wpg[bg].def,3)->nm; }
            else if(bi>=0){ g_wt[bi].st=2; ps=g_wt[bi].pos; nm=st_wclass(g_wt[bi].def,3)->nm; }
            t_st_evpos=ps; st_event_verb(t_st_evpos,"powerup",pl[k].name,0,nm,"picked up",team,-1,0,"%s picked up %s",pl[k].name,nm); t_st_evpos=0; }
        memcpy(g_wpp[s].tk,tk,12); }
    // power-ups gone without anyone's pickup tick moving (PELOAD_SPAWN_LOG: is a power-up consumed some other way?)
    for(int g=0;g<g_nwpg;){ if(now-g_wpg[g].t<1.5){ g++; continue; }
        if(g_wlog && !g_wpg[g].taken){ int k=snprintf(lm,sizeof lm,"  [spawn] power-up gone without a pickup tick: %s at (%.1f, %.1f, %.1f)\n",st_wclass(g_wpg[g].def,3)->nm,g_wpg[g].pos[0],g_wpg[g].pos[1],g_wpg[g].pos[2]); (void)!write(2,lm,(size_t)k); }
        memmove(g_wpg+g,g_wpg+g+1,(size_t)(g_nwpg-g-1)*sizeof *g_wpg); g_nwpg--; }
    if(g_wlog) for(int q=0;q<nconf;q++){ int i=conf[q]; int k=snprintf(lm,sizeof lm,"  [spawn] %s spawned: %s at (%.2f, %.2f, %.2f)\n",g_wkind[g_wt[i].kind],st_wclass(g_wt[i].def,g_wt[i].type)->nm,g_wt[i].pos[0],g_wt[i].pos[1],g_wt[i].pos[2]); (void)!write(2,lm,(size_t)k); } }
