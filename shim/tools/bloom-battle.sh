#!/bin/bash
# A battle for the bullet spread (2026-11-08; the user asked about the AR's / BR's "random bloom"): N walkers (default 4) on our
# LOCAL dedicated server with Battle Rifle + Assault Rifle starts (DED_OPTIONS), every bullet logged by the server -- the game's
# authoritative simulation -- and by each walker's client (NHOOK_SHOTLOG, nhook noverlay.c), then shim/tools/bloom.py on the
# server's log (the truth) and on the clients' (what a client draws). Test identities only, our own server only (walker-battle.sh).
#   bash shim/tools/bloom-battle.sh [walkers 2..4, default 4] [seconds, default 480]
# Logs: ~/Library/Application Support/ReclaimerMac/battles/bloom/<time>/shots_<server|client>_<pid>.txt
set -u
cd "$(dirname "$0")/../.."
D="$HOME/Library/Application Support/ReclaimerMac/battles/bloom/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$D"
DED_OPTIONS='{"player.primary_weapon": 0, "player.secondary_weapon": 1}' NHOOK_SHOTLOG="$D/shots_%r_%p.txt" bash shim/tools/walker-battle.sh "${1:-4}" "${2:-480}" || exit 1
echo "== the server (authoritative)"; python3 shim/tools/bloom.py "$D"/shots_server_*.txt
cp "$HOME/Library/Application Support/ReclaimerMac/tracks/bloom.json" "$D/bloom-server.json" 2>/dev/null
echo "== the clients (each walker's own view)"; python3 shim/tools/bloom.py "$D"/shots_client_*.txt
cp "$HOME/Library/Application Support/ReclaimerMac/tracks/bloom.json" "$D/bloom-clients.json" 2>/dev/null
cp "$D/bloom-server.json" "$HOME/Library/Application Support/ReclaimerMac/tracks/bloom.json" 2>/dev/null; echo "logs: $D"
