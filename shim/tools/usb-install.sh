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
#   bash /Volumes/<stick>/ReclaimerMac/install.sh [install-folder]
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
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

# space: only what this Mac doesn't have yet counts (per part: the stick's size minus the installed size)
kb(){ local t=0 x; for x in "$@"; do [ -e "$x" ] && t=$((t + $(du -sk "$x" | cut -f1))); done; echo $t; }
delta(){ local a=$1 b=$2; [ $a -gt $b ] && echo $((a-b)) || echo 0; }
NEED=$(( $(delta "$(kb "$SRC/app")" "$(kb "$APP")") + $(delta "$(kb "$SRC/game")" "$(kb "$STEAM/common")") \
  + $(delta "$(kb "$SRC/workshop")" "$(kb "$STEAM/workshop")") + $(delta "$(kb "$SRC/drive_c")" "$(kb "$PFX/ReclaimerCache" "$PFX/ReclaimerContent" "$PFX/ReclaimerFonts")") + 300000 ))
HAVE=$(df -k "$HOME" | awk 'NR==2{print $4}')
echo "to copy: up to ~$((NEED/1024)) MB; ~$((HAVE/1048576)) GB free on this Mac"
[ "$HAVE" -gt "$NEED" ] || { echo "not enough free space"; exit 1; }

R="rsync -a --exclude=.DS_Store"
part(){ local name="$1"; shift; local log; log=$(mktemp)
  $R -i "$@" | tee "$log" | awk '/^[>c]f/{ sub(/^[^ ]+ /,""); print "      " $0; fflush() }'; local rc=${PIPESTATUS[0]}
  local n; n=$(grep -c '^[>c]f' "$log" || true); rm -f "$log"
  [ "$rc" = 0 ] || { echo "   $name: rsync failed (exit $rc)"; exit 1; }
  echo "   $name: $n file(s) copied"; }
echo "== loader -> $APP"
# --delete only inside the folders the stick owns (an old client exe, binary or script goes); anything else you keep in
# the install folder stays
mkdir -p "$APP"
for d in client shim/build shim/tools shim/crt re-tools; do [ -d "$SRC/app/$d" ] && mkdir -p "$APP/$d" && part "$d" --delete "$SRC/app/$d/" "$APP/$d/"; done
part "scripts and docs" --exclude=client --exclude=shim/build --exclude=shim/tools --exclude=shim/crt --exclude=re-tools "$SRC/app/" "$APP/"
echo "== Reclaimer caches/fonts -> $PFX"
mkdir -p "$PFX"; part "caches/fonts" "$SRC/drive_c/" "$PFX/"
if [ -d "$SRC/support" ]; then echo "== map pictures, walking graphs, heatmaps, shader list -> $SUP"; mkdir -p "$SUP"; part "support" "$SRC/support/" "$SUP/"; fi
if [ -d "$SRC/workshop" ]; then echo "== Workshop mods -> $STEAM/workshop"; mkdir -p "$STEAM/workshop"; part "workshop" --delete "$SRC/workshop/" "$STEAM/workshop/"; fi
if [ -d "$SRC/steam" ]; then mkdir -p "$STEAM"; for f in "$SRC"/steam/*; do [ -f "$f" ] && [ ! -f "$STEAM/$(basename "$f")" ] && cp -p "$f" "$STEAM/"; done; fi
# the client also looks under "Program Files (x86)\Steam\steamapps": the same folder (a relative link, so it survives a
# different user name)
X86="$PFX/Program Files (x86)/Steam"; mkdir -p "$X86"
[ -e "$X86/steamapps" ] || [ -L "$X86/steamapps" ] || ln -s "../../Program Files/Steam/steamapps" "$X86/steamapps"
if [ -d "$SRC/game" ]; then echo "== MCC files -> $STEAM/common (only new or changed files)"; mkdir -p "$STEAM/common"; part "MCC" "$SRC/game/" "$STEAM/common/"; fi

xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
chmod +x "$APP/shim/build/peload" "$APP/shim/build/glhelper" "$APP"/shim/*.sh "$APP"/shim/tools/*.sh 2>/dev/null || true
echo
echo "Installed. Play with:"
echo "   cd \"$APP\" && bash shim/play.sh"
echo "(first launch: about 20 s to the menu; click the window to use keyboard and mouse, F1 releases it)"
