#!/bin/bash
# A LOCAL-ONLY native dedicated server for testing nhook (backlog N12): the 0.10 app's own `--server FILE.toml` mode, with
# nhook injected, in an isolated folder (ReclaimerMac/native/test-dedicated: its own profile, cache, data). Its config
# (local-test.toml, made on first use) binds 127.0.0.1, is hidden, runs no master, reports no stats, no UPnP, no
# auto-update: nothing is listed anywhere. One server: Guardian Slayer on UDP 49190, an RCON password in rcon-password.txt
# (127.0.0.1 only). The server's stats page: http://127.0.0.1:8767/ (NHOOK_STATS_HTTP).
#   bash shim/tools/native-dedicated.sh start|stop|status|rcon <command...>
# Join it with a test client:  bash shim/native.sh --join-server 127.0.0.1:49190
set -u
cd "$(dirname "$0")/../.."
B="/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer"
D="$HOME/Library/Application Support/ReclaimerMac/native/test-dedicated"; mkdir -p "$D"
LOG="${NATIVE_DED_LOG:-$D/server.log}"; PIDF="$D/server.pid"
cfg() {
  [ -f "$D/local-test.toml" ] && return
  MCC="$(cat "$HOME/Library/Application Support/ProjectReclaimer/mcc-folder.txt")"
  [ -f "$D/rcon-password.txt" ] || { python3 -c 'import secrets; print(secrets.token_hex(12))' > "$D/rcon-password.txt"; chmod 600 "$D/rcon-password.txt"; }
  cat > "$D/local-test.toml" <<EOF
# nhook's LOCAL-ONLY test server (shim/tools/native-dedicated.sh): this Mac only, never listed, nothing reported.
game = "$MCC"
data = "data"
bans = "bans.json"
public_address = "127.0.0.1"
bind_address = "127.0.0.1"
upnp = false
auto_reload = true
auto_update = false
host_name = "nhook local test"
admins = []
stats_server = "off"
[master]
enabled = false
peers = []
[rcon]
password = "$(cat "$D/rcon-password.txt")"
address = "127.0.0.1"
[defaults]
hidden = true
password = ""
max_players = 16
min_players = 1
post_game_seconds = 10
anti_cheat = "enforce"
[[server]]
name = "nhook local test"
port = 49190
map = "Guardian"
game = "Slayer"
EOF
}
case "${1:-status}" in
  start)
    [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null && { echo "already running (pid $(cat "$PIDF"))"; exit 0; }
    if [ -f shim/native/build.sh ]; then bash shim/native/build.sh >/dev/null || { echo "build failed"; exit 1; }; fi   # (a release bundle ships the dylib built)
    [ -f shim/build/nhook.dylib ] || { echo "shim/build/nhook.dylib missing"; exit 1; }; cfg
    (cd "$D" && RECLAIMER_PROFILE_DIR="$D/profile" RECLAIMER_CACHE_DIR="$D/cache" NHOOK_STATS_HTTP="${NHOOK_STATS_HTTP:-8767}" NHOOK_STATS_OPEN=0 \
       PELOAD_FORENSIC_DIR="$D/forensics" NHOOK_HEATMAP_DIR="$D/heatmaps" NHOOK_LOG="$LOG.nhook" \
       NHOOK_HALO3="$(cat "$HOME/Library/Application Support/ProjectReclaimer/mcc-folder.txt")/halo3/halo3.dll" \
       DYLD_INSERT_LIBRARIES="$OLDPWD/shim/build/nhook.dylib" "$B" --server ./local-test.toml < /dev/null > "$LOG" 2>&1 & echo $! > "$PIDF")   # (no nohup: a system binary strips DYLD_*)
    echo "started (pid $(cat "$PIDF")), log $LOG; stats http://127.0.0.1:${NHOOK_STATS_HTTP:-8767}/; join: bash shim/native.sh --join-server 127.0.0.1:49190" ;;
  stop)
    [ -f "$PIDF" ] && { P=$(cat "$PIDF"); pkill -TERM -P "$P" 2>/dev/null; kill "$P" 2>/dev/null; sleep 2; pkill -9 -P "$P" 2>/dev/null; kill -9 "$P" 2>/dev/null; rm -f "$PIDF"; echo stopped; } || echo "not running" ;;
  status)
    [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null && { echo "running (pid $(cat "$PIDF"))"; pgrep -fl -P "$(cat "$PIDF")" | cut -c1-120; } || echo "not running" ;;
  rcon) shift; "$B" --rcon 127.0.0.1:49190 "$(cat "$D/rcon-password.txt")" "$@" ;;
  *) echo "usage: $0 start|stop|status|rcon <command...>"; exit 1 ;;
esac
