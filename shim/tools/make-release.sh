#!/bin/bash
# Build the release tarball: the loader, the GL helper, Reclaimer.app (double-click to play), the launch / test / USB /
# Workshop scripts, the native 0.10+ app's tooling (nhook.dylib + shim/native.sh, the profile converter), the map tools the loader runs, the VC++ runtime DLLs and the README.
# No game files and no Reclaimer client exe (players bring their own: shim/tools/client-update.sh fetches it).
#   bash shim/tools/make-release.sh <client version, e.g. 0.9.20> [suffix, e.g. 2]
# -> dist/ProjectReclaimerOSXNative-reclaimer-<version>[-<suffix>]-macos.tar.gz (+ .sha256)
# Releases are named for the Reclaimer client version they support. Built from committed sources only: it refuses when
# shim/, re-tools/ or the README have uncommitted changes (RELEASE_ALLOW_DIRTY=1 overrides).
set -e
cd "$(dirname "$0")/../.."
VER="${1:?client version, e.g. 0.9.20}"; SUF="${2:-}"
NAME="ProjectReclaimerOSXNative-reclaimer-$VER${SUF:+-$SUF}-macos"
if [ -z "${RELEASE_ALLOW_DIRTY:-}" ] && [ -n "$(git status --porcelain -- shim re-tools README.md reclaimer-mac.sh 2>/dev/null)" ]; then
  echo "uncommitted changes in shim/, re-tools/ or the README -- commit them first (or build from a clean worktree):"
  git status --short -- shim re-tools README.md reclaimer-mac.sh; exit 1; fi
ls shim/crt/*.dll >/dev/null 2>&1 || { echo "shim/crt/ needs ucrtbase.dll, vcruntime140.dll, msvcp140.dll (not in git)"; exit 1; }
NO_APP=1 bash shim/build.sh >/dev/null
bash shim/native/build.sh >/dev/null   # nhook.dylib: our tooling inside the native 0.10+ app (shim/native.sh)
D="dist/$NAME"
rm -rf "$D"; mkdir -p "$D/shim/build" "$D/shim/crt" "$D/shim/tools" "$D/shim/docs" "$D/shim/assets" "$D/re-tools" "$D/client"
cp shim/build/peload shim/build/glhelper shim/build/reclaimer-launcher shim/build/nhook.dylib "$D/shim/build/"; cp shim/crt/*.dll "$D/shim/crt/"
# nhook.dylib is built with -g: its symbol table names the build folder (the builder's home path) -- stripped, re-signed ad hoc
strip -S -x "$D/shim/build/nhook.dylib" && /usr/bin/codesign -s - -f "$D/shim/build/nhook.dylib" 2>/dev/null
cp shim/play.sh shim/target.sh shim/dedicated.sh shim/build.sh shim/native.sh "$D/shim/"
cp shim/docs/mods.md shim/docs/game-options.md shim/docs/FIXES.md shim/docs/native-client.md shim/docs/forensics.md "$D/shim/docs/"
cp shim/assets/Reclaimer.icns "$D/shim/assets/"
# the map tools the loader runs in the background (map pictures, invisible walls, walking graph, teleport spots) + helpers
for f in h3map h3bsp h3bounds h3nav h3inv h3spots h3dmg; do [ -f re-tools/$f.py ] && cp re-tools/$f.py "$D/re-tools/"; done
cp shim/tools/quiet-run.sh shim/tools/servers.sh shim/tools/maptop.sh shim/tools/make-usb.sh shim/tools/usb-install.sh \
   shim/tools/machine.sh shim/tools/workshop-helper.sh shim/tools/client-update.sh shim/tools/make-app.sh \
   shim/tools/profile-native.py "$D/shim/tools/"
cp reclaimer-mac.sh README.md "$D/"; touch "$D/client/.gitkeep"
chmod +x "$D/shim/build/peload" "$D/shim/build/glhelper" "$D/shim/build/reclaimer-launcher" "$D"/shim/*.sh "$D"/shim/tools/*.sh "$D/shim/tools/profile-native.py" "$D/reclaimer-mac.sh"
# Reclaimer.app at the top of the bundle: portable (uses the folder it sits in), signed ad hoc
RECLAIMER_APP_VERSION="$VER${SUF:+-$SUF}" bash "$D/shim/tools/make-app.sh" --portable | sed "s#^made .*/Reclaimer.app#Reclaimer.app#"
/usr/bin/codesign --verify --strict "$D/Reclaimer.app" && echo "Reclaimer.app signature verified"
# no home paths in what is published (the builder's user name)
if grep -r -l -a "$HOME" "$D" >/dev/null 2>&1; then echo "refusing: these files contain $HOME:"; grep -r -l -a "$HOME" "$D"; exit 1; fi
( cd dist && COPYFILE_DISABLE=1 tar czf "$NAME.tar.gz" "$NAME" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
ls -la "dist/$NAME.tar.gz"; cat "dist/$NAME.tar.gz.sha256"
