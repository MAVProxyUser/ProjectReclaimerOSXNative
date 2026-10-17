#!/bin/bash
# Install Project Reclaimer for Mac from a stick made by shim/tools/make-usb.sh (lands on the stick as
# ReclaimerMac/install.sh). Copies:
#   app/      -> ~/ReclaimerMac (the loader, launch scripts, VC++ runtime DLLs, your client exe)   [or the folder you pass]
#   game/     -> ~/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/
#                (exactly where ./reclaimer-mac.sh download / DepotDownloader puts the MCC files)
#   drive_c/  -> ~/Library/Application Support/ReclaimerMac/prefix/drive_c/ (Reclaimer caches, fonts, profile if copied)
# Resumable: run it again if it was interrupted. Then play with:  cd ~/ReclaimerMac && bash shim/play.sh
#
#   bash /Volumes/<stick>/ReclaimerMac/install.sh [install-folder]
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
APP="${1:-$HOME/ReclaimerMac}"
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
STEAM="$PFX/Program Files/Steam/steamapps/common"

[ "$(uname -m)" = "arm64" ] || echo "note: this is meant for Apple Silicon Macs"
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  echo "Rosetta 2 is not installed. Install it first (it asks for your password), then run this again:"
  echo "   softwareupdate --install-rosetta --agree-to-license"
  exit 1
fi
[ -d "$SRC/game/Halo The Master Chief Collection/halo3" ] || { echo "no MCC files on the stick ($SRC/game)"; exit 1; }

NEED=$(du -sk "$SRC" | cut -f1); HAVE=$(df -k "$HOME" | awk 'NR==2{print $4}')
echo "copying ~$((NEED/1048576)) GB; ~$((HAVE/1048576)) GB free on this Mac"
[ "$HAVE" -gt "$NEED" ] || { echo "not enough free space"; exit 1; }

R="rsync -a --exclude=.DS_Store"
echo "== loader -> $APP"
mkdir -p "$APP"; $R "$SRC/app/" "$APP/"
echo "== Reclaimer caches/fonts -> $PFX"
mkdir -p "$PFX"; $R "$SRC/drive_c/" "$PFX/"
echo "== MCC files -> $STEAM (takes a while)"
mkdir -p "$STEAM"; $R --progress "$SRC/game/" "$STEAM/"

xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
chmod +x "$APP/shim/build/peload" "$APP"/shim/*.sh "$APP"/shim/tools/*.sh 2>/dev/null || true
echo
echo "Installed. Play with:"
echo "   cd \"$APP\" && bash shim/play.sh"
echo "(first launch: about 20 s to the menu; click the window to use keyboard and mouse, F1 releases it)"
