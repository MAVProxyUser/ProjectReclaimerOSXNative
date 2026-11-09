#!/bin/bash
# Watch for a new Project Reclaimer client release and port nhook to it (2026-11-09; the user: "I see that .2 is out.. ensure you pull
# it and update everything, ensure all the hooks work, and continue testing. while you work the patches out stay on this current
# server since it is on .1"). Every <secs>: GitHub's latest release of ProjectReclaimer/project-reclaimer-releases (the app's own
# updater asks the same: RECLAIMER_UPDATE_URL). A tag other than <current> ->
#   1. its macOS client zip + SHA256SUMS.txt into ReclaimerMac/native/<tag>/ (the zip checked against the sums),
#   2. the app unpacked there (client/Project Reclaimer.app) -- NOT into /Applications: the public walker keeps the installed build
#      until its server moves (a game running from /Applications must not have its binary replaced under it),
#   3. nhook ported: re-tools/machoport.py port re-tools/machoport-sigs.json <binary> --apply (napp_builds.h + app-builds.txt),
#      nhook rebuilt,
#   4. the hooks self test (shim/tools/native-hooks-test.sh) against that app, beside the walker (HOOKS_TEST_BESIDE=1, port 8781),
# then a summary and the end. Nothing is installed; "install" is the user's call (ditto into /Applications once the servers move).
#   bash shim/tools/release-watch.sh [current v0.10.1] [secs 300]
set -u
cd "$(dirname "$0")/../.."
CUR=${1:-v0.10.1}; EVERY=${2:-300}; REPO=ProjectReclaimer/project-reclaimer-releases
S="$HOME/Library/Application Support/ReclaimerMac/native"
while true; do
  TAG=$(curl -fsSL -m 20 "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])' 2>/dev/null)
  if [ -n "$TAG" ] && [ "$TAG" != "$CUR" ]; then break; fi
  echo "$(date +%H:%M) latest ${TAG:-?}: nothing new"; sleep "$EVERY"
done
echo "$(date +%H:%M) NEW RELEASE $TAG"
D="$S/$TAG"; mkdir -p "$D"; Z="project-reclaimer-client-$TAG-macos-aarch64.zip"; BASE="https://github.com/$REPO/releases/download/$TAG"
curl -fsSL -m 600 -o "$D/SHA256SUMS.txt" "$BASE/SHA256SUMS.txt" || { echo "FAIL the sums did not download"; exit 1; }
curl -fsSL -m 900 -o "$D/$Z" "$BASE/$Z" || { echo "FAIL $Z did not download"; exit 1; }
WANT=$(grep " $Z\$" "$D/SHA256SUMS.txt" | awk '{print $1}'); GOT=$(shasum -a 256 "$D/$Z" | awk '{print $1}')
[ -n "$WANT" ] && [ "$WANT" = "$GOT" ] || { echo "FAIL $Z does not match SHA256SUMS.txt ($GOT vs ${WANT:-none})"; exit 1; }
echo "  $Z checked against SHA256SUMS.txt"
rm -rf "$D/client"; mkdir -p "$D/client"; ditto -x -k "$D/$Z" "$D/client" || { echo "FAIL unzip"; exit 1; }
APPD=$(find "$D/client" -maxdepth 3 -name 'Project Reclaimer.app' -type d | head -1); BIN="$APPD/Contents/MacOS/project-reclaimer"
[ -x "$BIN" ] || { echo "FAIL no app binary in the zip"; exit 1; }
echo "  app: $APPD ($(defaults read "$APPD/Contents/Info" CFBundleShortVersionString 2>/dev/null))"
python3 re-tools/machoport.py port re-tools/machoport-sigs.json "$BIN" --apply 2>&1 | sed 's/^/  /'
bash shim/native/build.sh 2>&1 | grep -E 'error|built' | sed 's/^/  /'
echo "  the hooks self test against $TAG (beside the walker):"
HOOKS_TEST_BESIDE=1 NATIVE_APP="$APPD" bash shim/tools/native-hooks-test.sh 2>&1 | sed 's/^/    /'
echo "$(date +%H:%M) $TAG: ported; nothing installed (the walker stays on $CUR)"
