#!/bin/bash
# Play Guardian through the game client on the D3D11->GL renderer (no Wine).
# Controller: any GameController-supported pad (Xbox, DualSense, ...) via the shim's XInput.
# Keyboard + mouse: click the game window to capture them (the client's own behavior); F1 or switching
#   windows releases. Cmd-key shortcuts (Cmd+Tab, Cmd+Q) still go to macOS.
#   PELOAD_INVERT_PITCH=0      normal (non-inverted) look; inverted is the default
#   PELOAD_INPUT_SCRIPT=walk   scripted turn/walk test (PELOAD_INPUT_LOOP=1 repeats it)
#   PELOAD_AUDIO=0             silence (game audio plays through CoreAudio by default)
#   PELOAD_VSYNC=1             vsync the window (off by default: frames are never held back)
#   RECLAIMER_EXE=<path>       the Project Reclaimer v0.9.13 client exe (default: client/ or analysis/)
#   RECLAIMER_MENU=1           start in Reclaimer's own frontend menu (server browser, host game, settings)
#                              instead of jumping straight into a Guardian match
# The window title shows elapsed time, the load time, the engine frame rate, draw calls and frame time.
# Extra env can be passed in front:  PELOAD_INVERT_PITCH=0 bash shim/play.sh
# Engine/worker output goes to /tmp/reclaimer_play.log. Quit by closing the window or Ctrl-C.
cd "$(dirname "$0")/.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
MCC="C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection"
MCC_DIR="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
# The code patches below are offsets into this exact build of the client.
EXE_SHA256="a393504dc32a8fd35dabb5b26ea1427fbaedabdac76b00133d49ebc44fb8efd1"
XA="exe+460294:909090909090,exe+4602cb:909090909090,exe+460302:909090909090,exe+460335:9090,exe+65ddf9:eb"

EXE="${RECLAIMER_EXE:-}"
if [ -z "$EXE" ]; then
  for c in client/project-reclaimer-v0.9.13.exe analysis/project-reclaimer-v0.9.13.exe; do
    [ -f "$c" ] && EXE="$c" && break
  done
fi
if [ -z "$EXE" ] || [ ! -f "$EXE" ]; then
  echo "Project Reclaimer v0.9.13 not found. Download project-reclaimer-v0.9.13.exe from"
  echo "https://projectreclaimer.dev and put it in: $(pwd)/client/"
  exit 1
fi
if [ "$(shasum -a 256 "$EXE" | cut -d' ' -f1)" != "$EXE_SHA256" ]; then
  echo "warning: $EXE is not the v0.9.13 build this loader is patched for (SHA-256 mismatch)."
fi
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh' (or use a release bundle)"; exit 1; }
[ -d "$MCC_DIR/halo3" ] || { echo "MCC files not found in: $MCC_DIR"; echo "run: ./reclaimer-mac.sh download"; exit 1; }

pkill -9 -f 'build/peload' 2>/dev/null; sleep 1
rm -f "$PFX/ReclaimerProfile/starting" 2>/dev/null
# Direct match: unset RECLAIMER_LOCAL_RUNTIME (the client's default "menu:0" boots its frontend menu).
RUNTIME=(PELOAD_LOCAL_RUNTIME="__unset__")
if [ -n "${RECLAIMER_MENU:-}" ]; then
  RUNTIME=()
  echo "starting Reclaimer's menu (~17 s); log: /tmp/reclaimer_play.log"
else
  echo "starting (~12 s to gameplay: loading screen, then the match); log: /tmp/reclaimer_play.log"
fi
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 "${RUNTIME[@]}" \
    PELOAD_PATCH="$XA" PELOAD_WORKER_INHERIT_STDIO=1 PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 \
    arch -x86_64 shim/build/peload "$EXE" \
    game-client --mcc-path "$MCC" < <(sleep 86400) > /tmp/reclaimer_play.log 2>&1
