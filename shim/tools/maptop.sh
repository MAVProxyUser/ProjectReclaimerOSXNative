#!/bin/bash
# Make the top-down map pictures the live stats page draws its heatmap on: one PNG + JSON (world bounds) per
# multiplayer map (stock and downloaded Workshop maps), from the map's own collision geometry (re-tools/h3bsp.py), into
# ~/Library/Application Support/ReclaimerMac/maptop/. Maps that already have a picture are skipped (FORCE=1 redoes them).
#     bash shim/tools/maptop.sh
# Needs python3 with numpy and Pillow. Takes 1-4 s per map. play.sh runs it in the background when pictures are missing,
# and the stats page makes the picture of any map played that still has none (stats.c st_maptop_ensure).
cd "$(dirname "$0")/../.."
MAPS="${MCC_MAPS:-$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/maps}"
OUT="$HOME/Library/Application Support/ReclaimerMac/maptop"
[ -f re-tools/h3bsp.py ] || { echo "re-tools/h3bsp.py not found"; exit 1; }
python3 -c 'import numpy, PIL' 2>/dev/null || { echo "needs python3 with numpy and Pillow (pip3 install numpy pillow)"; exit 1; }
# plus every Workshop mod's maps (the client keeps them in steamapps/workshop/content/976730/<id>/maps)
WS="${MCC_WORKSHOP:-$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files (x86)/Steam/steamapps/workshop/content/976730}"
mkdir -p "$OUT"; n=0; f=0
for m in "$MAPS"/*.map "$WS"/*/maps/*.map; do
  [ -f "$m" ] || continue
  [ "$(head -c 4 "$m" 2>/dev/null)" = "daeh" ] || continue           # Halo 3 cache files only
  b=$(basename "$m" .map)
  case "$b" in [0-9]*|mainmenu|shared|campaign) continue;; esac      # campaign levels and shared resources
  [ -z "${FORCE:-}" ] && [ -f "$OUT/$b.png" ] && [ -f "$OUT/$b.json" ] && continue
  if python3 re-tools/h3bsp.py "$m" "$OUT/$b.png" --size 1024 >/dev/null 2>&1; then n=$((n+1)); else f=$((f+1)); echo "failed: $b"; fi
done
echo "maptop: $n made, $f failed ($OUT)"
