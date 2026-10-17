#!/bin/bash
# Build the release tarball: the loader, launch/test/USB scripts, the VC++ runtime DLLs and the README.
# No game files and no Reclaimer client exe (players bring their own).
#   bash shim/tools/make-release.sh <client version, e.g. 0.9.18> [suffix, e.g. 2]
# -> dist/ProjectReclaimerOSXNative-reclaimer-<version>[-<suffix>]-macos.tar.gz (+ .sha256)
# Releases are named for the Reclaimer client version they support.
set -e
cd "$(dirname "$0")/../.."
VER="${1:?client version, e.g. 0.9.18}"; SUF="${2:-}"
NAME="ProjectReclaimerOSXNative-reclaimer-$VER${SUF:+-$SUF}-macos"
bash shim/build.sh >/dev/null
rm -rf "dist/$NAME"; mkdir -p "dist/$NAME/shim/build" "dist/$NAME/shim/crt" "dist/$NAME/shim/tools" "dist/$NAME/client"
cp shim/build/peload "dist/$NAME/shim/build/"; cp shim/crt/*.dll "dist/$NAME/shim/crt/"
cp shim/play.sh shim/target.sh shim/dedicated.sh shim/build.sh "dist/$NAME/shim/"
mkdir -p "dist/$NAME/shim/docs"; cp shim/docs/mods.md shim/docs/game-options.md "dist/$NAME/shim/docs/"
cp shim/tools/quiet-run.sh shim/tools/servers.sh shim/tools/make-usb.sh shim/tools/usb-install.sh "dist/$NAME/shim/tools/"
cp reclaimer-mac.sh README.md "dist/$NAME/"; touch "dist/$NAME/client/.gitkeep"
chmod +x "dist/$NAME/shim/build/peload" "dist/$NAME"/shim/*.sh "dist/$NAME"/shim/tools/*.sh "dist/$NAME/reclaimer-mac.sh"
( cd dist && tar czf "$NAME.tar.gz" "$NAME" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
ls -la "dist/$NAME.tar.gz"; cat "dist/$NAME.tar.gz.sha256"
