#!/bin/bash
# Run Project Reclaimer's headless dedicated server on this Mac (no window, runs in the background):
#     bash shim/dedicated.sh                                  # start it (creates the server folder on first use)
#     RECLAIMER_JOIN=127.0.0.1:49190 bash shim/play.sh        # join it
#     bash shim/dedicated.sh status                           # ask it what it is playing (ready, players, map)
#     bash shim/dedicated.sh stop                             # stop it
# The server waits in its lobby until one player has joined (min_players), then starts the match; `status` says
# "ready": true from then on. A join takes ~20 s after the menu is up.
#
# Server folder: C:\ReclaimerTestServer in the loader's prefix (DED_FOLDER='C:\<name>' for another). On first use
# this runs `dedicated init`, links game/halo3 to your MCC install (nothing is copied) and writes a LOCAL-ONLY
# dedicated.toml: hidden, not listed on any master, no UPnP, public address 127.0.0.1, one Valhalla Slayer server on
# UDP 49190 with RCON on TCP 127.0.0.1:49190. To host for other people, edit it (dedicated.toml.example next to it
# documents every option) -- that publishes the server, so only do it on purpose.
# DED_CONFIG=<file>.toml runs another config in the same folder (e.g. mapchange.toml: three 1-minute games, a test of map changes).
# DED_VOICE=1 / DED_VOICE=0 switches the server's voice chat on / off in its dedicated.toml before it starts (off by default;
# on is needed to exercise a client's microphone path).
# Logs: /tmp/reclaimer_dedicated.log (the supervisor) and /tmp/reclaimer_dedicated_<pid>.log (every game-server
# process: the supervisor reads their output through pipes, so the loader copies it to these files).
cd "$(dirname "$0")/.."
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
STATE="$HOME/Library/Application Support/ReclaimerMac"
PIDFILE="$STATE/dedicated.pgid"
FOLDER="${DED_FOLDER:-C:\\ReclaimerTestServer}"
UFOLDER="$PFX/$(echo "${FOLDER#?:\\}" | tr '\\' '/')"
MCC="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"

EXE="${RECLAIMER_EXE:-}"
if [ -z "$EXE" ]; then
  for c in $(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1); do [ -f "$c" ] && EXE="$c" && break; done
fi
[ -n "$EXE" ] && [ -f "$EXE" ] || { echo "Project Reclaimer client exe not found (see README)"; exit 1; }
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh'"; exit 1; }
pel(){ arch -x86_64 shim/build/peload "$EXE" "$@" < /dev/null 2>&1 | sed 's/\x1b\[[0-9;]*m//g' |
       grep -avE '^\s*\[|^  >>|^peload|^  (\(at|image base|subsystem|mapped at|TEB @|guest |  DllMain|bound imps|static TLS|block 0x|TLS callback|entry @)|^        unimplemented'; }

stop_server(){ [ -f "$PIDFILE" ] && kill -9 -- -"$(cat "$PIDFILE")" 2>/dev/null; rm -f "$PIDFILE"; }
case "${1:-}" in
  stop) stop_server; echo "dedicated server stopped"; exit 0;;
  status) PORT="$(grep -m1 -A3 '^\[\[server\]\]' "$UFOLDER/dedicated.toml" | grep -m1 '^port' | tr -dc 0-9)"
          pel server-status "127.0.0.1:${PORT:-49190}" | grep -aE '^\s*[{}]|^\s*"'; exit 0;;
  "") ;;
  *) echo "usage: bash shim/dedicated.sh [stop|status]"; exit 1;;
esac
stop_server

if [ ! -f "$UFOLDER/dedicated.toml" ]; then
  [ -d "$MCC/halo3" ] || { echo "MCC files not found in: $MCC"; exit 1; }
  echo "creating the server folder $FOLDER"
  pel dedicated init "$FOLDER" | grep -av '^$' | sed 's/^/  /' | head -20
  [ -d "$UFOLDER" ] || { echo "dedicated init did not create $UFOLDER"; exit 1; }
  mkdir -p "$UFOLDER/game"; [ -e "$UFOLDER/game/halo3" ] || ln -s "$MCC/halo3" "$UFOLDER/game/halo3"
  [ -f "$UFOLDER/dedicated.toml" ] && mv "$UFOLDER/dedicated.toml" "$UFOLDER/dedicated.toml.init"
  cat > "$UFOLDER/dedicated.toml" <<EOF
# Local test server: Valhalla Slayer on this Mac only (hidden, not reported anywhere). Written by shim/dedicated.sh;
# dedicated.toml.example documents every option. game/halo3 is a link to the MCC install.
game = "game"
content = "content"
data = "data"
bans = "bans.json"
public_address = "127.0.0.1"
upnp = false
auto_update = false
host_name = "Mac test server"
admins = []
stats_server = "off"

[master]
enabled = false
port = 49175
peers = []
origin = ""

[rcon]
password = "$(LC_ALL=C tr -dc 'a-z0-9' < /dev/urandom | head -c 16)"
address = "127.0.0.1"

[vpn]
lists = []

[defaults]
voice = false
voice_team_only = false
text_chat = true
chat_log = false
votes = false
block_vpn = false
hidden = true
password = ""
max_players = 16
min_players = 1
share_mods = false
share_rate = 20

[[server]]
name = "Valhalla Test"
port = 49190
map = "Valhalla"
game = "Slayer"
EOF
  echo "checking it:"; pel dedicated --config "$FOLDER\\dedicated.toml" check | grep -av '^$' | sed 's/^/  /' | head -20
fi

case "${DED_VOICE:-}" in
  1) sed -i '' 's/^voice = false/voice = true/' "$UFOLDER/dedicated.toml";;
  0) sed -i '' 's/^voice = true/voice = false/' "$UFOLDER/dedicated.toml";;
esac
rm -f /tmp/reclaimer_dedicated_[0-9]*.log
set -m
env PELOAD_SKINGUARD=1 PELOAD_VTGUARD=1 PELOAD_ALLOC_GUARD=1 PELOAD_RECLAIMER_PATCHES=all \
    PELOAD_HIDE_WINDOW=1 PELOAD_AUDIO=0 PELOAD_NO_PAD=1 PELOAD_NO_KBM=1 PELOAD_NO_SYS_CURSOR=1 PELOAD_NO_FOCUS=1 \
    PELOAD_STDERR_TEE='/tmp/reclaimer_dedicated_%p.log' $DED_ENV \
    arch -x86_64 shim/build/peload "$EXE" dedicated --config "$FOLDER\\${DED_CONFIG:-dedicated.toml}" run \
    < /dev/null > /tmp/reclaimer_dedicated.log 2>&1 &
echo $! > "$PIDFILE"
PORT="$(grep -m1 -A3 '^\[\[server\]\]' "$UFOLDER/${DED_CONFIG:-dedicated.toml}" | grep -m1 '^port' | tr -dc 0-9)"
echo "dedicated server starting ($FOLDER); log: /tmp/reclaimer_dedicated.log"
echo "join it:  RECLAIMER_JOIN=127.0.0.1:${PORT:-49190} bash shim/play.sh"
echo "stop it:  bash shim/dedicated.sh stop"
