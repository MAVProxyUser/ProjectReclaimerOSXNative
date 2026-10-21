#!/bin/bash
# One short, input-less run of every installed game with THIS checkout's loader, and a pass / fail line for each -- run it after a
# loader change so a fix for one game can't silently break another (shim/docs/games.md).
#     bash shim/tools/game-matrix.sh            # every installed game
#     bash shim/tools/game-matrix.sh h2 reach   # some of them
#   h3     Project Reclaimer: our local dedicated server + one quiet-run client joining it -> the match starts, the chat hook
#          answers a typed line, no [CRASH] (client log /tmp/quiet-run.log)
#   reach  Reach Renaissance: a local Sword Base match (shim/reach.sh match) -> frames presented, no [CRASH] (/tmp/reach_play.log)
#   h2     Project Gravemind: an offline Lockout match (shim/gravemind.sh --map-id 44, GM_TEST=1) -> its "game state 1", a window
#          frame that isn't black, no [CRASH] (/tmp/gravemind_play.log)
# Test runs only: no input, no audio, never focused, test profiles. Never while another game runs (one client at a time).
cd "$(dirname "$0")/../.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
MCC="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
GAMES=("$@"); [ ${#GAMES[@]} -eq 0 ] && GAMES=(h3 reach h2)
running(){ pgrep -f '^[^ ]*build/peload' >/dev/null; }
if running; then echo "a game is running - not starting (one client at a time)"; exit 3; fi
stop_pgid(){ kill -TERM -- -"$1" 2>/dev/null; sleep 2; kill -9 -- -"$1" 2>/dev/null; }
RESULTS=()
result(){ RESULTS+=("$1"); echo "$1"; }
frame_lit(){ python3 - "$1" <<'EOF'
import sys,struct
d=open(sys.argv[1],'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); px=d[off:]; n=w*abs(h); lit=0; tot=0
for i in range(0,n*4,4*97): tot+=1; lit+=(px[i]+px[i+1]+px[i+2])>30
print(1 if tot and lit*10>tot else 0)
EOF
}
for g in "${GAMES[@]}"; do case "$g" in
h3)
  EXE=$(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1)
  [ -n "$EXE" ] && [ -f "$MCC/halo3/halo3.dll" ] || { result "h3     SKIP (no client exe in client/ or no Halo 3 in MCC)"; continue; }
  bash shim/dedicated.sh >/dev/null 2>&1; sleep 5
  QR_STATS_OPEN=0 bash shim/tools/quiet-run.sh "PELOAD_STATS_UISTEPS=1 PELOAD_FEED_LOG=1" 180 "reclaimer://join?server=127.0.0.1:49190" >/dev/null 2>&1 &
  sleep 6; ok=0
  for i in $(seq 1 60); do grep -a -q -E 'Loading screen complete|Joining server failed|\[CRASH\]' /tmp/quiet-run.log 2>/dev/null && break; sleep 3; done
  if grep -a -q 'Loading screen complete' /tmp/quiet-run.log; then
    sleep 8; curl -s -X POST -H 'X-Reclaimer-Control: 1' 'http://127.0.0.1:8765/control?do=uisteps&s=k:11;s:1;t:matrix%20check;s:0.5;k:24' >/dev/null
    for i in $(seq 1 20); do grep -a -q 'chat: matrix check' /tmp/quiet-run.log && break; sleep 1; done
    grep -a -q 'chat: matrix check' /tmp/quiet-run.log && ok=1; fi
  crash=$(grep -a -c '\[CRASH\]' /tmp/quiet-run.log)
  for p in $(pgrep -f '^[^ ]*build/peload'); do pg=$(ps -o pgid= -p "$p" | tr -d ' '); [ "$pg" = "$(cat "$HOME/Library/Application Support/ReclaimerMac/dedicated.pgid" 2>/dev/null)" ] || stop_pgid "$pg"; done
  bash shim/dedicated.sh stop >/dev/null 2>&1; sleep 2
  [ $ok = 1 ] && [ "$crash" = 0 ] && result "h3     PASS (joined our local server, match started, chat hook answered)" || result "h3     FAIL (match/chat ok=$ok, crashes=$crash; /tmp/quiet-run.log)";;
reach)
  [ -f "$PFX/ReachRenaissance/bin/host-probe.exe" ] && [ -f "$MCC/haloreach/haloreach.dll" ] || { result "reach  SKIP (Reach Renaissance or MCC's Halo: Reach not installed)"; continue; }
  set -m; PELOAD_NO_FOCUS=1 PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_PAD=1 PELOAD_AUDIO=0 PELOAD_PASTEBOARD=reclaimer-test \
    bash shim/reach.sh match 20_sword_slayer slayer_054 5 >/dev/null 2>&1 & RP=$!; set +m
  sleep 75; pg=$(ps -o pgid= -p $RP 2>/dev/null | tr -d ' '); [ -n "$pg" ] && stop_pgid "$pg"
  fr=$(grep -a -c -E '^t=[0-9]+s frames \+[1-9]' /tmp/reach_play.log); crash=$(grep -a -c '\[CRASH\]' /tmp/reach_play.log)
  [ "$fr" -gt 3 ] && [ "$crash" = 0 ] && result "reach  PASS (Sword Base match: $fr frame reports, no crash)" || result "reach  FAIL (frame reports=$fr, crashes=$crash; /tmp/reach_play.log)";;
h2)
  [ -f "$MCC/halo2/halo2.dll" ] && ls client/h2gravemind*.zip >/dev/null 2>&1 || [ -f "$PFX/Gravemind/h2gravemind.exe" ] || { result "h2     SKIP (no Gravemind zip in client/ or no Halo 2 in MCC)"; continue; }
  rm -f "$HOME"/reclaimer_win_*.bmp; set -m
  GM_TEST=1 PELOAD_D3DGL_SHOTWIN=1200 PELOAD_PRESENT_HIDDEN=1 bash shim/gravemind.sh --map-id 44 >/dev/null 2>&1 & GP=$!; set +m
  sleep 70; pg=$(ps -o pgid= -p $GP 2>/dev/null | tr -d ' '); [ -n "$pg" ] && stop_pgid "$pg"
  st=$(grep -a -c 'game state 1' "$PFX/ReclaimerCache/Project Gravemind/logs/h2gravemind.log" 2>/dev/null); crash=$(grep -a -c '\[CRASH\]' /tmp/gravemind_play.log)
  f=$(ls -t "$HOME"/reclaimer_win_*.bmp 2>/dev/null | head -1); lit=0; [ -n "$f" ] && lit=$(frame_lit "$f"); rm -f "$HOME"/reclaimer_win_*.bmp
  [ "${st:-0}" -gt 0 ] && [ "$lit" = 1 ] && [ "$crash" = 0 ] && result "h2     PASS (offline Lockout: in game, frame drawn, no crash)" || result "h2     FAIL (in game=${st:-0}, frame lit=$lit, crashes=$crash; /tmp/gravemind_play.log)";;
*) result "$g     ? (unknown: h3 | reach | h2)";;
esac; done
echo "----"; printf '%s\n' "${RESULTS[@]}"
