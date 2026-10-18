#!/bin/bash
# Offline test of the clip recorder (shim/src/rec.m; shim/docs/recording.md) -- no game needed, nothing is written to
# your own ~/Movies: tools/rec-test.m (an offscreen GL animation as the window, two tones as the audio callbacks) is
# built with rec.m for x86_64 and run under Rosetta like the loader, into a scratch HOME; ffprobe / ffmpeg check what
# came out: codecs, size, length, frame count, picture orientation, both tones in the sound, the folder list, the
# audio-only file, a recording that never got a picture, an early stop, and a recorder killed mid-clip.
#     bash shim/tools/rec-test.sh            (KEEP=1 keeps the scratch folder; FFPROBE= / FFMPEG= other binaries)
set -u
cd "$(dirname "$0")"
FFPROBE=${FFPROBE:-/opt/homebrew/bin/ffprobe}; FFMPEG=${FFMPEG:-/opt/homebrew/bin/ffmpeg}
[ -x "$FFPROBE" ] && [ -x "$FFMPEG" ] || { echo "needs ffprobe + ffmpeg (brew install ffmpeg)"; exit 2; }
OUT=$(mktemp -d "${TMPDIR:-/tmp}/rectest.XXXXXX"); DIR="$OUT/home/Movies/Reclaimer"; mkdir -p "$OUT/home"
cleanup(){ [ "${KEEP:-0}" = 1 ] && echo "kept $OUT" || rm -rf "$OUT"; }; trap cleanup EXIT
clang -arch x86_64 -O1 -o "$OUT/rec-test" rec-test.m ../src/rec.m -framework OpenGL -framework Foundation \
  -framework AVFoundation -framework CoreMedia -framework CoreVideo || exit 1
FAIL=0
ok(){ echo "  ok    $1"; }
bad(){ echo "  FAIL  $1"; FAIL=1; }
check(){ if [ "$2" = 1 ]; then ok "$1"; else bad "$1"; fi; }
LOG=""
run(){ LOG="$OUT/run-$1.log"; REC_TEST_HOME="$OUT/home" PELOAD_REC_DEBUG=1 arch -x86_64 "$OUT/rec-test" "$@" > "$LOG" 2>&1; local r=$?
    NEWEST=$(ls -t "$DIR" 2>/dev/null | head -1); [ $r = 0 ] || { echo "     recorder log:"; sed 's/^/       /' "$LOG"; }; return $r; }
probe(){ "$FFPROBE" -v error -show_entries "$1" -of default=nw=1:nk=1 "${@:2}"; }
near(){ awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}' && echo 1 || echo 0; }
level(){ "$FFMPEG" -v info -t "${3:-60}" -i "$1" -map 0:a -af "${2}volumedetect" -f null - 2>&1 | awk '/mean_volume/{print $5}'; }   # file, filters, seconds

echo "== picture + sound, 3 s, a 2560x1440 window (recorded at 1920x1080)"
run av 3 2560 1440; check "recorder finished without an error" $([ $? = 0 ] && echo 1 || echo 0)
F="$DIR/$NEWEST"; echo "     $NEWEST: $(grep -h 'saved' "$LOG" | sed 's/.*: //')"
check "video stream h264 1920x1080" $([ "$(probe stream=codec_name,width,height -select_streams v:0 "$F" | tr '\n' ' ')" = "h264 1920 1080 " ] && echo 1 || echo 0)
check "audio stream aac 48000 Hz stereo" $([ "$(probe stream=codec_name,sample_rate,channels -select_streams a:0 "$F" | tr '\n' ' ')" = "aac 48000 2 " ] && echo 1 || echo 0)
check "length 3.0 s (+-0.15)" $(near "$(probe format=duration "$F")" 3.0 0.15)
NF=$(probe stream=nb_frames -select_streams v:0 "$F"); check "frames at 60 fps: $NF (>= 170)" $([ "${NF:-0}" -ge 170 ] && echo 1 || echo 0)
PX=$("$FFMPEG" -v error -ss 1 -i "$F" -frames:v 1 -vf scale=2:2:flags=area -f rawvideo -pix_fmt rgb24 - | od -An -tu1 | tr -s ' \n' ' ')
check "picture upright: top red, bottom blue ($PX)" $(echo "$PX" | awk '{exit !($1>150 && $3<90 && $7<90 && $9>150)}' && echo 1 || echo 0)
L440=$(level "$F" "bandpass=f=440:width_type=h:w=30,"); L660=$(level "$F" "bandpass=f=660:width_type=h:w=30,"); L3K=$(level "$F" "bandpass=f=3000:width_type=h:w=200," 1.4)   # (before the 2 kHz sync mark at 1.5 s)
check "440 Hz tone (48 kHz float stereo source): $L440 dB" $(awk -v x="$L440" 'BEGIN{exit !(x>-22)}' && echo 1 || echo 0)
check "660 Hz tone (44.1 kHz int16 mono source, resampled): $L660 dB" $(awk -v x="$L660" 'BEGIN{exit !(x>-26)}' && echo 1 || echo 0)
check "nothing at 3 kHz in the first 1.4 s (no clicks or aliasing): $L3K dB" $(awk -v x="$L3K" 'BEGIN{exit !(x<-45)}' && echo 1 || echo 0)
VS=$("$FFPROBE" -v error -f lavfi -i "movie=$F,signalstats" -show_entries frame=pts_time:frame_tags=lavfi.signalstats.YAVG -of csv=p=0 | awk -F, '$2>200{print $1; exit}')
AS=$("$FFMPEG" -v info -i "$F" -map 0:a -af "bandpass=f=2000:width_type=h:w=200,silencedetect=n=-35dB:d=0.05" -f null - 2>&1 | awk '/silence_end/{print $5; exit}')
check "picture and sound in sync: white frame at ${VS:-none} s, 2 kHz burst at ${AS:-none} s (+-0.035)" $(near "${VS:-0}" "${AS:-9}" 0.035)
LD=$(sed -n 's/^LIST .*"file":"'"$NEWEST"'","size":[0-9]*,"duration":\([0-9.]*\).*/\1/p' "$LOG")
check "folder list has it, length from the file's boxes: ${LD:-none} s" $(near "${LD:-0}" 3.0 0.15)

echo "== picture + sound, 12 s (the 4 s sound ring wraps; the pixel buffer pool must keep recycling)"
run av 12 1280 720; check "recorder finished without an error" $([ $? = 0 ] && echo 1 || echo 0)
F="$DIR/$NEWEST"; echo "     $NEWEST: $(grep -h 'saved' "$LOG" | sed 's/.*: //')"
check "1280x720, length 12.0 s (+-0.15)" $([ "$(probe stream=width,height -select_streams v:0 "$F" | tr '\n' ' ')" = "1280 720 " ] && near "$(probe format=duration "$F")" 12.0 0.15 | grep -q 1 && echo 1 || echo 0)
NF=$(probe stream=nb_frames -select_streams v:0 "$F"); check "frames at 60 fps: $NF (>= 690)" $([ "${NF:-0}" -ge 690 ] && echo 1 || echo 0)
L440=$("$FFMPEG" -v info -ss 3 -i "$F" -map 0:a -af "bandpass=f=440:width_type=h:w=30,volumedetect" -f null - 2>&1 | awk '/mean_volume/{print $5}')
L3K=$("$FFMPEG" -v info -ss 3 -i "$F" -map 0:a -af "bandpass=f=3000:width_type=h:w=200,volumedetect" -f null - 2>&1 | awk '/mean_volume/{print $5}')
check "after the wraps: 440 Hz $L440 dB, 3 kHz $L3K dB (steady tone, no clicks)" $(awk -v a="$L440" -v b="$L3K" 'BEGIN{exit !(a>-22 && b<-45)}' && echo 1 || echo 0)

echo "== sound only, 2 s"
run audio 2; check "recorder finished without an error" $([ $? = 0 ] && echo 1 || echo 0)
F="$DIR/$NEWEST"; echo "     $NEWEST"
check "an .m4a" $([ "${NEWEST##*.}" = m4a ] && echo 1 || echo 0)
check "only an aac stream" $([ "$(probe stream=codec_type,codec_name "$F" | tr '\n' ' ')" = "aac audio " ] && echo 1 || echo 0)
check "length 2.0 s (+-0.15)" $(near "$(probe format=duration "$F")" 2.0 0.15)

echo "== picture + sound, but no picture ever arrives (the window hidden), 1.5 s"
run noframes 1.5; check "recorder finished without an error" $([ $? = 0 ] && echo 1 || echo 0)
F="$DIR/$NEWEST"
check "valid mp4 with one black frame and the sound" $([ "$(probe stream=codec_name "$F" | tr '\n' ' ')" = "h264 aac " ] && echo 1 || echo 0)
check "length 1.5 s (+-0.15)" $(near "$(probe format=duration "$F")" 1.5 0.15)

echo "== a 30 s recording stopped after 1.2 s"
run stop 30; check "recorder finished without an error" $([ $? = 0 ] && echo 1 || echo 0)
F="$DIR/$NEWEST"
check "length about 1.2 s: $(probe format=duration "$F")" $(near "$(probe format=duration "$F")" 1.2 0.2)

echo "== the recorder killed 4.5 s into a 10 s clip (a game crash)"
REC_TEST_HOME="$OUT/home" arch -x86_64 "$OUT/rec-test" av 10 1280 720 > "$OUT/kill.log" 2>&1 & P=$!
sleep 4.5; kill -9 $P 2>/dev/null; wait $P 2>/dev/null
F="$DIR/$(ls -t "$DIR" | head -1)"; D=$(probe format=duration "$F" 2>/dev/null)
check "the partial file plays up to its last 2 s fragment: ${D:-unreadable} s" $(awk -v x="${D:-0}" 'BEGIN{exit !(x>=1.9)}' && echo 1 || echo 0)

[ $FAIL = 0 ] && echo "all recorder checks passed" || echo "SOME CHECKS FAILED"
exit $FAIL
