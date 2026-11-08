#!/bin/bash
# Watch real players (2026-11-08; the user asked: "join several games that don't require map downloads, and watch and sample how
# real players move around the map, and skirmish"). Picks a public 0.10 server that needs NO download (no mods), has no password,
# is open and busy, joins it with the user's own profile in a background window (the 0.10 app lets only a real profile into
# public servers; nothing is pressed: the player stands where it spawns -- nhook's sparring aim / walker refuse public servers by
# themselves), records it with shim/tools/match-recorder.py, quits the app and picks again, until TOTAL seconds. Recordings:
# ~/Library/Application Support/ReclaimerMac/tracks (no names).
# Midday 2026-11-08 (the user: "sample a bunch of large games (that don't require downloads) make sure it is a mix of slayer, ctf,
# and infection game types. and sample the whole game"): every server is put in a class -- slayer, ctf, infection, other -- and
# each whole match recorded in a class this run lowers that class by OBS_CLASS_W players in the pick, so the classes take turns
# while the bigger game still wins inside a class ("other" -- assault, king of the hill ... -- counts 25 players less; campaign,
# forge, firefight, death race and UNSC vs Flood vs Covenant are never picked). The client stays on a server until it has
# recorded OBS_WHOLE whole matches there (a recording that started after the one it joined into, at least 1500 samples = 2.5
# min), or MAXSESSION seconds, then picks again; at least OBS_MIN_PLAYERS (6) players, 4 when no server has that many.
#   bash shim/tools/observe-public.sh [TOTAL seconds, default 3600] [MAXSESSION seconds, default 2700]
# Its game is muted (OBS_MUTE=0 = with sound); it records with match-recorder.py, so nhook's own recorder (ntracks.c) is off
# (OBS_NATIVE_TRACKS=1 + NHOOK_TRACKS_DIR=<other folder> = both, to compare); NATIVE_DYLIB=<copy> keeps one dylib for the whole run while shim/build is rebuilt.
# Refuses to start beside a running Project Reclaimer (the user's game included); stops at once if one appears that is not its own.
set -u
cd "$(dirname "$0")/../.."
TOTAL=${1:-3600}; MAXS=${2:-2700}; TR="$HOME/Library/Application Support/ReclaimerMac/tracks"; mkdir -p "$TR/logs"
WHOLE=${OBS_WHOLE:-1}; MINP=${OBS_MIN_PLAYERS:-6}; CW=${OBS_CLASS_W:-15}
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
t0=$(date +%s); skip=""; n_slayer=0; n_ctf=0; n_infection=0; n_other=0
pick() {   # "host:port map | mode | N players | class" of the best server now ("" = none)
  timeout 60 python3 shim/tools/servers10.py --json 2>/dev/null | python3 -c '
import json, sys
skip = set(sys.argv[1].split()); counts = json.loads(sys.argv[2]); minp = int(sys.argv[3]); cw = float(sys.argv[4])
def cls(t):
    if any(k in t for k in ("campaign", "forge", "firefight", "death race", "race")): return None
    if "flood" in t and "cov" in t: return None                        # (memory: skip unsc vs flood vs cov)
    if any(k in t for k in ("infection", "zombie", "flood")): return "infection"
    if any(k in t for k in ("ctf", "capture the flag", "flag")): return "ctf"
    if any(k in t for k in ("slayer", "swat", "snipe", "br starts", "fiesta", "ffa", "free for all", "rumble", "doubles", "beams", "kills", "mlg", "team br")): return "slayer"
    return "other"
cands = []
for l in sys.stdin:
    try: s = json.loads(l)
    except Exception: continue
    i = s.get("info") or {}; a = s.get("address") or ""
    if not a or a in skip or i.get("password") or i.get("mods") or i.get("joins_closed") or i.get("version") != "0.10.1": continue
    n, mx = i.get("players") or 0, i.get("max_players") or 0
    if n >= mx: continue
    c = cls(((i.get("mode") or "") + " " + (i.get("name") or "")).lower())
    if c is None: continue
    cands.append((n - cw * counts.get(c, 0) - (25 if c == "other" else 0), a, i.get("map"), i.get("mode"), n, c))
for need in (minp, 4):
    ok = [x for x in cands if x[4] >= need]
    if ok: b = max(ok); print(b[1], b[2], "|", b[3], "|", b[4], "players |", b[5]); break' "$skip" "{\"slayer\":$n_slayer,\"ctf\":$n_ctf,\"infection\":$n_infection,\"other\":$n_other}" "$MINP" "$CW"; }
while [ $(( $(date +%s) - t0 )) -lt "$TOTAL" ]; do
  line=$(pick); addr=${line%% *}
  if [ -z "$addr" ]; then echo "$(date +%H:%M) no suitable server: waiting 5 minutes"; sleep 300; continue; fi
  cls=${line##*| }; echo "$(date +%H:%M) joining $line"
  L="$TR/logs/$(date +%Y%m%d-%H%M%S).log"; RL="$L.rec"
  NATIVE_REAL=1 NHOOK_STATS_OPEN=0 NHOOK_MUTE_OUTPUT="${OBS_MUTE:-1}" NHOOK_TRACKS="${OBS_NATIVE_TRACKS:-0}" NATIVE_LOG="$L" bash shim/native.sh --join-server "$addr" --background > "$L.out" 2>&1 &
  for i in $(seq 1 30); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$L.out" 2>/dev/null); [ -n "$P" ] && break; sleep 1; done
  [ -z "${P:-}" ] && { echo "the app did not start"; sleep 30; continue; }
  left=$(( TOTAL - ($(date +%s) - t0) )); secs=$(( MAXS < left ? MAXS : left ))
  mode=$(echo "$line" | cut -d'|' -f2 | sed 's/^ *//; s/ *$//')
  ts=$(date +%s); : > "$RL"
  python3 shim/tools/match-recorder.py --port 8765 --seconds "$secs" --server "$addr" --mode "$mode" >> "$RL" 2>&1 &
  RP=$!; got=0
  # the game stays in the server for the whole visit: a recorder that stops early is restarted, the app is not rejoined
  while kill -0 "$P" 2>/dev/null && [ $(( $(date +%s) - ts )) -lt "$secs" ]; do sleep 5
    kill -0 $RP 2>/dev/null || { echo "the recorder stopped: restarting it"; sleep 10; python3 shim/tools/match-recorder.py --port 8765 --seconds $(( secs - ($(date +%s) - ts) )) --server "$addr" --mode "$mode" >> "$RL" 2>&1 & RP=$!; }
    for q in $(pgrep -f "$APPRE"); do [ "$q" != "$P" ] && { echo "another Project Reclaimer started (pid $q): stopping"; kill $RP; kill "$P"; sleep 3; kill -9 "$P" 2>/dev/null; exit 1; }; done
    grep -aq 'Public join timed out\|join failed\|could not join\|server is full' "$L" 2>/dev/null && { echo "join failed: $addr"; skip="$skip $addr"; break; }
    # never got into a match (stuck in the pregame lobby after a join -- 2026-11-08 11:35: launched Guardian while the server
    # moved on to The Pit, 15 minutes "pregame lobby: waiting"): leave after OBS_NOMATCH (300) s and skip the server
    if ! grep -aq '\[rec\] recording' "$RL" && [ $(( $(date +%s) - ts )) -gt "${OBS_NOMATCH:-300}" ]; then echo "$(date +%H:%M) no match after ${OBS_NOMATCH:-300} s: leaving $addr"; skip="$skip $addr"; break; fi
    # whole matches: finished recordings after the first (that one began when we joined, mid-match), 1500+ samples
    got=$(grep -a ': [0-9]* samples' "$RL" | tail -n +2 | awk '{ if ($3 + 0 >= 1500) n++ } END { print n + 0 }')
    [ "$got" -ge "$WHOLE" ] && { echo "$(date +%H:%M) $got whole match(es) recorded here: next server"; break; }; done
  kill $RP 2>/dev/null; for i in 1 2 3 4 5 6; do kill -0 $RP 2>/dev/null || break; sleep 1; done   # (the recorder closes its file)
  cat "$RL"
  kill "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; sleep 2
  got=$(grep -a ': [0-9]* samples' "$RL" | tail -n +2 | awk '{ if ($3 + 0 >= 1500) n++ } END { print n + 0 }')
  case "$cls" in slayer) n_slayer=$((n_slayer + got));; ctf) n_ctf=$((n_ctf + got));; infection) n_infection=$((n_infection + got));; *) n_other=$((n_other + got));; esac
  echo "whole matches so far: slayer $n_slayer, ctf $n_ctf, infection $n_infection, other $n_other"
  grep -a '\[switch\] game Playing' "$L" >/dev/null || { echo "never got into a match on $addr: skipping it"; skip="$skip $addr"; }
  [ "$got" = 0 ] && skip="$skip $addr"   # (a whole visit without a whole match: someone else's turn)
  w=$(( 180 - ($(date +%s) - ts) )); [ $w -gt 0 ] && { echo "waiting $w s before the next join"; sleep $w; }   # (joins at least 3 minutes apart)
done
echo "$(date +%H:%M) done; recordings in $TR"
