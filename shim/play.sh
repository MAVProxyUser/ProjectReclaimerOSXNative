#!/bin/bash
# Play Guardian through the game client on the D3D11->GL renderer (no Wine).
# Controller: any GameController-supported pad (Xbox, DualSense, ...) via the shim's XInput.
#   PELOAD_INVERT_PITCH=0      normal (non-inverted) look; inverted is the default
#   PELOAD_INPUT_SCRIPT=walk   scripted turn/walk test (PELOAD_INPUT_LOOP=1 repeats it)
#   PELOAD_AUDIO=0             silence (game audio plays through CoreAudio by default)
# The window title shows the engine frame rate, draw calls per frame and frame time.
# Extra env can be passed in front:  PELOAD_INVERT_PITCH=0 bash shim/play.sh
# Engine/worker output goes to /tmp/reclaimer_play.log. Quit by closing the window or Ctrl-C.
cd "$(dirname "$0")/.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
MCC="C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection"
XA="exe+460294:909090909090,exe+4602cb:909090909090,exe+460302:909090909090,exe+460335:9090,exe+65ddf9:eb"
pkill -9 -f 'build/peload' 2>/dev/null; sleep 1
rm -f "$PFX/ReclaimerProfile/starting" 2>/dev/null
echo "starting (the window stays black ~15-20 s while the map loads); log: /tmp/reclaimer_play.log"
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_LOCAL_RUNTIME="__unset__" \
    PELOAD_PATCH="$XA" PELOAD_WORKER_INHERIT_STDIO=1 PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 \
    arch -x86_64 shim/build/peload analysis/project-reclaimer-v0.9.13.exe \
    game-client --mcc-path "$MCC" < <(sleep 86400) > /tmp/reclaimer_play.log 2>&1
