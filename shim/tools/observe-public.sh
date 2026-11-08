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
# Infection is left out (2026-11-08 13:15, the user: "remove infection from the game type joins for now as with 86 [minutes] the
# bias is heavy"); OBS_INFECTION=1 = back in.
# OBS_MATCHES=N stops after N whole matches; OBS_MAX_PLAYERS caps the size (smaller games); OBS_STRICT=1 never falls back to a
# smaller server than OBS_MIN_PLAYERS (it waits for one). The user, 2026-11-08 13:45: "observe 10 games of relative large size
# first, then observe 4 smaller games" -> OBS_MATCHES=10 OBS_MIN_PLAYERS=12 OBS_STRICT=1, then OBS_MATCHES=4 OBS_MIN_PLAYERS=4
# OBS_MAX_PLAYERS=11 OBS_STRICT=1.
# Coverage: the pick favours the map x game type the tome has least of (+25 none, +12 under 10 minutes); OBS_DONE=N = whole matches
# already recorded in this phase by an earlier run.
# OBS_RANDOM=1 draws the server (weights exp(score / 8): coverage and size still count) instead of taking the best one
# (shim/tools/learn-loop.py: "randomly joining and completing games").
# Click the observer's window to play it yourself (controller included): the visit then lasts until you quit the game.
# Its game is muted (OBS_MUTE=0 = with sound); it records with match-recorder.py, so nhook's own recorder (ntracks.c) is off
# (OBS_NATIVE_TRACKS=1 + NHOOK_TRACKS_DIR=<other folder> = both, to compare); NATIVE_DYLIB=<copy> keeps one dylib for the whole run while shim/build is rebuilt.
# Refuses to start beside a running Project Reclaimer (the user's game included); stops at once if one appears that is not its own.
set -u
cd "$(dirname "$0")/../.."
TOTAL=${1:-3600}; MAXS=${2:-2700}; TR="$HOME/Library/Application Support/ReclaimerMac/tracks"; mkdir -p "$TR/logs"
WHOLE=${OBS_WHOLE:-1}; MINP=${OBS_MIN_PLAYERS:-6}; MAXP=${OBS_MAX_PLAYERS:-999}; CW=${OBS_CLASS_W:-15}; NMATCH=${OBS_MATCHES:-0}; STRICT=${OBS_STRICT:-0}
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting"; exit 1; fi
t0=$(date +%s); skip=""; n_slayer=0; n_ctf=0; n_infection=0; n_other=0
pick() {   # "host:port map | mode | N players | class" of the best server now ("" = none)
  timeout 60 python3 shim/tools/servers10.py --json 2>/dev/null | python3 -c '
import json, sys
skip = set(sys.argv[1].split()); counts = json.loads(sys.argv[2]); minp = int(sys.argv[3]); cw = float(sys.argv[4]); infection = sys.argv[5] == "1"; maxp = int(sys.argv[6]); strict = sys.argv[7] == "1"
rnd = len(sys.argv) > 9 and sys.argv[9] == "1"   # OBS_RANDOM=1: a weighted draw (exp(score / 8)) instead of the best score
# COVERAGE (2026-11-08 14:00, the user: "sample each map at least once in context if possible, so keep track of all the maps you have
# played, and under what game types"): the tome (tracks/tome.json, engage-model.py) says how many minutes of each map x game type
# are recorded; a server whose map x type has none gets +25 players in the pick, under 10 minutes +12. Listing names -> map: the
# display name in the listing, else its first word first 5 letters (aVALANCE 30VS30 = Avalanche); unknown = no bonus.
H3 = {"armory": "Rat\u0027s Nest", "bunkerworld": "Standoff", "chill": "Narrows", "chillout": "Cold Storage", "construct": "Construct", "cyberdyne": "The Pit",
      "deadlock": "High Ground", "descent": "Assembly", "docks": "Longshore", "fortress": "Citadel", "ghosttown": "Ghost Town", "guardian": "Guardian",
      "isolation": "Isolation", "lockout": "Blackout", "midship": "Heretic", "riverworld": "Valhalla", "salvation": "Epitaph", "sandbox": "Sandbox",
      "shrine": "Sandtrap", "sidewinder": "Avalanche", "snowbound": "Snowbound", "spacecamp": "Orbital", "warehouse": "Foundry", "zanzibar": "Last Resort"}
try: tome = json.load(open(sys.argv[8])).get("maps", {})
except Exception: tome = {}
def internal(listed):
    l = (listed or "").lower()
    for k, v in H3.items():
        if v.lower() in l: return k
    w = l.split()[0] if l.split() else ""
    for k, v in H3.items():
        f = v.lower().replace(chr(39), "").split()[0]
        if len(w) >= 5 and len(f) >= 5 and w[:5] == f[:5]: return k
    return None
def coverage_bonus(listed, c):
    k = internal(listed)
    if k is None: return 0, "?"
    m = ((tome.get(k) or {}).get("classes") or {}).get(c, {}).get("minutes", 0)
    return (25 if m <= 0 else 12 if m < 10 else 0), "%s %s %.0f min" % (k, c, m)
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
    if n >= mx or n > maxp: continue
    c = cls(((i.get("mode") or "") + " " + (i.get("name") or "")).lower())
    if c is None or (c == "infection" and not infection): continue
    cb, cov = coverage_bonus(i.get("map"), c)
    cands.append((n - cw * counts.get(c, 0) - (25 if c == "other" else 0) + cb, a, i.get("map"), i.get("mode"), n, c + (" (coverage +%d: %s)" % (cb, cov) if cb else "")))
for need in ((minp,) if strict else (minp, 4)):
    ok = [x for x in cands if x[4] >= need]
    if ok and rnd:
        import math, random; top = max(x[0] for x in ok); b = random.choices(ok, [math.exp((x[0] - top) / 8.0) for x in ok])[0]
    elif ok: b = max(ok)
    if ok: print(b[1], b[2], "|", b[3], "|", b[4], "players |", b[5]); break' "$skip" "{\"slayer\":$n_slayer,\"ctf\":$n_ctf,\"infection\":$n_infection,\"other\":$n_other}" "$MINP" "$CW" "${OBS_INFECTION:-0}" "$MAXP" "$STRICT" "$TR/tome.json" "${OBS_RANDOM:-0}"; }
DONE0=${OBS_DONE:-0}   # (whole matches a previous run of this phase already recorded)
while [ $(( $(date +%s) - t0 )) -lt "$TOTAL" ] && { [ "$NMATCH" = 0 ] || [ $((DONE0 + n_slayer + n_ctf + n_infection + n_other)) -lt "$NMATCH" ]; }; do
  line=$(pick); addr=${line%% *}
  if [ -z "$addr" ]; then echo "$(date +%H:%M) no suitable server: waiting 5 minutes"; sleep 300; continue; fi
  cls=${line##*| }; cls=${cls%% *}; echo "$(date +%H:%M) joining $line"
  L="$TR/logs/$(date +%Y%m%d-%H%M%S).log"; RL="$L.rec"
  NATIVE_REAL=1 NHOOK_STATS_OPEN=0 NHOOK_TAKEOVER=1 NHOOK_TAKEOVER_FILE="$L.takeover" NHOOK_MUTE_OUTPUT="${OBS_MUTE:-1}" NHOOK_TRACKS="${OBS_NATIVE_TRACKS:-0}" NATIVE_LOG="$L" bash shim/native.sh --join-server "$addr" --background > "$L.out" 2>&1 &
  for i in $(seq 1 30); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$L.out" 2>/dev/null); [ -n "$P" ] && break; sleep 1; done
  [ -z "${P:-}" ] && { echo "the app did not start"; sleep 30; continue; }
  left=$(( TOTAL - ($(date +%s) - t0) )); secs=$(( MAXS < left ? MAXS : left ))
  mode=$(echo "$line" | cut -d'|' -f2 | sed 's/^ *//; s/ *$//')
  ts=$(date +%s); : > "$RL"
  python3 shim/tools/match-recorder.py --port 8765 --seconds "$secs" --server "$addr" --mode "$mode" >> "$RL" 2>&1 &
  RP=$!; got=0; tk=0
  # the game stays in the server for the whole visit: a recorder that stops early is restarted, the app is not rejoined
  # YOU CAN PLAY (2026-11-08): click the observer's window -> nhook gives you the controls (the controller too, sound on) and writes
  # $L.takeover; the visit then lasts until you quit the game (no leaving after a whole match, no time limit), recorded with you as
  # a real player (match-recorder.py --you-plays: coach.py sees your matches)
  while kill -0 "$P" 2>/dev/null && { [ -f "$L.takeover" ] || [ $(( $(date +%s) - ts )) -lt "$secs" ]; }; do sleep 5
    if [ -f "$L.takeover" ]; then
      if [ "$tk" = 0 ]; then tk=1; echo "$(date +%H:%M) you took the controls: this visit lasts until you quit the game (recording you as a player)"
        kill $RP 2>/dev/null; sleep 2; python3 shim/tools/match-recorder.py --port 8765 --seconds 86400 --server "$addr" --mode "$mode" --you-plays >> "$RL" 2>&1 & RP=$!; fi
      kill -0 $RP 2>/dev/null || { python3 shim/tools/match-recorder.py --port 8765 --seconds 86400 --server "$addr" --mode "$mode" --you-plays >> "$RL" 2>&1 & RP=$!; }
      continue; fi
    kill -0 $RP 2>/dev/null || { echo "the recorder stopped: restarting it"; sleep 10; python3 shim/tools/match-recorder.py --port 8765 --seconds $(( secs - ($(date +%s) - ts) )) --server "$addr" --mode "$mode" >> "$RL" 2>&1 & RP=$!; }
    for q in $(pgrep -f "$APPRE"); do [ "$q" != "$P" ] && { echo "another Project Reclaimer started (pid $q): stopping"; kill $RP; kill "$P"; sleep 3; kill -9 "$P" 2>/dev/null; exit 1; }; done
    grep -aq 'Public join timed out\|join failed\|could not join\|server is full' "$L" 2>/dev/null && { echo "join failed: $addr"; skip="$skip $addr"; break; }
    # the server went away under us (2026-11-08 13:39: "the host left the game: back to the main menu" -- the game sat in the
    # main menu for the rest of the visit): leave, skip it for this run, pick another. ("[switch] leaving the game" is no sign: the
    # app logs it on every join, as it leaves the menu scenery)
    grep -aq 'back to the main menu' "$L" 2>/dev/null && { echo "$(date +%H:%M) the game left $addr (back to the main menu): next server"; skip="$skip $addr"; break; }
    # never got into a match (stuck in the pregame lobby after a join -- 2026-11-08 11:35: launched Guardian while the server
    # moved on to The Pit, 15 minutes "pregame lobby: waiting"): leave after OBS_NOMATCH (300) s and skip the server
    if ! grep -aq '\[rec\] recording' "$RL" && [ $(( $(date +%s) - ts )) -gt "${OBS_NOMATCH:-300}" ]; then echo "$(date +%H:%M) no match after ${OBS_NOMATCH:-300} s: leaving $addr"; skip="$skip $addr"; break; fi
    # whole matches (shim/tools/observe-whole.py): finished recordings of 1500+ samples that began at the match's start (the
    # recorder's "match time" under 90 s) or after the first one (that one began when we joined, maybe mid-match)
    got=$(python3 shim/tools/observe-whole.py "$RL")
    [ "$got" -ge "$WHOLE" ] && { echo "$(date +%H:%M) $got whole match(es) recorded here: next server"; break; }; done
  kill $RP 2>/dev/null; for i in 1 2 3 4 5 6; do kill -0 $RP 2>/dev/null || break; sleep 1; done   # (the recorder closes its file)
  cat "$RL"
  kill "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; sleep 2
  got=$(python3 shim/tools/observe-whole.py "$RL")
  case "$cls" in slayer) n_slayer=$((n_slayer + got));; ctf) n_ctf=$((n_ctf + got));; infection) n_infection=$((n_infection + got));; *) n_other=$((n_other + got));; esac
  echo "whole matches so far: slayer $n_slayer, ctf $n_ctf, infection $n_infection, other $n_other"
  grep -a '\[switch\] game Playing' "$L" >/dev/null || { echo "never got into a match on $addr: skipping it"; skip="$skip $addr"; }
  [ "$got" = 0 ] && skip="$skip $addr"   # (a whole visit without a whole match: someone else's turn)
  w=$(( 180 - ($(date +%s) - ts) )); [ $w -gt 0 ] && { echo "waiting $w s before the next join"; sleep $w; }   # (joins at least 3 minutes apart)
done
echo "$(date +%H:%M) done: $((n_slayer + n_ctf + n_infection + n_other)) whole matches (slayer $n_slayer, ctf $n_ctf, infection $n_infection, other $n_other); recordings in $TR"
