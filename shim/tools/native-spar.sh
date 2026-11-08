#!/bin/bash
# A sparring partner for the native app (the loader's shim/target.sh PELOAD_SPAR=1 PELOAD_NAV=enemy): the SECOND test identity
# (ReclaimerMac/native/test-second, its own player) joins a server and turns toward the nearest enemy's head (nbot.c). It never
# fires, and on a server it stands where it spawned: a joined client's moves come from the app's own input, which the walker
# can't drive (nbot.c; the walker works in a local game). Its stats page is http://127.0.0.1:8766/ (Sparring aim button, Walk list).
#   bash shim/tools/native-spar.sh [start [host:port] | stop]      default server: our local dedicated server (started if needed)
# Test identities only, and only a local game or a server at a loopback / private address: nbot.c refuses anything else.
# Join the same server with your own client to practise: bash shim/native.sh --join-server 127.0.0.1:49190 (NATIVE_REAL=1).
set -u
cd "$(dirname "$0")/../.."
SUP="$HOME/Library/Application Support/ReclaimerMac/native"; PIDF="$SUP/spar.pid"; LOG="${NATIVE_LOG:-/tmp/reclaimer_spar.log}"
cmd="${1:-start}"
if [ "$cmd" = stop ]; then
  P=$(cat "$PIDF" 2>/dev/null); [ -n "$P" ] && kill "$P" 2>/dev/null && sleep 2 && kill -9 "$P" 2>/dev/null; rm -f "$PIDF"; echo "sparring partner stopped"; exit 0; fi
SRV="${2:-127.0.0.1:49190}"
if [ "$SRV" = 127.0.0.1:49190 ]; then bash shim/tools/native-dedicated.sh status 2>/dev/null | grep -q "^running" || bash shim/tools/native-dedicated.sh start || exit 1; fi
[ -d "$SUP/test-second" ] || { echo "no second test identity at $SUP/test-second (made by the app's first start with that profile folder)"; exit 1; }
NATIVE_LOG="$LOG" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_STATS_HTTP="${SPAR_STATS_HTTP:-8766}" NATIVE_PROFILE="$SUP/test-second" \
  RECLAIMER_CACHE_DIR="$SUP/test-second-cache" PELOAD_FORENSIC_DIR="$SUP/test-second-forensics" NHOOK_HEATMAP_DIR="$SUP/test-second-heatmaps" \
  NHOOK_SPAR="${SPAR_AIM:-1}" NHOOK_NAV="${SPAR_WALK:-off}" \
  bash shim/native.sh --join-server "$SRV" --background > "$LOG.out" 2>&1 &
for i in $(seq 1 20); do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$LOG.out" 2>/dev/null); [ -n "$P" ] && break; sleep 1; done
[ -n "${P:-}" ] || { echo "the sparring partner did not start (see $LOG.out)"; exit 1; }
echo "$P" > "$PIDF"; echo "sparring partner: pid $P on $SRV, page http://127.0.0.1:${SPAR_STATS_HTTP:-8766}/, log $LOG"
