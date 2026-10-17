#!/bin/bash
# Fill a USB stick (or any folder) with everything needed to play on another Apple Silicon Mac without downloading
# MCC again: the loader, the launch scripts, the VC++ runtime DLLs, your Reclaimer client exe, your MCC files and the
# client's caches/fonts. The stick gets ReclaimerMac/install.sh, which copies it all into place on the other Mac.
#
#   bash shim/tools/make-usb.sh /Volumes/<stick> [--with-profile] [--minimal]
#
#   --with-profile  also copy your Reclaimer profile (nickname, identity key). Don't play on both Macs at once with it.
#   --minimal       only the MCC files Project Reclaimer reads (~25 GB: halo3/, Data/, MCC/Content/Paks, top-level files)
#                   instead of the whole MCC folder (~29 GB)
# The copy is resumable: run it again to finish an interrupted copy. Nothing on the stick outside ReclaimerMac/ is
# touched. These are your own game files -- for your own Macs, not for sharing.
set -e
cd "$(dirname "$0")/../.."
DEST="${1:-}"; shift || true
PROFILE=0; MINIMAL=0
for a in "$@"; do case "$a" in --with-profile) PROFILE=1;; --minimal) MINIMAL=1;; *) echo "unknown option $a"; exit 1;; esac; done
[ -n "$DEST" ] && [ -d "$DEST" ] || { echo "usage: bash shim/tools/make-usb.sh /Volumes/<stick> [--with-profile] [--minimal]"; exit 1; }
PFX="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c"
MCC="$PFX/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
[ -d "$MCC/halo3" ] || { echo "MCC files not found in: $MCC"; exit 1; }
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh' first"; exit 1; }
EXE="$(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1)"
[ -n "$EXE" ] || EXE="$(ls client/project-reclaimer-*.exe 2>/dev/null | tail -1)"
[ -n "$EXE" ] || { echo "no Reclaimer client exe in client/"; exit 1; }

OUT="$DEST/ReclaimerMac"; mkdir -p "$OUT/app/shim/build" "$OUT/app/shim/crt" "$OUT/app/shim/tools" "$OUT/app/client" "$OUT/drive_c" "$OUT/game"
NEED=$(du -sk "$MCC" | cut -f1); [ $MINIMAL = 1 ] && NEED=$(du -sk "$MCC/halo3" "$MCC/Data" "$MCC/MCC/Content/Paks" | awk '{s+=$1} END{print s}')
NEED=$((NEED + $(du -sk "$PFX/ReclaimerCache" | cut -f1) + 200000))
HAVE=$(df -k "$DEST" | awk 'NR==2{print $4}'); HAVE=$((HAVE + $(du -sk "$OUT" | cut -f1)))
echo "space needed ~$((NEED/1048576)) GB, available ~$((HAVE/1048576)) GB on $(df -h "$DEST" | awk 'NR==2{print $1}')"
[ "$HAVE" -gt "$NEED" ] || { echo "not enough space on $DEST"; exit 1; }

R="rsync -a --exclude=.DS_Store"
echo "== loader and scripts"
$R shim/build/peload "$OUT/app/shim/build/"; $R shim/crt/ "$OUT/app/shim/crt/"
$R shim/play.sh shim/target.sh shim/build.sh "$OUT/app/shim/"; $R shim/tools/quiet-run.sh "$OUT/app/shim/tools/"
$R README.md "$OUT/app/"; $R "$EXE" "$OUT/app/client/"
cp shim/tools/usb-install.sh "$OUT/install.sh"; chmod +x "$OUT/install.sh" "$OUT/app/shim/build/peload"
echo "== Reclaimer caches and fonts"
for d in ReclaimerCache ReclaimerContent ReclaimerFonts; do [ -d "$PFX/$d" ] && $R "$PFX/$d" "$OUT/drive_c/"; done
[ $PROFILE = 1 ] && $R "$PFX/ReclaimerProfile" "$OUT/drive_c/" && echo "   (profile included)"
echo "== MCC files ($([ $MINIMAL = 1 ] && echo minimal || echo full)) -- this takes a while on a USB stick"
G="$OUT/game/Halo The Master Chief Collection"; mkdir -p "$G"
if [ $MINIMAL = 1 ]; then
  $R --progress "$MCC/halo3" "$MCC/Data" "$G/"; mkdir -p "$G/MCC/Content"; $R --progress "$MCC/MCC/Content/Paks" "$G/MCC/Content/"
  for f in app.config build_tag.txt installscript.vdf; do [ -f "$MCC/$f" ] && $R "$MCC/$f" "$G/"; done
else
  $R --progress "$MCC/" "$G/"
fi
echo "== verifying sizes"
du -sh "$OUT"/* | sed 's/^/   /'
echo
echo "Done. On the other Mac: plug in the stick, then run"
echo "   bash \"/Volumes/$(basename "$DEST")/ReclaimerMac/install.sh\""
