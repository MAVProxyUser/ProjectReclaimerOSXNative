#!/bin/bash
# Fill a USB stick (or any folder) with everything needed to play on another Apple Silicon Mac without downloading
# MCC again: the loader + GL helper, the launch scripts, the VC++ runtime DLLs, the newest Reclaimer client exe, your MCC
# files, your downloaded Workshop mods (maps), the client's caches/fonts and the loader's map pictures / walking graphs /
# shader list. The stick gets ReclaimerMac/install.sh, which copies it all into place on the other Mac.
#
#   bash shim/tools/make-usb.sh /Volumes/<stick> [--update] [--with-profile] [--minimal] [--no-workshop] [--no-game]
#
#   --update        ONLY the loader, GL helper, scripts, docs, newest client exe and install.sh (seconds; no MCC files, no
#                   Workshop mods, no caches / map pictures). Then on the other Mac: install.sh --update
#
#   --with-profile  also copy your Reclaimer profile (nickname, identity key). Don't play on both Macs at once with it.
#   --minimal       only the MCC files Project Reclaimer reads (~25 GB: halo3/, Data/, MCC/Content/Paks, top-level files)
#                   instead of the whole MCC folder (~29 GB)
#   --no-workshop   leave the Workshop mods out (they can be ~15 GB; the client downloads them again when a server needs one)
#   --no-game       skip the MCC files entirely (for a quick refresh of the loader on a stick that already has them)
#
# UPDATING: run the same command again on a stick made before. Only what changed is copied: rsync compares size and
# modification time, so unchanged MCC / Workshop files are skipped (a check of ~30 GB of files takes seconds). The stick
# then holds exactly the current loader, scripts and newest client exe (older exes and removed scripts are deleted there);
# a Workshop mod you deleted here is deleted on the stick too. Each part says how many files it copied. Nothing on the
# stick outside ReclaimerMac/ is touched. These are your own game files -- for your own Macs, not for sharing.
set -e
cd "$(dirname "$0")/../.."
DEST="${1:-}"; shift || true
PROFILE=0; MINIMAL=0; WORKSHOP=1; GAME=1; UPDATE=0
for a in "$@"; do case "$a" in --with-profile) PROFILE=1;; --minimal) MINIMAL=1;; --no-workshop) WORKSHOP=0;; --no-game) GAME=0;; --update) UPDATE=1; GAME=0; WORKSHOP=0;;
  *) echo "unknown option $a"; exit 1;; esac; done
[ -n "$DEST" ] && [ -d "$DEST" ] || { echo "usage: bash shim/tools/make-usb.sh /Volumes/<stick> [--update] [--with-profile] [--minimal] [--no-workshop] [--no-game]"; exit 1; }
SUP="$HOME/Library/Application Support/ReclaimerMac"
PFX="$SUP/prefix/drive_c"
MCC="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
WS="$PFX/Program Files/Steam/steamapps/workshop"
[ $GAME = 0 ] || [ -d "$MCC/halo3" ] || { echo "MCC files not found in: $MCC"; exit 1; }
for b in peload glhelper; do [ -x shim/build/$b ] || { echo "shim/build/$b missing: run 'bash shim/build.sh' first"; exit 1; }; done
EXE="$(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1)"
[ -n "$EXE" ] || EXE="$(ls client/project-reclaimer-*.exe 2>/dev/null | tail -1)"
[ -n "$EXE" ] || { echo "no Reclaimer client exe in client/"; exit 1; }
[ $WORKSHOP = 1 ] && [ ! -d "$WS/content" ] && WORKSHOP=0

OUT="$DEST/ReclaimerMac"
G="$OUT/game/Halo The Master Chief Collection"
# space: only what is not on the stick yet counts (per part: its size here minus its size there)
kb(){ local t=0 x; for x in "$@"; do [ -e "$x" ] && t=$((t + $(du -sk "$x" | cut -f1))); done; echo $t; }
delta(){ local a=$1 b=$2; [ $a -gt $b ] && echo $((a-b)) || echo 0; }
NEED=$(delta "$(kb shim/build/peload shim/build/glhelper "$EXE")" "$(kb "$OUT/app")")
[ $UPDATE = 1 ] || NEED=$((NEED + $(delta "$(kb "$PFX/ReclaimerCache" "$PFX/ReclaimerContent" "$PFX/ReclaimerFonts")" "$(kb "$OUT/drive_c")") - $(kb "$PFX/ReclaimerCache/Temp")))
[ $WORKSHOP = 1 ] && NEED=$((NEED + $(delta "$(kb "$WS")" "$(kb "$OUT/workshop")")))
if [ $GAME = 1 ]; then
  if [ $MINIMAL = 1 ]; then H=$(kb "$MCC/halo3" "$MCC/Data" "$MCC/MCC/Content/Paks"); else H=$(kb "$MCC"); fi
  NEED=$((NEED + $(delta "$H" "$(kb "$G")")))
fi
NEED=$((NEED + 300000))
HAVE=$(df -k "$DEST" | awk 'NR==2{print $4}')
echo "to copy: up to ~$((NEED/1024)) MB (new or grown parts); free on the stick: ~$((HAVE/1048576)) GB"
[ "$HAVE" -gt "$NEED" ] || { echo "not enough space on $DEST"; exit 1; }

mkdir -p "$OUT"
R="rsync -a --exclude=.DS_Store"
# rsync one part: list each file it copies (an update copies a handful) and how many; stop if rsync fails (stick full)
part(){ local name="$1"; shift; local log; log=$(mktemp)
  $R -i "$@" | tee "$log" | awk '/^[>c]f/{ sub(/^[^ ]+ /,""); print "      " $0; fflush() }'; local rc=${PIPESTATUS[0]}
  local n; n=$(grep -c '^[>c]f' "$log" || true); rm -f "$log"
  [ "$rc" = 0 ] || { echo "   $name: rsync failed (exit $rc)"; exit 1; }
  echo "   $name: $n file(s) copied"; }

echo "== loader, GL helper, scripts, newest client exe"
# the app folder is staged exactly (--delete): the stick never keeps an old exe, binary or script next to the new ones
APPST=$(mktemp -d); trap 'rm -rf "$APPST"' EXIT; chmod 755 "$APPST"   # mktemp makes it 0700, which rsync -a would give the stick's app/
mkdir -p "$APPST/shim/build" "$APPST/shim/tools" "$APPST/shim/docs" "$APPST/re-tools" "$APPST/client"
ln shim/build/peload shim/build/glhelper "$APPST/shim/build/" 2>/dev/null || cp -p shim/build/peload shim/build/glhelper "$APPST/shim/build/"
cp -pR shim/crt "$APPST/shim/"
cp -p shim/play.sh shim/target.sh shim/dedicated.sh shim/build.sh "$APPST/shim/"
cp -p shim/tools/quiet-run.sh shim/tools/servers.sh shim/tools/maptop.sh shim/tools/workshop-helper.sh shim/tools/client-update.sh shim/tools/machine.sh "$APPST/shim/tools/"
for f in h3map h3bsp h3nav h3inv h3dmg; do [ -f re-tools/$f.py ] && cp -p re-tools/$f.py "$APPST/re-tools/"; done   # map pictures, walking graphs, invisible walls
for f in mods.md game-options.md FIXES.md; do [ -f shim/docs/$f ] && cp -p shim/docs/$f "$APPST/shim/docs/"; done
cp -p README.md "$APPST/"; cp -p "$EXE" "$APPST/client/"
cp -p shim/tools/usb-install.sh "$APPST/install.sh.new"
part "app (client $(basename "$EXE"))" --delete --exclude=install.sh.new "$APPST/" "$OUT/app/"
cp "$APPST/install.sh.new" "$OUT/install.sh"; chmod +x "$OUT/install.sh"

if [ $UPDATE = 0 ]; then
echo "== Reclaimer caches and fonts"
mkdir -p "$OUT/drive_c"
# ReclaimerCache/Temp = the client's per-launch scratch folders (mcc-startup-*): never copied (and removed from the stick)
for d in ReclaimerCache ReclaimerContent ReclaimerFonts; do [ -d "$PFX/$d" ] && part "$d" --delete --delete-excluded --exclude=/ReclaimerCache/Temp/ "$PFX/$d" "$OUT/drive_c/"; done
[ $PROFILE = 1 ] && part "ReclaimerProfile (your profile)" "$PFX/ReclaimerProfile" "$OUT/drive_c/"

echo "== map pictures, walking graphs, heatmaps, the shader list (made here; saves the other Mac making them again)"
mkdir -p "$OUT/support"
for d in maptop heatmaps; do [ -d "$SUP/$d" ] && part "$d" --delete "$SUP/$d" "$OUT/support/"; done
[ -f "$SUP/glpairs_v1.txt" ] && part "glpairs_v1.txt" "$SUP/glpairs_v1.txt" "$OUT/support/"
fi

if [ $WORKSHOP = 1 ]; then
  echo "== Workshop mods ($(ls "$WS/content/976730" 2>/dev/null | wc -l | tr -d ' ') downloaded) -- only new or changed files"
  mkdir -p "$OUT/workshop"; part "workshop" --delete "$WS/" "$OUT/workshop/"
fi

# Steam's library list + MCC's manifest (tiny): how the client finds MCC by itself (a join link, the menu's MCC check)
[ $UPDATE = 1 ] || mkdir -p "$OUT/steam"; [ $UPDATE = 1 ] || for f in libraryfolders.vdf appmanifest_976730.acf; do [ -f "$PFX/Program Files/Steam/steamapps/$f" ] && cp -p "$PFX/Program Files/Steam/steamapps/$f" "$OUT/steam/"; done

if [ $GAME = 1 ]; then
  echo "== MCC files ($([ $MINIMAL = 1 ] && echo minimal || echo full)) -- only new or changed files"
  mkdir -p "$G"
  if [ $MINIMAL = 1 ]; then
    part "MCC halo3 + Data" "$MCC/halo3" "$MCC/Data" "$G/"; mkdir -p "$G/MCC/Content"; part "MCC Paks" "$MCC/MCC/Content/Paks" "$G/MCC/Content/"
    for f in app.config build_tag.txt installscript.vdf; do [ -f "$MCC/$f" ] && $R "$MCC/$f" "$G/"; done
  else
    part "MCC" "$MCC/" "$G/"
  fi
fi
echo "== on the stick"
du -sh "$OUT"/* | sed 's/^/   /'
echo
if [ $UPDATE = 1 ]; then echo "Done (update: loader, scripts and client only). On the other Mac, plug in the stick and run"
  echo "   bash \"/Volumes/$(basename "$DEST")/ReclaimerMac/install.sh\" --update"
else echo "Done. On the other Mac: plug in the stick, then run (again after each update: it copies only what changed)"
  echo "   bash \"/Volumes/$(basename "$DEST")/ReclaimerMac/install.sh\""; fi
