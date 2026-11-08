#!/bin/bash
# Probe (2026-11-08, the soundboard): does the native app's --voice-wav read its file AS IT PLAYS (a named pipe works: live audio)
# and does it leave silent frames unsent? Our local-only server + B (the second test identity, listening) + A (the test profile)
# with --voice-wav <fifo> --voice-talk all; a writer streams a WAV header with an open-ended data size, then 12 s of silence,
# 3 s of a tone, 12 s of silence, 3 s of a tone, in real time. Prints A's "[voice] active ... sent N" lines and B's "heard" lines.
#   bash shim/tools/voice-fifo-probe.sh            test identities and our own server only; refuses beside another app
set -u
cd "$(dirname "$0")/../.."
SUP="$HOME/Library/Application Support/ReclaimerMac/native"
if pgrep -f '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' >/dev/null; then echo "Project Reclaimer is running: not starting a probe"; exit 1; fi
T=$(mktemp -d "${TMPDIR:-/tmp}/voice-fifo.XXXXXX"); F="$T/talk.wav"; mkfifo "$F"
bash shim/tools/native-dedicated.sh start >/dev/null || exit 1
NATIVE_LOG="$T/b.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_STATS_HTTP=8766 NATIVE_PROFILE="$SUP/test-second" \
  RECLAIMER_CACHE_DIR="$SUP/test-second-cache" PELOAD_FORENSIC_DIR="$SUP/test-second-forensics" NHOOK_HEATMAP_DIR="$SUP/test-second-heatmaps" \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/b.out" 2>&1 &
sleep 10
python3 - "$F" "$T/writer.log" <<'EOF' &
import math, struct, sys, time
f = open(sys.argv[1], 'wb'); lg = open(sys.argv[2], 'w'); r = 48000
hdr = b'RIFF' + struct.pack('<I', 0xFFFFFFFF) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, r, r * 2, 2, 16) + b'data' + struct.pack('<I', 0xFFFFFFFF)
f.write(hdr); f.flush(); t0 = time.time(); lg.write('%.1f header written (the reader opened the pipe)\n' % 0); lg.flush()
def chunk(tone, n):
    return b''.join(struct.pack('<h', int(0.25 * 32767 * math.sin(2 * math.pi * 440 * (i / r))) if tone else 0) for i in range(n))
plan = [(0, 12), (1, 3), (0, 12), (1, 3), (0, 6)]; sent = 0
for tone, secs in plan:
    lg.write('%.1f %s for %d s\n' % (time.time() - t0, 'TONE' if tone else 'silence', secs)); lg.flush()
    for k in range(secs * 50):
        f.write(chunk(tone, 960)); f.flush(); sent += 960
        d = t0 + sent / r - time.time()
        if d > 0: time.sleep(d)
lg.write('%.1f done, %d samples\n' % (time.time() - t0, sent)); f.close()
EOF
WP=$!
NATIVE_LOG="$T/a.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server 127.0.0.1:49190 --background --voice-wav "$F" --voice-talk all > "$T/a.out" 2>&1 &
for i in $(seq 1 120); do kill -0 $WP 2>/dev/null || break; sleep 1; done
sleep 4
echo "== writer"; cat "$T/writer.log"
echo "== A (talking from the pipe)"; grep -a -E '\[voice\] (active|capturing|no capture|microphone|wav)|voice-wav|--voice' "$T/a.log" | cut -c1-200 | tail -14
echo "== B (listening)"; grep -a '\[voice\] active' "$T/b.log" | grep -o 'received [0-9]*.*' | cut -c1-160 | tail -10
for o in a b; do P=$(sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$T/$o.out"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; done
bash shim/tools/native-dedicated.sh stop >/dev/null; kill $WP 2>/dev/null; echo "logs: $T"
