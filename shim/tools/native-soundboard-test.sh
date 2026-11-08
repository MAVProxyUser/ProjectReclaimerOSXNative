#!/bin/bash
# Live check of the soundboard (shim/native/nmic.c): our local-only server + B (the second test identity, listening) + A (the test
# profile, capture allowed: RECLAIMER_TEST_SILENT=0, which on this Mac -- no microphone -- gives the app nhook's VIRTUAL microphone;
# --voice-talk all so A transmits). Both clients' sound output is muted (NHOOK_MUTE_OUTPUT: nothing on the speakers).
# A's page gets a generated tone WAV through POST /soundboard/upload, then do=sb&play=; PASS = A's app opened the soundboard
# microphone, B hears A at speaking level while it plays (rms above -40 dBFS) and the clip stops on do=sb&stop.
#   bash shim/tools/native-soundboard-test.sh        test identities and our own server only; refuses beside another app
set -u
cd "$(dirname "$0")/../.."
SUP="$HOME/Library/Application Support/ReclaimerMac/native"
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/native-sb.XXXXXX"); fail=0; mkdir -p "$T/sb"
python3 - "$T/tone.wav" <<'EOF'
import math, struct, sys, wave
r = 44100; w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(r)   # (stereo 44.1 kHz: the decoder converts)
w.writeframes(b''.join(struct.pack('<hh', v, v) for v in (int(0.3 * 32767 * math.sin(2 * math.pi * 330 * i / r) * (0.6 + 0.4 * math.sin(2 * math.pi * 3 * i / r))) for i in range(r * 6))))
w.close()
EOF
bash shim/tools/native-dedicated.sh start >/dev/null || exit 1
NATIVE_LOG="$T/b.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_STATS_HTTP=8766 NHOOK_MUTE_OUTPUT=1 NATIVE_PROFILE="$SUP/test-second" \
  RECLAIMER_CACHE_DIR="$SUP/test-second-cache" PELOAD_FORENSIC_DIR="$SUP/test-second-forensics" NHOOK_HEATMAP_DIR="$SUP/test-second-heatmaps" \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/b.out" 2>&1 &
sleep 8
RECLAIMER_TEST_SILENT=0 NHOOK_MUTE_OUTPUT=1 NHOOK_SOUNDBOARD_DIR="$T/sb" NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background --voice-talk all > "$T/a.out" 2>&1 &
for i in $(seq 1 90); do grep -aq '\[voice\] microphone Reclaimer soundboard' "$T/a.log" 2>/dev/null && break; sleep 1; done
grep -aq '\[voice\] microphone Reclaimer soundboard' "$T/a.log" && echo "ok   A's app opened the soundboard microphone" || { echo "FAIL A's app did not open the soundboard microphone"; fail=1; }
for i in $(seq 1 30); do curl -s -m 2 http://127.0.0.1:8765/stats.json >/dev/null && break; sleep 1; done; sleep 8
up=$(curl -s -m 10 -X POST -H 'X-Reclaimer-Control: 1' --data-binary @"$T/tone.wav" 'http://127.0.0.1:8765/soundboard/upload?name=tone%20test.wav')
echo "$up" | grep -q '"ok":true' && [ -f "$T/sb/tone test.wav" ] && echo "ok   upload: $up" || { echo "FAIL upload: $up"; fail=1; }
curl -s -m 3 http://127.0.0.1:8765/soundboard.json | grep -q 'tone test.wav' && echo "ok   the file list has it" || { echo "FAIL the file list"; fail=1; }
nb0=$(grep -ac 'heard 0x' "$T/b.log")
pl=$(curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' 'http://127.0.0.1:8765/control?do=sb&play=tone%20test.wav'); echo "play: $pl"
sleep 5; st=$(curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin).get("soundboard")))'); echo "A's soundboard state: $st"
sleep 4
loud=$(sed -n "$((nb0 + 1)),\$p" "$T/b.log" | grep -a 'heard 0x' | grep -o 'heard 0x[0-9a-f]*: [0-9]* frames ([0-9]* concealed) rms -[0-9.]*' | awk '{v=$NF; if (v+0 > -40) n++} END {print n+0}')
sed -n "$((nb0 + 1)),\$p" "$T/b.log" | grep -a 'heard 0x' | grep -o 'heard 0x[0-9a-f]*: [0-9]* frames ([0-9]* concealed) rms -[0-9.]* dBFS' | tail -4
[ "$loud" -ge 1 ] && echo "ok   B heard the clip at speaking level ($loud report(s) above -40 dBFS)" || { echo "FAIL B did not hear the clip"; fail=1; }
curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' 'http://127.0.0.1:8765/control?do=sb&play=tone%20test.wav' >/dev/null; sleep 1
curl -s -m 3 -X POST -H 'X-Reclaimer-Control: 1' 'http://127.0.0.1:8765/control?do=sb&stop=1' >/dev/null; sleep 1
pls=$(curl -s -m 3 http://127.0.0.1:8765/stats.json | python3 -c 'import json,sys; print(json.load(sys.stdin)["soundboard"]["playing"])'); [ "$pls" = None ] && echo "ok   stop" || { echo "FAIL stop: still playing $pls"; fail=1; }
grep -a '\[voice\] active' "$T/a.log" | tail -1 | cut -c1-120
for o in a b; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/$o.out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; done
bash shim/tools/native-dedicated.sh stop >/dev/null
[ $fail = 0 ] && { rm -rf "$T"; echo PASS; } || { echo "logs: $T"; echo FAIL; }; exit $fail
