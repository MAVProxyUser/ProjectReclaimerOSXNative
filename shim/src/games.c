// games.c -- which game the loader is running (2026-10-21). Three fan clients run on the same loader: Project Reclaimer
// (Halo 3: halo3.dll), Reach Renaissance (Halo: Reach: haloreach.dll) and Project Gravemind (Halo 2: halo2.dll). Fixes that make a
// Windows function behave as on Windows are shared by all of them; anything that only one game needs -- patches of its exe or
// engine, hooks, special cases, different defaults -- asks game_is() first, so one game's work can't change another's.
// shim/docs/games.md lists what is per game and what is shared; shim/tools/game-matrix.sh runs all three.
//   Detection: the exe's file name at start (project-reclaimer* / *reclaimer* -> h3; h2gravemind* -> h2; host-probe,
//   "Reach Renaissance", menu-overlay, rr-* -> reach), else the first engine DLL loaded (halo3 / haloreach / halo2).
//   PELOAD_GAME=h3|reach|h2|other overrides. The choice is printed as "[game] ...".
enum { GAME_OTHER=0, GAME_H3=1, GAME_REACH=2, GAME_H2=3 };
static int g_game=-1;
static const char* game_name(int g){ return g==GAME_H3?"h3 (Project Reclaimer, halo3.dll)":g==GAME_REACH?"reach (Reach Renaissance, haloreach.dll)":
    g==GAME_H2?"h2 (Project Gravemind, halo2.dll)":"other"; }
static int game_from_env(void){ const char*e=getenv("PELOAD_GAME"); if(!e||!*e) return -1;
    if(!strcasecmp(e,"h3")||!strcasecmp(e,"reclaimer")) return GAME_H3; if(!strcasecmp(e,"reach")) return GAME_REACH;
    if(!strcasecmp(e,"h2")||!strcasecmp(e,"gravemind")) return GAME_H2; return GAME_OTHER; }
static void game_detect_exe(const char*path){
    int g=game_from_env();
    if(g<0){ const char*b=path?path:""; for(const char*q=b;*q;q++) if(*q=='/'||*q=='\\') b=q+1;
        if(strcasestr(b,"reclaimer")) g=GAME_H3;
        else if(strcasestr(b,"h2gravemind")||strcasestr(b,"gravemind")) g=GAME_H2;
        else if(strcasestr(b,"host-probe")||strcasestr(b,"reach renaissance")||strcasestr(b,"menu-overlay")||!strncasecmp(b,"rr-",3)) g=GAME_REACH;
        else g=GAME_OTHER; }
    g_game=g; char m[160]; int k=snprintf(m,sizeof m,"  [game] %s%s\n",game_name(g),game_from_env()>=0?" (PELOAD_GAME)":""); (void)!write(2,m,k); }
// an engine DLL settles an exe the name didn't place
static void game_note_dll(const char*name){ if(g_game!=GAME_OTHER || !name) return; int g=-1;
    if(strcasestr(name,"halo3.dll")) g=GAME_H3; else if(strcasestr(name,"haloreach.dll")) g=GAME_REACH; else if(strcasestr(name,"halo2.dll")) g=GAME_H2;
    if(g>=0){ g_game=g; char m[160]; int k=snprintf(m,sizeof m,"  [game] %s (from the engine DLL)\n",game_name(g)); (void)!write(2,m,k); } }
static int game_is(int g){ return g_game==g; }
// per-game defaults that are switches elsewhere: PELOAD_<name> set = that value; unset = the game's default
static int game_default_on(const char*env,int on_for_game){ const char*e=getenv(env); if(e&&*e) return strcmp(e,"0")!=0; return game_is(on_for_game); }
