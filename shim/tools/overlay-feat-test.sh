#!/bin/bash
# Offline test of the overlay panel's loader side (no game): extracts the REAL per-feature settings code from
# shim/src/hitbox.c (hbf_init / hbs_* / hb_feat_set / team classes / hb_vis_needed) and stats.c (st_feat /
# st_overlay_features, sb_put) by marker, compiles it natively with a few stubs and checks it:
#   do=feat parsing (lists, %2C, all, bad input -> refused), "applies only where the feature has it", the old global
#   switches as apply-to-all (team cycle, visible bit, text px), env defaults, the depth readback's need, friend /
#   enemy classes (occupants, shooters) and the snapshot JSON.
#   bash shim/tools/overlay-feat-test.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/ovfeat.XXXXXX"); trap 'rm -rf "$T"' EXIT
# blocks by marker: from the first line matching START up to (not including) the first later line matching END
cut_block(){ awk -v s="$2" -v e="$3" 'index($0,s)==1{on=1} on && index($0,e)==1 && !(index($0,s)==1){exit} on{print}' "$1"; }
{
cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
static volatile int g_hb_toggle, g_hb_teamcycle;   // (wininput.c)
EOF
cut_block src/hitbox.c 'enum { HBF_HITBOX' 'static int hb_overlay_on'
cut_block src/hitbox.c 'typedef struct { float c[3], r, o[3]' '// PELOAD_INPUT_FACE'
echo 'static int hbz_xray(void){ return getenv("PELOAD_HB_ZONES_XRAY")?1:0; }'
cut_block src/hitbox.c 'static int hb_vis_needed' 'static void hb_vis_tick'
cut_block src/stats.c 'typedef struct { char*b; size_t n, cap; } StBuf;' 'static void sb_str'
cut_block src/stats.c '// ---- the overlay panel (user, 2026-10-18)' 'static int st_control('
cat <<'EOF'
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static int feat(const char*q){ return st_feat(q); }
int main(int argc,char**argv){
    if(argc>1 && !strcmp(argv[1],"env")){   // PELOAD_HB_TEAMS=enemies PELOAD_HB_VISIBLE=1 PELOAD_HB_TEXT_PX=9 (+ PELOAD_HB_ZONES_XRAY=1)
        hbf_init(); CHECK(g_hbs[HBF_HITBOX].team==1 && g_hbs[HBF_INFO].team==1 && g_hbs[HBF_ITEMS].team==-1);
        CHECK(g_hbs[HBF_HITBOX].vis==1 && g_hbs[HBF_ITEMS].vis==1 && g_hbs[HBF_ZONES].vis==-1);
        CHECK(g_hbs[HBF_INFO].px==9 && g_hbs[HBF_ITEMS].px==9 && g_hbs[HBF_HITBOX].px==-1 && g_hbs[HBF_ZONES].px==9);   // zones: labels in xray mode
        printf("%s\n",fails?"env: FAILED":"env: ok"); return fails!=0; }
    hbf_init();
    // defaults: team all, through walls, 7 px; N/A = -1
    CHECK(g_hbs[HBF_HITBOX].team==0 && g_hbs[HBF_HITBOX].vis==0 && g_hbs[HBF_HITBOX].px==-1);
    CHECK(g_hbs[HBF_INFO].team==0 && g_hbs[HBF_INFO].px==7 && g_hbs[HBF_ITEMS].team==-1 && g_hbs[HBF_ITEMS].px==7);
    CHECK(g_hbs[HBF_ZONES].team==-1 && g_hbs[HBF_ZONES].vis==-1 && g_hbs[HBF_ZONES].px==-1);
    CHECK(!hb_vis_needed());
    // per feature
    CHECK(feat("do=feat&f=hitboxes,info&team=enemies&vis=1"));
    CHECK(g_hbs[HBF_HITBOX].team==1 && g_hbs[HBF_INFO].team==1 && g_hbs[HBF_HITBOX].vis==1 && g_hbs[HBF_VEH].team==0 && g_hbs[HBF_VEH].vis==0);
    CHECK(!hb_vis_needed());                       // vis on, but the features are off: no readback
    CHECK(feat("do=feat&f=hitboxes&on=1")); CHECK(hb_feat(HBF_HITBOX)); CHECK(hb_vis_needed());
    CHECK(feat("do=feat&f=hitboxes&vis=0")); CHECK(!hb_vis_needed());
    CHECK(feat("do=feat&f=zones&on=1")); CHECK(hb_vis_needed());   // zones: always per pixel
    CHECK(feat("do=feat&f=zones&on=0")); CHECK(!hb_vis_needed());
    CHECK(feat("do=feat&f=items&team=friends&px=20")); CHECK(g_hbs[HBF_ITEMS].team==-1 && g_hbs[HBF_ITEMS].px==20);   // team n/a, px applies
    CHECK(feat("do=feat&f=info%2Citems&px=11")); CHECK(g_hbs[HBF_INFO].px==11 && g_hbs[HBF_ITEMS].px==11);
    CHECK(feat("do=feat&f=tracers&f=vehicles&team=friends")); CHECK(g_hbs[HBF_TRACER].team==2 && g_hbs[HBF_VEH].team==2);
    CHECK(feat("do=feat&f=all&on=1")); for(int f=0;f<HBF_N;f++) CHECK(f==HBF_VISIBLE ? !hb_feat(f) : hb_feat(f));
    CHECK(feat("do=feat&f=all&on=0")); for(int f=0;f<HBF_N;f++) CHECK(!hb_feat(f));
    CHECK(feat("do=feat&f=all&on=0"));   // idempotent: stays off
    for(int f=0;f<HBF_N;f++) CHECK(!hb_feat(f));
    // refused: unknown feature, bad values, nothing to set, no feature
    CHECK(!feat("do=feat&f=bogus&on=1")); CHECK(!feat("do=feat&f=visible&on=1")); CHECK(!feat("do=feat&f=info&team=foes"));
    CHECK(!feat("do=feat&f=info&px=3")); CHECK(!feat("do=feat&f=info&px=61")); CHECK(!feat("do=feat&f=info&px=12x")); CHECK(!feat("do=feat&f=info&vis=2"));
    CHECK(!feat("do=feat&f=info")); CHECK(!feat("do=feat&on=1"));
    CHECK(g_hbs[HBF_INFO].px==11 && g_hbs[HBF_INFO].team==1);   // (unchanged by the refused ones)
    // the old global switches = apply to all; a later per-feature setting sticks
    g_hb_teamcycle++; hbs_sync(); for(int f=0;f<HBF_N;f++) CHECK(g_hbs[f].team==-1 || g_hbs[f].team==1);
    CHECK(feat("do=feat&f=info&team=friends")); hbs_sync(); CHECK(g_hbs[HBF_INFO].team==2 && g_hbs[HBF_HITBOX].team==1);
    g_hb_toggle^=1<<HBF_VISIBLE; hbs_sync(); for(int f=0;f<HBF_N;f++) CHECK(g_hbs[f].vis==-1 || g_hbs[f].vis==1);
    g_hb_text_px=15; hbs_sync(); CHECK(g_hbs[HBF_INFO].px==15 && g_hbs[HBF_ITEMS].px==15 && g_hbs[HBF_HITBOX].px==-1);
    // friend / enemy classes: we are team 0; B (enemy) and C (friend) ride vehicle V; W is empty
    g_hb_myteam=0; HbObj o[5]; memset(o,0,sizeof o);
    o[0].type=0; o[0].team=0; o[0].handle=0x10001; o[0].parent=0xFFFFFFFFu;   // A: a teammate on foot
    o[1].type=0; o[1].team=1; o[1].handle=0x10002; o[1].parent=0x20001;       // B: an enemy in V
    o[2].type=0; o[2].team=0; o[2].handle=0x10003; o[2].parent=0x20001;       // C: a teammate in V
    o[3].type=1; o[3].handle=0x20001; o[3].parent=0xFFFFFFFFu;                // V
    o[4].type=1; o[4].handle=0x20002; o[4].parent=0xFFFFFFFFu;                // W
    CHECK(hb_bip_cls(&o[0])==2 && hb_bip_cls(&o[1])==1);
    CHECK(hb_occ_cls(0x20001,o,5)==3 && hb_occ_cls(0x20002,o,5)==0);
    CHECK(hb_owner_cls(0x10002,o,5)==1 && hb_owner_cls(0x10001,o,5)==2 && hb_owner_cls(0x20001,o,5)==3 && hb_owner_cls(0x30009,o,5)==0 && hb_owner_cls(0xFFFFFFFFu,o,5)==0);
    CHECK(hb_team_ok(0,0) && !hb_team_ok(1,0) && !hb_team_ok(2,0) && hb_team_ok(1,1) && !hb_team_ok(2,1) && hb_team_ok(1,3) && hb_team_ok(2,3));
    // the snapshot's features object
    StBuf s={0}; sb_put(&s,"{\"x\":0"); st_overlay_features(&s); sb_put(&s,"}"); printf("JSON:%s\n",s.b);
    printf("%s\n",fails?"FAILED":"ok"); return fails!=0; }
EOF
} > "$T/t.c"
clang -O1 -Wall -Wno-unused-function -Wno-unused-variable -o "$T/t" "$T/t.c"
"$T/t" > "$T/out" 2> "$T/err" || { cat "$T/out" "$T/err"; exit 1; }
grep -v '^JSON:' "$T/out"
grep '^JSON:' "$T/out" | sed 's/^JSON://' | python3 -c 'import json,sys; d=json.load(sys.stdin)["features"]; assert list(d)==["hitboxes","vehicles","tracers","info","items","zones"], d; assert d["items"]["team"] is None and d["zones"]["vis"] is None and d["info"]["px"]==15 and d["info"]["team"]=="friends"; print("json: ok", json.dumps(d))'
PELOAD_HB_TEAMS=enemies PELOAD_HB_VISIBLE=1 PELOAD_HB_TEXT_PX=9 PELOAD_HB_ZONES_XRAY=1 "$T/t" env 2> "$T/err" || { cat "$T/err"; exit 1; }
