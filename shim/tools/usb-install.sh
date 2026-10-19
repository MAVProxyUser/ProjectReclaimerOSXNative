#!/bin/bash
# Install (or update) Project Reclaimer for Mac from a stick made by shim/tools/make-usb.sh (lands on the stick as
# ReclaimerMac/install.sh). Copies:
#   app/      -> ~/ReclaimerMac (the loader + GL helper, launch scripts, VC++ runtime DLLs, the client exe) [or the folder you pass]
#   game/     -> ~/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/
#                (exactly where ./reclaimer-mac.sh download / DepotDownloader puts the MCC files)
#   workshop/ -> .../steamapps/workshop/ (downloaded Workshop mods: their maps)
#   drive_c/  -> ~/Library/Application Support/ReclaimerMac/prefix/drive_c/ (Reclaimer caches, fonts, profile if copied)
#   support/  -> ~/Library/Application Support/ReclaimerMac/ (map pictures, walking graphs, heatmaps, shader list)
#   steam/    -> .../steamapps/ (Steam's library list + MCC's manifest, only where missing: how the client finds MCC)
# Run it again after updating the stick: only new or changed files are copied (and a client exe / script the stick no
# longer has is removed from the install folder). Resumable if interrupted. Then play with:
#   cd ~/ReclaimerMac && bash shim/play.sh
#
#   bash /Volumes/<stick>/ReclaimerMac/install.sh [--update | --keep-on-stick | --minimal | --no-movies | --no-workshop] [--dry-run] [--skip-space-check] [install-folder]
#   --update   ONLY the loader, GL helper, scripts, client exe and teleport spots (seconds): after make-usb.sh --update, on a
#              Mac that has the game already. No MCC files, Workshop mods, caches or map pictures are copied.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
SKIPSPACE=0; NOMOVIES=0; WORKSHOP=1; ONSTICK=0; DRYRUN=0; UPDATE=0
while [ "${1:-}" != "${1#--}" ]; do case "$1" in
  --skip-space-check) SKIPSPACE=1;;
  --update) UPDATE=1;;
  --no-workshop) WORKSHOP=0;;
  --no-movies) NOMOVIES=1;;
  --minimal) NOMOVIES=1; WORKSHOP=0;;
  --keep-on-stick) ONSTICK=1;;
  --dry-run) DRYRUN=1;;
  *) echo "unknown option $1"; exit 1;;
esac; shift; done
# Space (tested 2026-10-18 against a stripped MCC copy): the client REFUSES to start without the Halo 3 campaign maps
# ("requires BOTH Halo 3 Campaign and Halo 3 Multiplayer"), and its engine worker exits without MCC/Content/Paks/
# MCC-WindowsNoEditor.pak (10.2 GB; "MCC Conduit font archive unavailable"). Not needed: MCC/Content/Movies (4.6 GB) and the
# Workshop mods (15 GB; a modded server's mods download when you join it).
#   --no-movies       leave the movies out                                   (~41 GB)
#   --no-workshop     leave the Workshop mods out                            (~31 GB)
#   --minimal         both                                                   (~25 GB)
#   --dry-run         only print what the copy needs and what this Mac has
#   --keep-on-stick   the pak, the movies and the Workshop mods stay on the stick, linked from this Mac (~15 GB);
#                     the stick must be plugged in (as /Volumes/<same name>) to play. Run again without it to copy them.
MCCN="Halo The Master Chief Collection"
EXCL=(); [ $NOMOVIES = 1 ] || [ $ONSTICK = 1 ] && EXCL+=(MCC/Content/Movies); [ $ONSTICK = 1 ] && EXCL+=(MCC/Content/Paks)
[ $ONSTICK = 1 ] && WORKSHOP=0
APP="${1:-$HOME/ReclaimerMac}"
SUP="$HOME/Library/Application Support/ReclaimerMac"
PFX="$SUP/prefix/drive_c"
STEAM="$PFX/Program Files/Steam/steamapps"

[ "$(uname -m)" = "arm64" ] || echo "note: this is meant for Apple Silicon Macs"
V=$(sw_vers -productVersion); case "$V" in 1[0-2].*) echo "macOS $V is too old: the loader needs macOS 13 or newer"; exit 1;; esac
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  echo "Rosetta 2 is not installed. Install it first (it asks for your password), then run this again:"
  echo "   softwareupdate --install-rosetta --agree-to-license"
  exit 1
fi
[ -d "$SRC/game/Halo The Master Chief Collection/halo3" ] || [ -d "$STEAM/common/Halo The Master Chief Collection/halo3" ] ||
  { echo "no MCC files on the stick ($SRC/game) or on this Mac"; exit 1; }

R="rsync -a --exclude=.DS_Store"
part(){ local name="$1"; shift; local log; log=$(mktemp)
  $R -i "$@" | tee "$log" | awk '/^[>c]f/{ sub(/^[^ ]+ /,""); print "      " $0; fflush() }'; local rc=${PIPESTATUS[0]}
  local n; n=$(grep -c '^[>c]f' "$log" || true); rm -f "$log"
  [ "$rc" = 0 ] || { echo "   $name: rsync failed (exit $rc)"; exit 1; }
  echo "   $name: $n file(s) copied"; }
# Reclaimer.app: a double-clickable launcher for shim/play.sh, in the install folder and ~/Applications (shim/tools/make-app.sh)
make_app(){ [ -f "$APP/shim/tools/make-app.sh" ] || return 0; echo "== Reclaimer.app"; bash "$APP/shim/tools/make-app.sh" --applications 2>&1 | sed 's/^/   /' || true; }
app_part(){ echo "== loader -> $APP"
  # --delete only inside the folders the stick owns (an old client exe, binary or script goes); anything else you keep in
  # the install folder stays
  mkdir -p "$APP"
  for d in client shim/build shim/tools shim/crt re-tools; do [ -d "$SRC/app/$d" ] && mkdir -p "$APP/$d" && part "$d" --delete "$SRC/app/$d/" "$APP/$d/"; done
  part "scripts and docs" --exclude=client --exclude=shim/build --exclude=shim/tools --exclude=shim/crt --exclude=re-tools "$SRC/app/" "$APP/"
  xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
  chmod +x "$APP/shim/build/peload" "$APP/shim/build/glhelper" "$APP/shim/build/reclaimer-launcher" "$APP"/shim/*.sh "$APP"/shim/tools/*.sh 2>/dev/null || true; }
if [ $UPDATE = 1 ]; then
  [ $DRYRUN = 1 ] && { echo "(dry run: --update copies only $SRC/app -> $APP)"; exit 0; }
  [ -d "$STEAM/common/Halo The Master Chief Collection/halo3" ] || echo "note: no MCC files on this Mac yet -- run install.sh without --update once"
  app_part; make_app
  if ls "$SRC"/support/maptop/*.spots.json >/dev/null 2>&1; then echo "== teleport spots -> $SUP/maptop"; mkdir -p "$SUP/maptop"
    part "spots" "$SRC"/support/maptop/*.spots.json "$SUP/maptop/"; fi
  echo; echo "Updated. Play: double-click Reclaimer in Applications (or Launchpad), or"; echo "   cd \"$APP\" && bash shim/play.sh"; exit 0; fi
# space: only what this Mac doesn't have yet counts (per part: the stick's size minus the installed size)
kb(){ local t=0 x; for x in "$@"; do [ -e "$x" ] && t=$((t + $(du -sk "$x" | cut -f1))); done; echo $t; }
delta(){ local a=$1 b=$2; [ $a -gt $b ] && echo $((a-b)) || echo 0; }
GAMEKB=$(kb "$SRC/game"); for x in ${EXCL[@]+"${EXCL[@]}"}; do GAMEKB=$((GAMEKB - $(kb "$SRC/game/$MCCN/$x"))); done
WSKB=0; [ $WORKSHOP = 1 ] && WSKB=$(delta "$(kb "$SRC/workshop")" "$(kb "$STEAM/workshop")")
NEED=$(( $(delta "$(kb "$SRC/app")" "$(kb "$APP")") + $(delta "$GAMEKB" "$(kb "$STEAM/common")") \
  + WSKB + $(delta "$(kb "$SRC/drive_c")" "$(kb "$PFX/ReclaimerCache" "$PFX/ReclaimerContent" "$PFX/ReclaimerFonts")") + 300000 ))
# free space: what Finder shows ("available for important usage": includes purgeable space -- iCloud caches, local
# snapshots -- that macOS frees as files are written), else df's (only what is free right now; it was the only number
# used, and refused a Mac with 69.7 GB free in Finder for a ~46 GB copy). Both in KB, like NEED.
DF=$(df -k "$HOME" | awk 'NR==2{print $4}')
IMP=$(osascript -l JavaScript -e 'ObjC.import("Foundation"); var r=$.NSURL.fileURLWithPath("'"$HOME"'").resourceValuesForKeysError(["NSURLVolumeAvailableCapacityForImportantUsageKey"], null); var n=r.objectForKey("NSURLVolumeAvailableCapacityForImportantUsageKey"); String(Math.floor(n.longLongValue/1024))' 2>/dev/null | tr -dc 0-9)
HAVE=$DF; [ -n "$IMP" ] && [ "$IMP" -gt "$DF" ] && HAVE=$IMP
echo "to copy: up to ~$((NEED/1048576)) GB; free on this Mac: ~$((HAVE/1048576)) GB as Finder counts it (~$((DF/1048576)) GB without purgeable space)"
[ $DRYRUN = 1 ] && { echo "(dry run: nothing copied)"; exit 0; }
if [ "$HAVE" -le "$NEED" ]; then
  if [ "$SKIPSPACE" = 1 ]; then echo "not enough free space by that count -- going on anyway (--skip-space-check)"
  else echo "not enough free space (free some, or run again with --skip-space-check)"; exit 1; fi
fi

app_part; make_app
echo "== Reclaimer caches/fonts -> $PFX"
mkdir -p "$PFX"; part "caches/fonts" "$SRC/drive_c/" "$PFX/"
if [ -d "$SRC/support" ]; then echo "== map pictures, walking graphs, heatmaps, shader list -> $SUP"; mkdir -p "$SUP"; part "support" "$SRC/support/" "$SUP/"; fi
if [ -d "$SRC/workshop" ] && [ $WORKSHOP = 1 ]; then echo "== Workshop mods -> $STEAM/workshop"; mkdir -p "$STEAM/workshop"; part "workshop" --delete "$SRC/workshop/" "$STEAM/workshop/"; fi
if [ -d "$SRC/steam" ]; then mkdir -p "$STEAM"; for f in "$SRC"/steam/*; do [ -f "$f" ] && [ ! -f "$STEAM/$(basename "$f")" ] && cp -p "$f" "$STEAM/"; done; fi
# the client also looks under "Program Files (x86)\Steam\steamapps": the same folder (a relative link, so it survives a
# different user name)
X86="$PFX/Program Files (x86)/Steam"; mkdir -p "$X86"
[ -e "$X86/steamapps" ] || [ -L "$X86/steamapps" ] || ln -s "../../Program Files/Steam/steamapps" "$X86/steamapps"
if [ -d "$SRC/game" ]; then echo "== MCC files -> $STEAM/common (only new or changed files${EXCL[*]+; left out: ${EXCL[*]}})"; mkdir -p "$STEAM/common"
  EX=(); for x in ${EXCL[@]+"${EXCL[@]}"}; do EX+=("--exclude=/$MCCN/$x"); done
  part "MCC" ${EX[@]+"${EX[@]}"} "$SRC/game/" "$STEAM/common/"
  if [ $ONSTICK = 1 ]; then   # link what stays on the stick
    for x in MCC/Content/Paks MCC/Content/Movies; do d="$STEAM/common/$MCCN/$x"; [ -d "$d" ] && [ ! -L "$d" ] && continue; mkdir -p "$(dirname "$d")"; ln -sfn "$SRC/game/$MCCN/$x" "$d"; done
    [ -e "$STEAM/workshop" ] && [ ! -L "$STEAM/workshop" ] || ln -sfn "$SRC/workshop" "$STEAM/workshop"
    echo "   linked to the stick: the pak, the movies, the Workshop mods (keep the stick plugged in to play)"; fi; fi

xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
chmod +x "$APP/shim/build/peload" "$APP/shim/build/glhelper" "$APP"/shim/*.sh "$APP"/shim/tools/*.sh 2>/dev/null || true
echo
echo "Installed. Play: double-click Reclaimer in Applications (or Launchpad), or"
echo "   cd \"$APP\" && bash shim/play.sh"
echo "(first launch: about 20 s to the menu; click the window to use keyboard and mouse, F1 releases it)"
