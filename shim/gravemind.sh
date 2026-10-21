#!/bin/bash
# Project Gravemind (Halo 2 on MCC's halo2.dll) on the loader. Guide: shim/docs/gravemind.md.
#     bash shim/gravemind.sh                       # its launcher (its menus need Direct2D: see the guide's status)
#     bash shim/gravemind.sh --map-id 44           # straight into an offline match: 44 Lockout, 45 Ascension, 46 Midship ...
#                                                  # (LEEME.txt "MAPAS" lists them all)
#     bash shim/gravemind.sh --host --map-id 44    # host a game (others join your IP on UDP 11774)
#     bash shim/gravemind.sh --connect <ip>:11774  # join one
# Its other options (--name, --width/--height, --borderless, --server ...) pass straight through; LEEME.txt documents them.
# Needs: MCC's Halo 2 multiplayer files (DepotDownloader depot 976736: shim/docs/gravemind.md) and Gravemind's zip
# (h2gravemind*.zip) in client/. Click the window to capture the mouse; F1 releases it.
# GM_TEST=1: an input-less test run (no keyboard / mouse / pad / audio, never takes focus) with its own profile
# (C:\ReclaimerProfileGMTest) -- test runs never use your profile or your Gravemind name.
# Log: /tmp/gravemind_play.log; Gravemind's own log: C:\ReclaimerCache\Project Gravemind\logs\h2gravemind.log.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
GM="$PFX/Gravemind"
MCC_DIR="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
[ -x "$ROOT/shim/build/peload" ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
[ -f "$MCC_DIR/halo2/halo2.dll" ] || { echo "Halo 2 is not installed in MCC: see shim/docs/gravemind.md (DepotDownloader, depot 976736)"; exit 1; }
# install / update from the newest zip in client/ (only its own files: the exe, LEEME.txt, the font licence, the .bat examples)
ZIP="$(ls -t "$ROOT"/client/h2gravemind*.zip 2>/dev/null | head -1)"
if [ -n "$ZIP" ] && { [ ! -f "$GM/h2gravemind.exe" ] || [ "$ZIP" -nt "$GM/h2gravemind.exe" ]; }; then
  mkdir -p "$GM" && unzip -q -o "$ZIP" -d "$GM" && touch "$GM/h2gravemind.exe" && echo "installed $(basename "$ZIP") into C:\\Gravemind"
fi
[ -f "$GM/h2gravemind.exe" ] || { echo "Project Gravemind is not installed: put its zip (h2gravemind*.zip) in client/"; exit 1; }
for p in $(pgrep -f '^[^ ]*build/peload'); do
  c="$(ps -o command= -p "$p" 2>/dev/null)"; case "$c" in *" dedicated "*|*" local-server "*|*"probe-engine-worker C:\\Rec"*) continue;; esac
  echo "a game is already running (pid $p) - not starting"; exit 3
done
ENV=(PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 PELOAD_TRACE_XINPUT=1 PELOAD_TRACE_FG=1 PELOAD_TRACE_D2D=1 PELOAD_TRACE_WINMM="${PELOAD_TRACE_WINMM:-stats}" PELOAD_GL_HELPER="${PELOAD_GL_HELPER:-1}" PELOAD_STATS_HTTP="${PELOAD_STATS_HTTP:-0}")
[ -n "${GM_TEST:-}" ] && ENV+=(PELOAD_NO_FOCUS=1 PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_PAD=1 PELOAD_AUDIO=0
  PELOAD_PASTEBOARD=reclaimer-test 'USERPROFILE=C:\ReclaimerProfileGMTest') && mkdir -p "$PFX/ReclaimerProfileGMTest"
LOG="${GM_LOG:-/tmp/gravemind_play.log}"
echo "Project Gravemind$( [ -n "${GM_TEST:-}" ] && echo ' (test run)'); log: $LOG"
cd "$GM" && exec env "${ENV[@]}" arch -x86_64 "$ROOT/shim/build/peload" "$GM/h2gravemind.exe" "$@" > "$LOG" 2>&1
