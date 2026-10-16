#!/bin/bash
# Unattended test run: ONE instance, own profile (C:\ReclaimerProfileTest), no input/focus/audio, visible window
# (QR_MINIMIZE=1 minimizes it; a minimized client can stall its loading screen), keeps presenting while hidden; stopped (own process group only) after SECS. Log: /tmp/quiet-run.log
#   bash shim/tools/quiet-run.sh "<extra env>" <secs> [client args...]
#   e.g. bash shim/tools/quiet-run.sh "PELOAD_LOCAL_RUNTIME=__unset__" 60      # direct Guardian match
# QR_PAD=1 lets the pad path run (needed for PELOAD_INPUT_SCRIPT scripted input).
# Refuses to start while any other game process is running (never runs beside the user's game).
cd "$(dirname "$0")/../.."
EXTRA="$1"; SECS="${2:-60}"; shift 2
# (a running shim/target.sh target is allowed: it is input-less and has its own process group)
TPG="$(cat "$HOME/Library/Application Support/ReclaimerMac/target.pgid" 2>/dev/null)"
others(){ for p in $(pgrep -f '^[^ ]*build/peload'); do [ -n "$TPG" ] && [ "$(ps -o pgid= -p "$p" | tr -d ' ')" = "$TPG" ] && continue; echo "$p"; done; }
if [ -n "$(others)" ]; then echo "a game is already running - not starting"; ps -axo pid,pgid,etime,command | grep -E '[0-9] [^ ]*build/peload' | cut -c1-120; exit 3; fi
EXE="${QR_EXE:-client/project-reclaimer-client-v0.9.18.exe}"
ARGS=("$@"); [ ${#ARGS[@]} -eq 0 ] && ARGS=(game-client --mcc-path "C:\\Program Files\\Steam\\steamapps\\common\\Halo The Master Chief Collection")
mkdir -p "$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/ReclaimerProfileTest"
rm -f "$HOME"/reclaimer_win_*.bmp; : > /tmp/quiet-run.log
PADENV="PELOAD_NO_PAD=1"; [ -n "${QR_PAD:-}" ] && PADENV=""
MINENV=""; [ -n "${QR_MINIMIZE:-}" ] && MINENV="PELOAD_MINIMIZE=1"
set -m
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_RECLAIMER_PATCHES=all PELOAD_WORKER_INHERIT_STDIO=1 \
  PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_PRESENT_HIDDEN=1 PELOAD_D3DGL=1 PELOAD_FPS_LOG=1 \
  $PADENV PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_FOCUS=1 PELOAD_AUDIO=0 $MINENV \
  PELOAD_TITLE_PREFIX="TEST (no input)" RECLAIMER_PROFILE_DIR='C:\ReclaimerProfileTest' $EXTRA \
  arch -x86_64 shim/build/peload "$EXE" "${ARGS[@]}" < /dev/null > /tmp/quiet-run.log 2>&1 &
PG=$!; sleep "$SECS"; kill -9 -- -$PG 2>/dev/null; sleep 1
if [ -n "$(others)" ]; then echo "WARNING: a game process is still running after cleanup"; ps -axo pid,pgid,command | grep -E '[0-9] [^ ]*build/peload' | cut -c1-120; fi
echo DONE
