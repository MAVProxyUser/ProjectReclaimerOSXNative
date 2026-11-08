#!/bin/bash
# Live check of the voice channel nhook reads in the native app (nstats.c nh_on_opus_body: the mixer entry's channel byte,
# 1 global / 2 team / 3 proximity -> the forensic archive's voice records and the page's "voice_ch"): our native dedicated
# server (shim/tools/native-dedicated.sh, local only) + two test clients. B listens; A talks from a generated WAV with the
# app's own test options (--voice-wav FILE, --voice-talk all|team|nearby|open). B's log must name A's channel.
#   bash shim/tools/native-voice-test.sh [all|nearby|team ...]     (default: all nearby; each one its own A launch)
# Test runs only: refuses to run beside another Project Reclaimer (the user's game included).
set -u
cd "$(dirname "$0")/../.."
APPRE='^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
if pgrep -f "$APPRE" >/dev/null; then echo "Project Reclaimer is running: not starting a test"; exit 1; fi
bash shim/native/build.sh >/dev/null || { echo "FAIL build"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/native-voice.XXXXXX"); SUP="$HOME/Library/Application Support/ReclaimerMac/native"
MODES="${*:-all nearby}"
# 8 s of a speech-like signal (tones with a syllable rhythm), 48 kHz mono, about -12 dBFS
python3 - "$T/talk.wav" <<'EOF'
import math, struct, sys, wave
r = 48000; w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(r)
fr = bytearray()
for i in range(r * 8):
    t = i / r; env = 0.5 + 0.5 * math.sin(2 * math.pi * 3.3 * t)
    v = env * (0.5 * math.sin(2 * math.pi * 220 * t) + 0.3 * math.sin(2 * math.pi * 440 * t) + 0.2 * math.sin(2 * math.pi * 880 * t))
    fr += struct.pack('<h', int(v * 0.25 * 32767))
w.writeframes(bytes(fr)); w.close()
EOF
pid_of() { sed -n 's/^native app pid \([0-9]*\).*/\1/p' "$1"; }
stop_app() { P=$(pid_of "$1"); [ -n "$P" ] && { kill "$P" 2>/dev/null; sleep 2; kill -9 "$P" 2>/dev/null; }; }
bash shim/tools/native-dedicated.sh start || exit 1
NATIVE_LOG="$T/b.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 NHOOK_STATS_HTTP=8766 NATIVE_PROFILE="$SUP/test-second" \
  RECLAIMER_CACHE_DIR="$SUP/test-second-cache" PELOAD_FORENSIC_DIR="$SUP/test-second-forensics" NHOOK_HEATMAP_DIR="$SUP/test-second-heatmaps" \
  bash shim/native.sh --join-server 127.0.0.1:49190 --background > "$T/b.out" 2>&1 &
sleep 10; fail=0
for M in $MODES; do
  want="?"; [ "$M" = all ] && want=global; [ "$M" = nearby ] && want=proximity; [ "$M" = team ] && want=team
  n0=$(grep -c 'talks on channel' "$T/b.log" 2>/dev/null)
  NATIVE_LOG="$T/a_$M.log" NHOOK_STATS_OPEN=0 NATIVE_ALLOW_SECOND=1 bash shim/native.sh --join-server 127.0.0.1:49190 --background \
    --voice-wav "$T/talk.wav" --voice-talk "$M" > "$T/a_$M.out" 2>&1 &
  if [ "$M" = nearby ]; then   # proximity reaches only living players nearby: once both are in the game, the server puts A beside B
    for i in $(seq 1 90); do n=$(curl -s -m 5 http://127.0.0.1:8767/stats.json | python3 -c 'import json,sys; print(sum(1 for p in json.load(sys.stdin).get("players",[]) if p.get("alive")))' 2>/dev/null || echo 0)
      [ "${n:-0}" -ge 2 ] && break; sleep 1; done
    me() { curl -s -m 8 "http://127.0.0.1:$1/stats.json" | python3 -c 'import json,sys; print([p["slot"] for p in json.load(sys.stdin)["players"] if p.get("you")][0])' 2>/dev/null; }
    SA=""; for i in $(seq 1 20); do SA=$(me 8765); [ -n "$SA" ] && break; sleep 1; done; SB=$(me 8766)
    [ -n "$SA" ] && [ -n "$SB" ] && curl -s -m 3 -X POST -H X-Reclaimer-Control:1 "http://127.0.0.1:8767/control?do=tpnext&slot=$SB&who=$SA" >/dev/null && echo "A (slot $SA) put next to B (slot $SB)"
  fi
  got=""; for i in $(seq 1 120); do got=$(grep 'talks on channel' "$T/b.log" 2>/dev/null | sed -n "$((n0 + 1)),\$p" | sed 's/.*talks on channel //' | sort -u | tr '\n' ' ')
    [ -n "$got" ] && break; sleep 1; done
  vc=$(curl -s -m 5 http://127.0.0.1:8766/stats.json | python3 -c 'import json,sys; print(" ".join(s["voice_ch"] for s in json.load(sys.stdin).get("session",[]) if s.get("voice_ch")))' 2>/dev/null)
  grep -E '\[voice\] (capturing|no capture|microphone)|--voice-talk' "$T/a_$M.log" | head -2
  echo "A talks with --voice-talk $M: B heard channel(s): ${got:-none}; B's page: ${vc:-?}"
  case " $got " in *" $want "*) echo "ok   $M -> $want";; *) echo "FAIL $M: wanted $want"; fail=1;; esac
  stop_app "$T/a_$M.out"; sleep 3
done
sleep 3; echo "B's roster lines (each A launch joins and leaves):"
curl -s -m 5 'http://127.0.0.1:8766/feed.json?since=0' | python3 -c 'import json,sys; [print("  ", round(e.get("t",0)%10000,1), e.get("text")) for e in json.load(sys.stdin).get("events",[]) if e.get("kind") in ("join","leave","game")]'
stop_app "$T/b.out"; bash shim/tools/native-dedicated.sh stop
echo "logs: $T"; [ $fail = 0 ] && echo PASS || echo FAIL; exit $fail
