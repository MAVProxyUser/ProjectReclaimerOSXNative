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
# a size cap (2026-10-19, security review): a server only has to NAME a Workshop item for the client to ask for it
# (and an input-less test client presses Download itself). RECLAIMER_WORKSHOP_MAX_GB (default 32; Ultimate Forge 2.0 is 9.6)
MAXGB="${RECLAIMER_WORKSHOP_MAX_GB:-32}"; [[ "$MAXGB" =~ ^[0-9]+$ ]] || MAXGB=32
[ "$TOTAL" -le $((MAXGB * 1073741824)) ] || err "Workshop item $ID is $((TOTAL / 1073741824)) GB, more than RECLAIMER_WORKSHOP_MAX_GB ($MAXGB GB): not downloaded"
HAVEKB=$(du -sk "$DIR" 2>/dev/null | cut -f1); NEED=$((TOTAL - ${HAVEKB:-0} * 1024)); [ "$NEED" -gt 0 ] || NEED=0   # an update: only what is missing
FREEKB=$(df -k "$(dirname "$DIR")" 2>/dev/null | awk 'NR==2{print $4}'); [ -z "$FREEKB" ] || [ $((FREEKB * 1024)) -gt $((NEED + 1073741824)) ] ||
  err "Not enough disk space for Workshop item $ID: it needs $((NEED / 1048576)) MB more, $((FREEKB / 1024)) MB is free"
printf '{"Progress":{"done":0,"total":%s}}\n' "$TOTAL"

# 2. the download into the client's Workshop folder. DepotDownloader prints " 12.34% <file>" (of the whole item) only when a
# FILE is done, and checks files it already has without a word -- but the client gives up when "done" doesn't grow for
# 45 s ("Steam Workshop download timed out", shim/docs/workshop-helper.md), which a single 1-2 GB map took (Ultimate Forge,
# 2026-10-18). So every 2 s: the larger of its last percentage and the bytes on disk in the item folder (chunks land in
# preallocated sparse files) up to 99%, and at least one byte more than last time while DepotDownloader runs (never the
# total before it finished).
mkdir -p "$DIR" || err "Cannot create $DIR"
"$DD" -app 976730 -pubfile "$ID" -username "$U" -remember-password -dir "$DIR" </dev/null >>"$LOG" 2>&1 &
DDP=$!; trap 'kill $DDP 2>/dev/null; rm -rf "$T"' EXIT; LAST=0
while kill -0 $DDP 2>/dev/null; do
  sleep 2
  PCT=$(tr '\r' '\n' < "$LOG" | grep -Eo '^[[:space:]]*[0-9]+(\.[0-9]+)?%' | tail -1 | tr -d ' \t%')
  D1=$(awk -v p="${PCT:-0}" -v t="$TOTAL" 'BEGIN{ printf "%.0f", p*t/100 }')
  D2=$(( $(du -sk "$DIR" 2>/dev/null | cut -f1 | head -1) * 1024 ))
  D=$(( D1 > D2 ? D1 : D2 )); CAP=$(( TOTAL - TOTAL / 100 )); [ "$D" -gt "$CAP" ] && D=$CAP   # measured: at most 99%
  [ "$D" -le "$LAST" ] && D=$((LAST + 1)); [ "$D" -ge "$TOTAL" ] && D=$((TOTAL - 1))           # the last 1%: the heartbeat
  [ "$D" -gt "$LAST" ] && { printf '{"Progress":{"done":%s,"total":%s}}\n' "$D" "$TOTAL"; LAST=$D; }
done
wait $DDP 2>/dev/null

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
