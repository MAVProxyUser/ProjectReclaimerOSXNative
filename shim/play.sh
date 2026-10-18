#!/bin/bash
# Play Project Reclaimer on the D3D11->GL renderer (no Wine). Starts in Reclaimer's own menu: Server Browser,
# Host Game (Start Game hosts a local match), Customize, Settings.
# Controller: any GameController-supported pad (Xbox, DualSense, ...) via the shim's XInput.
# Keyboard + mouse: click the game window to capture them (the client's own behavior); F1 or switching
#   windows releases. Cmd-key shortcuts (Cmd+Tab, Cmd+Q) still go to macOS.
#   PELOAD_INVERT_PITCH=0      normal (non-inverted) look; inverted is the default
#   PELOAD_INPUT_SCRIPT=walk   scripted turn/walk test (PELOAD_INPUT_LOOP=1 repeats it)
#   PELOAD_AUDIO=0             silence (game audio plays through CoreAudio by default)
#   PELOAD_VSYNC=1             vsync the window (off by default: frames are never held back)
#   RECLAIMER_EXE=<path>       the Project Reclaimer client exe (default: the newest supported one in client/)
#   RECLAIMER_JOIN=host:port   start in the menu and join that server (the client's reclaimer://join link)
#   PELOAD_H3="name=value,..." set Halo 3 engine settings (shim/docs/halo3-engine-globals.md)
#   PELOAD_STATS_HTTP=<port>   live stats page port (default 8765: http://127.0.0.1:8765/); 0 turns it off
#   PELOAD_STATS_OPEN=0        don't open the stats page in the browser at launch (an open tab reconnects by itself)
#   PELOAD_D3DGL_SUBMIT=auto   GL submission thread: auto (default) = on in games with 40+ players, 1 = always, 0 = never
#   RECLAIMER_DIRECT=1         skip the menu and go straight into a local Guardian match (~12 s). Reclaimer's
#                              in-game UI (scoreboard names/scores, chat, game menu) only runs under its menu,
#                              so in this mode the scoreboard shows bars without text.
# The window title shows elapsed time, the load time, the engine frame rate, draw calls and frame time.
# Extra env can be passed in front:  PELOAD_INVERT_PITCH=0 bash shim/play.sh
# Engine/worker output goes to /tmp/reclaimer_play.log. Quit by closing the window or Ctrl-C.
cd "$(dirname "$0")/.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
MCC="C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection"
MCC_DIR="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
# The loader finds its client patches by byte signature (PELOAD_RECLAIMER_PATCHES), so it works across
# client builds; these are the builds it has been tested with (SHA-256 of the release exe).
KNOWN="0850d10888d984aa270e0fd5bfa833505010e9e646d789ca657a49b6e45a8a0d 0.9.20
a8d3a37ad27e31abc09d009a94210470976ecefb34908a9fb8e1a245f4924910 0.9.18
a393504dc32a8fd35dabb5b26ea1427fbaedabdac76b00133d49ebc44fb8efd1 0.9.13"

EXE="${RECLAIMER_EXE:-}"
if [ -z "$EXE" ]; then
  # the newest client release in client/ (servers require the current one), then the older test builds
  for c in $(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1) client/project-reclaimer-v0.9.13.exe analysis/project-reclaimer-v0.9.13.exe; do
    [ -f "$c" ] && EXE="$c" && break
  done
fi
if [ -z "$EXE" ] || [ ! -f "$EXE" ]; then
  echo "Project Reclaimer client not found. Download the newest project-reclaimer-client-v<version>.exe from"
  echo "https://github.com/ProjectReclaimer/project-reclaimer-releases/releases and put it in: $(pwd)/client/"
  exit 1
fi
VER="$(echo "$KNOWN" | grep "^$(shasum -a 256 "$EXE" | cut -d' ' -f1) " | cut -d' ' -f2)"
if [ -n "$VER" ]; then echo "client: $EXE (v$VER)"
else echo "warning: $EXE is not a client build this loader was tested with; patches are found by signature and the log lists any that were not ([sigpatch])."; fi
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh' (or use a release bundle)"; exit 1; }
[ -d "$MCC_DIR/halo3" ] || { echo "MCC files not found in: $MCC_DIR"; echo "run: ./reclaimer-mac.sh download"; exit 1; }

# Clear leftovers from an earlier launch, but leave a running test target (shim/target.sh) and dedicated server
# (shim/dedicated.sh) alone.
KEEP=" $(cat "$HOME/Library/Application Support/ReclaimerMac/target.pgid" "$HOME/Library/Application Support/ReclaimerMac/dedicated.pgid" 2>/dev/null | tr '\n' ' ') "
for p in $(pgrep -f '^[^ ]*build/peload'); do
  case "$KEEP" in *" $(ps -o pgid= -p "$p" | tr -d ' ') "*) continue;; esac
  kill -9 "$p" 2>/dev/null
done; sleep 1
# The stats page's port (PELOAD_STATS_HTTP, 8765) must be free before the game starts -- the game never serves on another
# port. A leftover map-picture / walking-graph generator from an earlier game (python re-tools/h3bsp.py / h3nav.py) is
# killed; anything else holding it is waited for (shown once, Ctrl-C to give up).
SPORT="${PELOAD_STATS_HTTP:-8765}"
if [ "$SPORT" != 0 ]; then
  waited=0
  while pids=$(lsof -nP -iTCP:"$SPORT" -sTCP:LISTEN -t 2>/dev/null) && [ -n "$pids" ]; do
    for p in $pids; do
      cmd=$(ps -o command= -p "$p" 2>/dev/null)
      case "$cmd" in *h3bsp.py*|*h3nav.py*|*maptop.sh*) kill -9 "$p" 2>/dev/null; echo "stats port $SPORT: killed a leftover map generator (pid $p)";;
        *) [ $waited = 0 ] && echo "stats port $SPORT is in use by pid $p ($cmd) -- waiting for it to close (Ctrl-C to give up)";; esac
    done
    waited=1; sleep 1
  done
fi
rm -f "$PFX/ReclaimerProfile/starting" 2>/dev/null
RUNTIME=()
ARGS=(game-client --mcc-path "$MCC")
if [ -n "${RECLAIMER_JOIN:-}" ]; then
  # The same link a browser or Discord hands the client; it finds MCC itself and joins once its menu is up.
  ARGS=("reclaimer://join?server=$RECLAIMER_JOIN")
  echo "starting Reclaimer's menu, then joining $RECLAIMER_JOIN; log: /tmp/reclaimer_play.log"
elif [ -n "${RECLAIMER_DIRECT:-}" ]; then
  # Direct match: unset RECLAIMER_LOCAL_RUNTIME (the client's default "menu:0" boots its frontend menu).
  RUNTIME=(PELOAD_LOCAL_RUNTIME="__unset__")
  echo "starting (~12 s to gameplay: loading screen, then the match); log: /tmp/reclaimer_play.log"
else
  echo "starting Reclaimer's menu (~17 s); log: /tmp/reclaimer_play.log"
fi
[ "${PELOAD_STATS_HTTP:-8765}" != 0 ] && echo "live stats: http://127.0.0.1:${PELOAD_STATS_HTTP:-8765}/"
# top-down map pictures for the stats page's heatmap (made once from the .map files; needs python3 + numpy + Pillow)
# (every launch: it only makes the ones still missing, e.g. newly downloaded Workshop maps; the stats page also makes the
# picture of any map played that has none)
[ "${PELOAD_STATS_HTTP:-8765}" != 0 ] && [ -f shim/tools/maptop.sh ] && \
  (bash shim/tools/maptop.sh > /tmp/reclaimer_maptop.log 2>&1 &)
# The native GL helper (shim/build/glhelper, PELOAD_GL_HELPER=1, the default): Apple's GL driver runs natively in its own
# process instead of translated inside this one -- about 2-3x the frame rate in big games. PELOAD_GL_HELPER=0 = the old
# in-process GL. Particle compute workers: the performance cores minus two (4..8); PELOAD_CS_WORKERS overrides.
# Import fast paths (shim/src/fastcall.c: per-thread heap block cache, cached thread ids, key-state table): on since
# 2026-10-18 (+1.3% median in a 128-player match; ~40 map loads without a fault); PELOAD_FASTCALL=0 = off.
PC="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo 6)"; CSW=$((PC-2)); [ "$CSW" -lt 4 ] && CSW=4; [ "$CSW" -gt 8 ] && CSW=8
[ -x shim/build/glhelper ] || { [ "${PELOAD_GL_HELPER:-1}" != 0 ] && echo "shim/build/glhelper missing (bash shim/build.sh): GL stays in-process"; }
# in a source checkout the stats page is served from shim/src (edits show on a browser refresh)
[ -f shim/src/stats_page.html ] && [ -z "${PELOAD_STATS_PAGE:-}" ] && export PELOAD_STATS_PAGE="$PWD/shim/src/stats_page.html"
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 "${RUNTIME[@]}" PELOAD_STATS_HTTP="${PELOAD_STATS_HTTP:-8765}" PELOAD_STATS_OPEN="${PELOAD_STATS_OPEN:-1}" \
    PELOAD_D3DGL_SUBMIT="${PELOAD_D3DGL_SUBMIT:-auto}" PELOAD_GL_HELPER="${PELOAD_GL_HELPER:-1}" PELOAD_CS_WORKERS="${PELOAD_CS_WORKERS:-$CSW}" \
    PELOAD_FASTCALL="${PELOAD_FASTCALL:-all}" \
    PELOAD_RECLAIMER_PATCHES=all PELOAD_WORKER_INHERIT_STDIO=1 PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 \
    arch -x86_64 shim/build/peload "$EXE" "${ARGS[@]}" < <(exec sleep 86400.$$ 2>/dev/null) > /tmp/reclaimer_play.log 2>&1
# the stdin feeder above (an open pipe that never sends anything) must not outlive the game: it used to hold the
# caller's stderr for 24 h (so `play.sh | tail` never returned) and piled up as orphaned shells. bash 3.2 reparents
# it to launchd, so it is found by its pid-tagged duration.
rc=$?; pkill -f "^sleep 86400\.$$\$" 2>/dev/null; exit $rc
