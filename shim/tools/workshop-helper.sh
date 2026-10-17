#!/bin/bash
# The Reclaimer client's Workshop helper, done natively with DepotDownloader (shim/docs/workshop-helper.md). When a
# server needs a mod you don't have, the client's "REQUIRED MODS" screen starts its own helper
# (`workshop-download --subscribe|--update <id>`), which needs a running Steam client. The loader runs this script
# instead, with its stdout on the client's pipe, so the screen's DOWNLOAD / TRY AGAIN fetch the mod with DepotDownloader.
# It prints the helper's JSON lines:
#   {"Progress":{"done":<bytes>,"total":<bytes>}} ... then {"Ready":{"root":"<Windows folder>","subscribed":true}}
#   or {"Error":"<text>"}
# Your Steam account name comes from $STEAM_USER or the first line of ~/Library/Application Support/ReclaimerMac/steam_user.
# DepotDownloader must already be signed in (run it once in a terminal, shim/docs/mods.md): this never asks for, or
# passes, a password -- an expired login comes back to the client as an error. Log: $TMPDIR/reclaimer_workshop_<id>.log
#     workshop-helper.sh <id> <folder, Windows form> <folder, Mac form>
ID="$1"; ROOT="$2"; DIR="$3"
esc(){ printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\r\n'; }
err(){ printf '{"Error":"%s"}\n' "$(esc "$1")"; exit 0; }
[[ "$ID" =~ ^[0-9]+$ ]] || err "Invalid Workshop item id"
[ -n "$ROOT" ] && [ -n "$DIR" ] || err "Workshop helper: missing folder"
DD="$(command -v DepotDownloader)" || err "DepotDownloader is not installed (brew install --cask depotdownloader)"
U="${STEAM_USER:-}"; [ -z "$U" ] && U="$(head -1 "$HOME/Library/Application Support/ReclaimerMac/steam_user" 2>/dev/null | tr -d '[:space:]')"
[ -n "$U" ] || err "No Steam account name for DepotDownloader: put it in ~/Library/Application Support/ReclaimerMac/steam_user"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
LOG="${TMPDIR:-/tmp}/reclaimer_workshop_$ID.log"; : > "$LOG"
signin_err(){ grep -qiE 'password|expired|rejected|logon|log on|login|steam guard|2fa|two-factor' "$LOG" &&
  err "DepotDownloader is not signed in to Steam (or its saved login expired): run it once in a terminal, see shim/docs/mods.md"; }

# 1. the item's size: its file list only
"$DD" -app 976730 -pubfile "$ID" -username "$U" -remember-password -manifest-only -dir "$T" </dev/null >>"$LOG" 2>&1
TOTAL=$(grep -h 'Total bytes on disk' "$T"/manifest_*.txt 2>/dev/null | head -1 | sed 's/.*: *//; s/[^0-9]//g')
if [ -z "$TOTAL" ]; then signin_err; err "DepotDownloader could not read Workshop item $ID: $(tail -1 "$LOG")"; fi
[ "$TOTAL" -gt 0 ] || TOTAL=1
printf '{"Progress":{"done":0,"total":%s}}\n' "$TOTAL"

# 2. the download into the client's Workshop folder; DepotDownloader prints " 12.34% <file>" (of the whole item)
mkdir -p "$DIR" || err "Cannot create $DIR"
"$DD" -app 976730 -pubfile "$ID" -username "$U" -remember-password -dir "$DIR" </dev/null 2>&1 | tee -a "$LOG" | tr '\r' '\n' |
  awk -v t="$TOTAL" 'match($0,/^[ \t]*[0-9]+(\.[0-9]+)?%/){ p=substr($0,RSTART,RLENGTH); gsub(/[ \t%]/,"",p); d=p*t/100;
      if(d>last+t/400 || (p+0>=100 && !full)){ if(p+0>=100) full=1; printf "{\"Progress\":{\"done\":%.0f,\"total\":%.0f}}\n", d, t; fflush(); last=d } }'

# 3. finished?
if grep -q 'Total downloaded' "$LOG" && [ -f "$DIR/ModInfo.json" ]; then
  printf '{"Progress":{"done":%s,"total":%s}}\n' "$TOTAL" "$TOTAL"
  printf '{"Ready":{"root":"%s","subscribed":true}}\n' "$(esc "$ROOT")"
  # the stats page's top-down pictures of the item's maps (heatmap background), made now rather than when first
  # played: detached (own session, off the client's pipe), native arm64 python (this script may run translated)
  MT="$(cd "$(dirname "$0")" && pwd)/maptop.sh"
  if [ -f "$MT" ] && [ -z "${PELOAD_NO_MAPTOP:-}" ]; then
    find "$DIR" -name '*.map' -print0 2>/dev/null | xargs -0 /usr/bin/python3 -c 'import os,sys; os.setsid(); os.execv("/usr/bin/arch",["arch","-arm64","/bin/bash"]+sys.argv[1:])' "$MT" \
      >>"${TMPDIR:-/tmp}/reclaimer_workshop_${ID}_maptop.log" 2>&1 &
  fi
else
  signin_err; err "DepotDownloader did not finish Workshop item $ID (log: $LOG)"
fi
