#!/bin/bash
# What a control-record button bit does on a JOINED client (2026-11-08): our local-only server + one test client holding the given
# bits in the record the control packet is built from (NHOOK_BOT_PKTBTN, nbot.c bot_on_abuild); prints the SERVER's count of the
# player's shots / grenades / kills and the client's own bullets. Sound muted.   bash shim/tools/native-btn-probe.sh <hex bits> [secs]
set -u
cd "$(dirname "$0")/../.."
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/native-btn.XXXXXX"); B=${1:-800}; S=${2:-15}
bash shim/tools/native-dedicated.sh start >/dev/null || exit 1
NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_MUTE_OUTPUT=1 NHOOK_BOT_HOLDBTN="$B" \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/a.out" 2>&1 &
st() { curl -s -m 5 "http://127.0.0.1:$1/stats.json" | python3 -c 'import json,sys; p=[q for q in json.load(sys.stdin).get("players",[])]; print(" ".join("%s:shots %s grenades %s weapon %s" % (q.get("name","?")[:6], q.get("shots"), q.get("grenades"), q.get("weapon")) for q in p) if p else "none")' 2>/dev/null; }
for i in $(seq 1 90); do q=$(st 8767); [ -n "$q" ] && [ "$q" != none ] && break; sleep 1; done
sleep 6; echo "bits 0x$B  before: server $(st 8767)"; sleep "$S"; echo "bits 0x$B  after $S s: server $(st 8767) | client $(st 8765)"
grep -a '\[perf\]' "$T/a.log" | tail -1 | grep -o 'object_delete [0-9]*, bullets [0-9]*'
P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/a.out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }
bash shim/tools/native-dedicated.sh stop >/dev/null; rm -rf "$T"
