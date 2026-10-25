#!/bin/bash
# The corpus for porting work: every Project Reclaimer CLIENT release exe in
#   ~/Library/Application Support/ReclaimerMac/releases/<tag>/<asset>   (+ that release's SHA256SUMS.txt)
# so re-tools/patchscan.py can check the loader's signatures against all of them (shim/docs/porting.md).
#   bash shim/tools/fetch-releases.sh            # every release missing locally
#   bash shim/tools/fetch-releases.sh v0.9.23    # one tag
# Each exe is checked against its release's SHA256SUMS.txt before it is kept; a verified copy already in client/ is reused.
# Only the client exe is fetched (v0.8.1 .. v0.8.5 "project-reclaimer.exe", v0.8.6 .. v0.9.17 "project-reclaimer-v<ver>.exe",
# from v0.9.18 "project-reclaimer-client-v<ver>.exe"). Nothing is run. Data only: never start an old release (servers refuse
# old clients anyway).
set -e
cd "$(dirname "$0")/../.."
REPO=ProjectReclaimer/project-reclaimer-releases
DEST="$HOME/Library/Application Support/ReclaimerMac/releases"; mkdir -p "$DEST"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
curl -fsSL "https://api.github.com/repos/$REPO/releases?per_page=100" > "$TMP/rel.json"
python3 - "$TMP/rel.json" "${1:-}" > "$TMP/list" <<'EOF'
import json,sys,re
want=sys.argv[2]
for r in json.load(open(sys.argv[1])):
    t=r["tag_name"]
    if want and t!=want: continue
    names=[a["name"] for a in r["assets"]]
    exe=[n for n in names if re.fullmatch(r"project-reclaimer(-client)?(-v[0-9.]+)?\.exe",n)]
    if exe and "SHA256SUMS.txt" in names: print(t, exe[0])
EOF
[ -s "$TMP/list" ] || { echo "no matching release"; exit 1; }
while read -r TAG EXE; do
  D="$DEST/$TAG"; [ -f "$D/$EXE" ] && [ -f "$D/SHA256SUMS.txt" ] && { echo "$TAG: have $EXE"; continue; }
  mkdir -p "$D"; BASE="https://github.com/$REPO/releases/download/$TAG"
  curl -fsSL -o "$D/SHA256SUMS.txt" "$BASE/SHA256SUMS.txt"
  WANT=$(grep -i " \*\?$EXE\$" "$D/SHA256SUMS.txt" | awk '{print tolower($1)}' | head -1)
  [ -n "$WANT" ] || { echo "$TAG: SHA256SUMS.txt has no line for $EXE -- skipped"; continue; }
  if [ -f "client/$EXE" ] && [ "$(shasum -a 256 "client/$EXE" | cut -d' ' -f1)" = "$WANT" ]; then cp "client/$EXE" "$D/$EXE"; echo "$TAG: $EXE (from client/, verified)"; continue; fi
  curl -fL --progress-bar -o "$TMP/$EXE" "$BASE/$EXE"
  GOT=$(shasum -a 256 "$TMP/$EXE" | cut -d' ' -f1)
  [ "$GOT" = "$WANT" ] || { echo "$TAG: checksum mismatch ($GOT, expected $WANT) -- not kept"; continue; }
  mv "$TMP/$EXE" "$D/$EXE"; echo "$TAG: $EXE (downloaded, verified)"
done < "$TMP/list"
