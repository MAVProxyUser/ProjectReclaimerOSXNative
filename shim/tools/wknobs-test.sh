#!/bin/bash
# Offline test of the stats page's Walker tab backend (shim/native/nwknobs.c: GET /walker.json, POST do=wknob) -- compiled on its
# own against stand-ins for nstats.c / nbot.c, run in a scratch home (NHOOK_WKNOBS_HOME), never the real live-knobs folders.
#   bash shim/tools/wknobs-test.sh        -> PASS / FAIL lines, exit 1 on a failure
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/wknobs-test.XXXXXX); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
typedef struct { char* b; size_t n, cap; } StBuf;
static void sb_put(StBuf* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); char t[4096]; int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (s->n + (size_t)k + 1 > s->cap) { s->cap = (s->n + (size_t)k + 1) * 2; s->b = realloc(s->b, s->cap); } memcpy(s->b + s->n, t, (size_t)k + 1); s->n += (size_t)k; }
static void sb_str(StBuf* s, const char* v) { sb_put(s, "\"%s\"", v); }
typedef struct { char n[48]; float v; } LiveKnob;
static int g_bot_started = 1; static char g_nick[64] = "Second";
static const char* lk_path(void) { static char p[1100]; snprintf(p, sizeof p, "%s/Library/Application Support/ReclaimerMac/battles/live-knobs/%s.json", getenv("NHOOK_WKNOBS_HOME"), g_nick); return p; }
#define nlog(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#include "native/nwknobs.c"
int main(int argc, char** argv) {
    if (!strcmp(argv[1], "json")) { StBuf b = {0}; wk_json(&b); puts(b.b ? b.b : ""); return 0; }
    return wk_control(argv[2]) ? 0 : 3; }
C
clang -w -I. -o "$T/t" "$T/t.c" || { echo "FAIL: does not compile"; exit 1; }
export NHOOK_WKNOBS_HOME="$T/home"; S="$T/home/Library/Application Support/ReclaimerMac"
PUB="$S/native/public-walker/home/Library/Application Support/ReclaimerMac/battles/live-knobs"; mkdir -p "$PUB"
echo '{"NHOOK_BOT_REACT": 0.842, "NHOOK_BOT_NOISE": 0.126}' > "$PUB/AIWalkerTest.json"
bad=0; ok() { echo "PASS: $1"; }; no() { echo "FAIL: $1"; bad=1; }
J=$("$T/t" json); echo "$J" | python3 -c 'import json,sys; d=json.load(sys.stdin); f={x["id"]:x for x in d["files"]}
assert f["pub/AIWalkerTest"]["knobs"]["NHOOK_BOT_REACT"]==0.842; assert d["self"]=="test/Second" and f["test/Second"]["exists"]==False' && ok "lists the public walker's file and this game's own (not there yet)" || no "listing: $J"
"$T/t" ctl 'do=wknob&f=pub%2FAIWalkerTest&set=NHOOK_BOT_SKILL%3A1%2CNHOOK_BOT_HOSE%3A1%2CNHOOK_BOT_REACT%3A0.5' 2>/dev/null && python3 -c 'import json,sys; d=json.load(open(sys.argv[1]))
assert d=={"NHOOK_BOT_REACT":0.5,"NHOOK_BOT_NOISE":0.126,"NHOOK_BOT_SKILL":1,"NHOOK_BOT_HOSE":1}, d' "$PUB/AIWalkerTest.json" && ok "set: merged into the file, the others kept" || no "set: $(cat "$PUB/AIWalkerTest.json")"
"$T/t" ctl 'do=wknob&f=pub/AIWalkerTest&del=NHOOK_BOT_HOSE,NHOOK_BOT_SKILL' 2>/dev/null && python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d=={"NHOOK_BOT_REACT":0.5,"NHOOK_BOT_NOISE":0.126}, d' "$PUB/AIWalkerTest.json" && ok "del: taken out" || no "del: $(cat "$PUB/AIWalkerTest.json")"
"$T/t" ctl 'do=wknob&f=test/Second&set=NHOOK_BOT_FIRE_EXTRA:6' 2>/dev/null && [ -f "$S/battles/live-knobs/Second.json" ] && ok "a walker's first change creates its file (and the folder)" || no "create"
for q in 'f=pub/../../x&set=NHOOK_BOT_SKILL:1' 'f=pub/AIWalkerTest&set=PATH:1' 'f=pub/AIWalkerTest&set=NHOOK_BOT_SKILL:nan' 'f=elsewhere/x&set=NHOOK_BOT_SKILL:1' 'f=pub/AIWalkerTest&set=NHOOK_bot:1' 'f=pub/AIWalkerTest'; do
  "$T/t" ctl "do=wknob&$q" 2>/dev/null; [ $? = 3 ] && ok "refused: $q" || no "accepted: $q"; done
ls "$PUB" | grep -q tmp && no "a temporary file was left" || ok "no temporary file left"
# the real layout: the public walker's home links ReclaimerMac back to the user's -- one folder, listed once, the public walker by its nickname
rm -rf "$S/native/public-walker/home/Library/Application Support/ReclaimerMac"; ln -s "$S" "$S/native/public-walker/home/Library/Application Support/ReclaimerMac"
mkdir -p "$S/native/public-walker/home/Library/Application Support/ProjectReclaimer"
echo '{"identity": {"nickname": "AIWalkerTest", "other": "x"}}' > "$S/native/public-walker/home/Library/Application Support/ProjectReclaimer/profile.json"
echo '{"NHOOK_BOT_REACT": 0.842}' > "$S/battles/live-knobs/AIWalkerTest.json"
J=$("$T/t" json); echo "$J" | python3 -c 'import json,sys; d=json.load(sys.stdin); ids=[x["id"] for x in d["files"]]; f={x["id"]:x for x in d["files"]}
assert sorted(ids)==["test/AIWalkerTest","test/Second"], ids; assert f["test/AIWalkerTest"]["where"]=="pub" and f["test/Second"]["where"]=="test"' && ok "a linked folder is listed once; the public walker told apart by its nickname" || no "linked folder: $J"
# the death review rides along: the last lines of battles/follow/why-<nick>.jsonl (a broken line skipped)
mkdir -p "$S/battles/follow"; { echo '{"t": 1, "cause": "slow start"}'; echo 'not json'; echo '{"t": 2, "cause": "outshot"}'; } > "$S/battles/follow/why-AIWalkerTest.json"l
J=$("$T/t" json); echo "$J" | python3 -c 'import json,sys; d=json.load(sys.stdin); f={x["id"]:x for x in d["files"]}
assert [r["cause"] for r in f["test/AIWalkerTest"]["why"]]==["slow start","outshot"], f["test/AIWalkerTest"]["why"]; assert f["test/AIWalkerTest"]["tune"]==[]' && ok "the death review's last lines ride along (a broken line skipped)" || no "why: $J"
exit $bad
