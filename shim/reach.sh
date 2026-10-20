#!/bin/bash
# Reach Renaissance (Halo: Reach on MCC's haloreach.dll) on the loader: a local match without the menus.
# Guide and status: shim/docs/reach-renaissance.md.
#     bash shim/reach.sh                                   # Slayer on Sword Base, 10-minute rounds
#     bash shim/reach.sh 30_settlement ctf_054 15          # <map> <game variant> <minutes>
# Maps: haloreach/maps/*.map in MCC (20_sword_slayer, 30_settlement, 35_island, 45_aftship, 50_panopticon,
# 52_ivory_tower, 70_boneyard, forge_halo, cex_* ...). Game variants: haloreach/game_variants/*.bin without ".bin"
# (slayer_054, ctf_054, koth_054, oddball_054, infection_054, headhunter_054, juggernaut_054 ...).
# Needs: MCC's Halo: Reach files   DEPOTS="1064225 1064226" ./reclaimer-mac.sh download   (13 GB, your Steam login)
#        Reach Renaissance          unzip ReachRenaissance-v<version>.zip into the prefix's drive_c (C:\ReachRenaissance)
# Log: /tmp/reach_play.log. Its own run folder (logs, profile) is ~/Library/Application Support/ReclaimerMac/reach-runs.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
RR="$PFX/ReachRenaissance"
MCC_DIR="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
[ -f "$RR/bin/host-probe.exe" ] || { echo "Reach Renaissance is not installed: unzip ReachRenaissance-v<version>.zip into $PFX (it makes ReachRenaissance/)"; exit 1; }
[ -f "$MCC_DIR/haloreach/haloreach.dll" ] || { echo "Halo: Reach is not installed in MCC: DEPOTS=\"1064225 1064226\" ./reclaimer-mac.sh download"; exit 1; }
[ -x "$ROOT/shim/build/peload" ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
MAP="${1:-20_sword_slayer}"; MODE="${2:-slayer_054}"; MIN="${3:-10}"
[ -f "$MCC_DIR/haloreach/maps/$MAP.map" ] || { echo "no map $MAP in $MCC_DIR/haloreach/maps"; exit 1; }
[ -f "$MCC_DIR/haloreach/game_variants/$MODE.bin" ] || { echo "no game variant $MODE in $MCC_DIR/haloreach/game_variants"; exit 1; }
RUN="$HOME/Library/Application Support/ReclaimerMac/reach-runs"; mkdir -p "$RUN"
# host-probe's first argument is its run folder; --launch-block is the match template Reach Renaissance's launcher uses
# (data/launch_block_1.bin); --seconds ends the session (one hour, or two rounds past the time limit).
SECS=$(( MIN*120 + 120 )); [ "$SECS" -lt 3600 ] && SECS=3600
cd "$RUN" && exec env PELOAD_PRESENT=1 PELOAD_HOLD_WINDOW=1 PELOAD_D3DGL=1 PELOAD_GL_HELPER="${PELOAD_GL_HELPER:-1}" \
    PELOAD_STATS_HTTP="${PELOAD_STATS_HTTP:-0}" \
    arch -x86_64 "$ROOT/shim/build/peload" "$RR/bin/host-probe.exe" run --launch-block 'C:\ReachRenaissance\data\launch_block_1.bin' \
    --map "$MAP" --mode "$MODE" --time-limit "$MIN" --show-window --seconds "$SECS" > /tmp/reach_play.log 2>&1
