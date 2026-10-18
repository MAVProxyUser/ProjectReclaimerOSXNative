#!/bin/bash
# Offline test of the forensic archive (shim/src/forensic.c): its Ogg Opus writer and the whole archive -> viewer path.
# No game and no loader: it builds shim/tools/forensic-oggtest.c (forensic.c with FX_TEST=1) natively and checks the
# results with ffmpeg / ffprobe. Needs ffmpeg with libopus (Homebrew's has it) and python3.
#   1. ffmpeg encodes test tones to Ogg Opus in several shapes (20 ms CELT mono, 60 ms stereo = code-3 packets,
#      2.5 / 10 / 40 / 120 ms, SILK voip at 12 kbit/s); the harness re-wraps their raw packets with our writer (same
#      pre-skip), reads them back (CRC, packets byte-identical, granules), and ffmpeg must decode our file without a
#      single message to exactly the samples it decodes from its own file (bit-identical PCM).
#   2. 50 x the 20 ms silence packet decode to 1 s of digital silence.
#   3. "pipeline": settings + consent, roster, 600+ voice packets as 4 speakers, a control packet, a malformed one,
#      events, a torn record, a map change -- then every /forensics endpoint; each .ogg it served must decode cleanly
#      to the expected number of samples, and every JSON reply must parse.
# Usage: bash shim/tools/forensic-oggtest.sh      (exit status 0 = all passed; FFMPEG= / FFPROBE= override the tools)
set -uo pipefail
cd "$(dirname "$0")/../.."
FF=${FFMPEG:-$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)}
FP=${FFPROBE:-$(command -v ffprobe || echo /opt/homebrew/bin/ffprobe)}
T=$(mktemp -d "${TMPDIR:-/tmp}/forensic-oggtest.XXXXXX")
fail=0
clang -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-c23-extensions \
  -o "$T/oggtest" shim/tools/forensic-oggtest.c || { echo "FAIL build"; exit 1; }
pcm(){ "$FF" -v error -i "$1" -f s16le -acodec pcm_s16le - 2>"$T/err.txt"; }
decode_ok(){   # <file>: ffmpeg decodes it with no output at all
  local out; out=$("$FF" -v error -i "$1" -f null - 2>&1); local rc=$?
  if [ $rc -ne 0 ] || [ -n "$out" ]; then echo "FAIL ffmpeg decode $1: rc=$rc $out"; fail=1; return 1; fi; return 0; }
samples_of(){ # <file> -> decoded samples per channel
  local ch; ch=$("$FP" -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$1")
  local bytes; bytes=$(pcm "$1" | wc -c | tr -d ' '); echo $(( bytes / (2*ch) )); }

echo "== 1. re-wrapping ffmpeg's Opus packets"
#     name        ffmpeg encoder options
cases=(
  "celt20       -ac 1 -b:a 32k -frame_duration 20"
  "stereo60     -ac 2 -b:a 64k -frame_duration 60"
  "celt2_5      -ac 1 -b:a 64k -frame_duration 2.5"
  "celt10       -ac 1 -b:a 32k -frame_duration 10"
  "s40          -ac 2 -b:a 48k -frame_duration 40"
  "long120      -ac 1 -b:a 32k -frame_duration 120"
  "silk_voip    -ac 1 -b:a 12k -application voip -frame_duration 20"
  "silk60       -ac 1 -b:a 10k -application voip -frame_duration 60"
)
for c in "${cases[@]}"; do
  set -- $c; name=$1; shift
  # a 440 Hz tone with a little noise, so every Opus mode has something to code
  "$FF" -v error -y -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=6" -f lavfi -i "anoisesrc=color=pink:amplitude=0.02:sample_rate=48000:duration=6" \
    -filter_complex "[0][1]amix=inputs=2:normalize=0" -c:a libopus "$@" "$T/$name.src.ogg" || { echo "FAIL encode $name"; fail=1; continue; }
  "$T/oggtest" direct "$T/$name.src.ogg" "$T/$name.out.ogg" > "$T/$name.log" 2>&1; rc=$?
  grep -E "^(ok|FAIL)" "$T/$name.log" | sed "s/^/  [$name] /"; [ $rc -eq 0 ] || fail=1
  decode_ok "$T/$name.out.ogg" || continue
  pcm "$T/$name.src.ogg" > "$T/$name.src.pcm"; pcm "$T/$name.out.ogg" > "$T/$name.out.pcm"
  # ffmpeg's file ends with a trimmed granule (the encoder's padding); ours keeps every sample: compare the common part
  python3 - "$T/$name.src.pcm" "$T/$name.out.pcm" "$(sed -n 's/^samples //p' "$T/$name.log")" "$("$FP" -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$T/$name.src.ogg")" "$name" "$T/$name.src.ogg" <<'EOF' || fail=1
import sys, struct, array
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read(); total=int(sys.argv[3]); ch=int(sys.argv[4]); name=sys.argv[5]
src=open(sys.argv[6],'rb').read(); pre=struct.unpack_from('<H',src,src.find(b'OpusHead')+10)[0]
want=(total-pre)*ch*2
# ffmpeg cuts its own file's last frame short (end trimming); in stereo its last few samples come out +-1 LSB from the
# untrimmed frame. Everything before the source's final 20 ms must be bit-identical.
cut=max(0,len(a)-960*ch*2); same=b[:cut]==a[:cut]
x=array.array('h'); x.frombytes(a[cut:]); y=array.array('h'); y.frombytes(b[cut:len(a)]); tail=max((abs(p-q) for p,q in zip(x,y)),default=0)
ok=len(b)==want and len(b)>=len(a) and same and tail<=1
print(("ok   " if ok else "FAIL ")+f"  [{name}] ffmpeg decodes ours to {len(b)//(2*ch)} samples x {ch} ch (want {total}-{pre} pre-skip = {total-pre}); its own file: {len(a)//(2*ch)}; "
      f"common part {'bit-identical' if same else 'DIFFERENT'} up to its trimmed last frame (max diff there {tail})")
sys.exit(0 if ok else 1)
EOF
done

echo "== 2. the 20 ms silence packet"
"$T/oggtest" silence "$T/silence.ogg" 50 >/dev/null
if decode_ok "$T/silence.ogg"; then
  pcm "$T/silence.ogg" > "$T/silence.pcm"
  python3 - "$T/silence.pcm" <<'EOF' || fail=1
import sys, array
a=array.array('h'); a.frombytes(open(sys.argv[1],'rb').read()); m=max((abs(x) for x in a),default=-1)
ok=len(a)==48000 and m<=1
print(("ok   " if ok else "FAIL ")+f"  50 silence packets -> {len(a)} samples, peak {m}")
sys.exit(0 if ok else 1)
EOF
fi

echo "== 3. archive -> viewer pipeline"
mkdir -p "$T/pipe"
"$T/oggtest" pipeline "$T/celt20.src.ogg" "$T/pipe" 2>&1 | sed 's/^/  /'; [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1
while read -r f want; do
  [ -n "$f" ] || continue
  if decode_ok "$T/pipe/$f"; then got=$(samples_of "$T/pipe/$f")
    if [ "$got" = "$want" ]; then echo "ok     $f decodes cleanly: $got samples ($(python3 -c "print(round($got/48000,3))") s)"; else echo "FAIL   $f: $got samples, want $want"; fail=1; fi
  fi
done < "$T/pipe/expect.txt"
for j in sessions.json sessions2.json index.json index2.json utt0.json; do
  if python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$T/pipe/$j" 2>/dev/null; then echo "ok     $j is valid JSON"; else echo "FAIL   $j is not valid JSON"; fail=1; fi
done
for j in events.jsonl roster.jsonl; do
  if python3 -c "import json,sys; [json.loads(l) for l in open(sys.argv[1]) if l.strip()]" "$T/pipe/$j" 2>/dev/null; then echo "ok     $j: every line is valid JSON"; else echo "FAIL   $j"; fail=1; fi
done

if [ $fail -eq 0 ]; then echo "ALL PASSED ($T)"; rm -rf "$T"; else echo "FAILURES (files kept in $T)"; fi
exit $fail
