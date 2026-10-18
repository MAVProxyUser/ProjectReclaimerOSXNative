#!/bin/bash
# Fetch the newest Project Reclaimer client release into client/ (servers refuse older clients soon after a release):
#     bash shim/tools/client-update.sh            # newest release
#     bash shim/tools/client-update.sh v0.9.19    # a given tag
# Downloads project-reclaimer-client-<tag>.exe and the release's SHA256SUMS.txt from
# https://github.com/ProjectReclaimer/project-reclaimer-releases, checks the exe against it, and only then moves it into
# client/. The launchers (play.sh, quiet-run.sh, target.sh, dedicated.sh, servers.sh) use the newest
# client/project-reclaimer-client-v*.exe. Nothing is run.
set -e
cd "$(dirname "$0")/../.."
REPO=ProjectReclaimer/project-reclaimer-releases
TAG="$1"
if [ -z "$TAG" ]; then
  TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
fi
NAME="project-reclaimer-client-$TAG.exe"
if [ -f "client/$NAME" ]; then echo "client/$NAME is already here"; exit 0; fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
BASE="https://github.com/$REPO/releases/download/$TAG"
echo "downloading $NAME ($TAG) ..."
curl -fL --progress-bar -o "$TMP/$NAME" "$BASE/$NAME"
curl -fsSL -o "$TMP/SHA256SUMS.txt" "$BASE/SHA256SUMS.txt"
WANT=$(grep -i " \*\?$NAME\$" "$TMP/SHA256SUMS.txt" | awk '{print tolower($1)}' | head -1)
GOT=$(shasum -a 256 "$TMP/$NAME" | awk '{print $1}')
if [ -z "$WANT" ]; then echo "SHA256SUMS.txt has no line for $NAME: not installed"; exit 1; fi
if [ "$WANT" != "$GOT" ]; then echo "checksum mismatch ($GOT, expected $WANT): not installed"; exit 1; fi
mkdir -p client && mv "$TMP/$NAME" "client/$NAME"
echo "client/$NAME installed (SHA-256 $GOT)"
echo "next: bash shim/play.sh   (the launch log's [sigpatch] lines say which client code paths were found in this build)"
