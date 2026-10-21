#!/bin/bash
# Reach Renaissance (Halo: Reach on MCC's haloreach.dll) on the loader, without its menus. Guide: shim/docs/reach-renaissance.md.
#     bash shim/reach.sh                                   # a local match: Slayer on Sword Base, 10-minute rounds
#     bash shim/reach.sh 30_settlement ctf_054 15          # <map> <game variant> <minutes>
#     bash shim/reach.sh servers                           # the public server list (every master Reach Renaissance knows)
#     bash shim/reach.sh join <address>                    # join one: <address> from `servers` (keyXXXX or ip:port)
# Maps: haloreach/maps/*.map in MCC (20_sword_slayer, 30_settlement, 35_island, 45_aftship, 50_panopticon,
# 52_ivory_tower, 70_boneyard, forge_halo, cex_* ...). Game variants: haloreach/game_variants/*.bin without ".bin"
# (slayer_054, ctf_054, koth_054, oddball_054, infection_054, headhunter_054, juggernaut_054 ...).
# Your in-game name: RR_NAME=<name> (default Player). Click the window to capture the mouse; F1 releases it.
# Needs: MCC's Halo: Reach files   DEPOTS="1064225 1064226" ./reclaimer-mac.sh download   (13 GB, your Steam login)
#        Reach Renaissance          unzip ReachRenaissance-v<version>.zip into the prefix's drive_c (C:\ReachRenaissance)
# Log: /tmp/reach_play.log. Run folders (the game's own logs, profile): C:\ReclaimerCache\ReachRenaissance\runs\mac.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
RR="$PFX/ReachRenaissance"
MCC='C:\Program Files (x86)\Steam\steamapps\common\Halo The Master Chief Collection'
MCC_DIR="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
UD="$PFX/ReclaimerCache/ReachRenaissance"; UDW='C:\ReclaimerCache\ReachRenaissance'   # %LOCALAPPDATA%\ReachRenaissance under the loader
[ -f "$RR/bin/host-probe.exe" ] || { echo "Reach Renaissance is not installed: unzip ReachRenaissance-v<version>.zip into $PFX (it makes ReachRenaissance/)"; exit 1; }
[ -f "$MCC_DIR/haloreach/haloreach.dll" ] || { echo "Halo: Reach is not installed in MCC: DEPOTS=\"1064225 1064226\" ./reclaimer-mac.sh download"; exit 1; }
[ -x "$ROOT/shim/build/peload" ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
RUN="$UD/runs/mac"; mkdir -p "$RUN"
LOADER_ENV=(PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 PELOAD_GL_HELPER="${PELOAD_GL_HELPER:-1}" PELOAD_STATS_HTTP="${PELOAD_STATS_HTTP:-0}")
peload(){ arch -x86_64 "$ROOT/shim/build/peload" "$@"; }

case "${1:-}" in
servers)
    cd "$RUN" && peload "$RR/bin/rr-browser.exe" list 2>&1 | grep -v -E '^peload:|DllMain|^  \[|^  (\(at|image|subsystem|mapped|TEB|guest|bound|static|block|TLS|entry)|^        unimpl|^\s*$'
    exit 0 ;;
join)
    ADDR="${2:?usage: bash shim/reach.sh join <address>   (bash shim/reach.sh servers lists them)}"
    # the menus' way: a lease from `rr-browser serve` (it asks the master that lists the server), then the game as a guest
    PORT=11799; while lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; do PORT=$((PORT+1)); done
    ( cd "$RUN" && exec arch -x86_64 "$ROOT/shim/build/peload" "$RR/bin/rr-browser.exe" serve --port $PORT --ui 'C:\ReachRenaissance\themes\classic' > /tmp/reach_browser.log 2>&1 ) &
    BPID=$!; trap 'kill $BPID 2>/dev/null' EXIT
    for i in $(seq 1 40); do curl -s -m 2 -o /dev/null http://127.0.0.1:$PORT/api/servers && break; sleep 0.5; done
    ROW=$(curl -s -m 20 http://127.0.0.1:$PORT/api/servers | python3 -I -c 'import json,sys
d=json.load(sys.stdin); a=sys.argv[1]
for r in d.get("rows",[]):
    if r["address"]==a:
        l=r["listing"]; print(l["name"],"|",l["kind"],"on",l["map"],"|",l["players"],"/",l["max_players"],"players |",r["compat"]["status"],"| mods:",", ".join(m["title"] for m in l.get("mods") or []) or "none")' "$ADDR")
    [ -n "$ROW" ] || { echo "no server $ADDR in the list (bash shim/reach.sh servers)"; exit 1; }
    echo "joining: $ROW"
    LEASE=$(curl -s -m 30 "http://127.0.0.1:$PORT/api/lease?address=$ADDR")
    ID=$(printf '%s' "$LEASE" | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("id",""))' 2>/dev/null)
    MASTER=$(printf '%s' "$LEASE" | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("master",""))' 2>/dev/null)
    [ -n "$ID" ] && [ -n "$MASTER" ] || { echo "no lease for $ADDR: $LEASE"; exit 1; }
    [ -s "$UD/player_id.txt" ] || printf '0009%s' "$(openssl rand -hex 6)" > "$UD/player_id.txt"
    cp -f "${RR_PROFILE:-$RR/data/profile_1.bin}" "$RUN/profile.bin"
    # Reach Renaissance's launcher writes these guest arguments into join-args.txt (0.1.17)
    ARGS=('C:\ReclaimerCache\ReachRenaissance\runs\mac\guest' --armory-appearance "$UDW\\armory-appearance.txt" --profile "$UDW\\runs\\mac\\profile.bin"
        --strings '\\?\C:\ReachRenaissance\data\strings_en.json' --elite-looks --features-on 6e --seconds 86400 --user-id "$(tr -d '\r\n \t' < "$UD/player_id.txt")"
        --user-name "${RR_NAME:-Player}" --mcc "$MCC" --session-players --game-players --players-table 32 --max-players 32 --max-machines 32
        --net-lan --allow-insecure-http --join "$MASTER" "$ID" --port 40012 --next-game --extra-maps '\\?\C:\ReachRenaissance\data\armor'
        --armor-pack rr_elite_pack --elite-armor-slots --show-window)
    cd "$RUN" && env "${LOADER_ENV[@]}" arch -x86_64 "$ROOT/shim/build/peload" "$RR/bin/host-probe.exe" "${ARGS[@]}" > /tmp/reach_play.log 2>&1
    exit $? ;;
esac

MAP="${1:-20_sword_slayer}"; MODE="${2:-slayer_054}"; MIN="${3:-10}"
[ -f "$MCC_DIR/haloreach/maps/$MAP.map" ] || { echo "no map $MAP in $MCC_DIR/haloreach/maps"; exit 1; }
[ -f "$MCC_DIR/haloreach/game_variants/$MODE.bin" ] || { echo "no game variant $MODE in $MCC_DIR/haloreach/game_variants"; exit 1; }
# host-probe's first argument is its run folder; --launch-block is the match template Reach Renaissance's launcher uses
# (data/launch_block_1.bin); --seconds ends the session (one hour, or two rounds past the time limit).
SECS=$(( MIN*120 + 120 )); [ "$SECS" -lt 3600 ] && SECS=3600
cd "$RUN" && exec env "${LOADER_ENV[@]}" \
    arch -x86_64 "$ROOT/shim/build/peload" "$RR/bin/host-probe.exe" 'C:\ReclaimerCache\ReachRenaissance\runs\mac\match' \
    --launch-block 'C:\ReachRenaissance\data\launch_block_1.bin' --mcc "$MCC" --user-name "${RR_NAME:-Player}" \
    --map "$MAP" --mode "$MODE" --time-limit "$MIN" --show-window --seconds "$SECS" > /tmp/reach_play.log 2>&1
