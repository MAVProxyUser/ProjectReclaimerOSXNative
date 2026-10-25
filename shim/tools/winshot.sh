#!/bin/bash
# One frame of a running client's WINDOW (menus, dialogs, the in-game UI) via the stats server's clip recorder: a 2 s clip,
# fetched over HTTP, one frame out of it. Never a desktop screenshot; works for input-less test clients too.
#   bash shim/tools/winshot.sh <out.png> [stats port, default 8765] [seconds into the clip, default 1.0]
# Needs ffmpeg. The clip stays in the game's recordings folder (PELOAD_REC_DIR if the game was started with one -- test
# runs use a scratch folder -- else ~/Movies/Reclaimer; the stats page's Recordings list can delete it).
OUT="${1:?usage: winshot.sh <out.png> [port] [seek]}"; PORT="${2:-8765}"; SEEK="${3:-1.0}"
URL="http://127.0.0.1:$PORT"; T0=$(date +%s)
R=$(curl -s -m 5 -X POST -H 'X-Reclaimer-Control: 1' "$URL/control?do=rec&secs=2&mode=av")
case "$R" in *'"ok":true'*) ;; *) echo "no stats server answering on $PORT, or recording refused: $R"; exit 1;; esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/winshot.XXXXXX"); trap 'rm -rf "$TMP"' EXIT
for i in $(seq 1 40); do   # a clip is listed (newest first) once it is finished
  F=$(curl -s -m 5 "$URL/recordings.json" | python3 -c 'import json,sys
r=[x for x in json.load(sys.stdin) if not x.get("audio_only") and x.get("time",0)>=float(sys.argv[1])-1]
print(r[0]["file"] if r else "")' "$T0" 2>/dev/null)
  if [ -n "$F" ] && curl -s -m 20 -o "$TMP/c.mp4" "$URL/recordings/$F"; then
    # a window that barely redraws (an idle menu) gives a clip of a frame or two: ffmpeg then writes nothing for a seek past
    # the last frame and still exits 0 -- so check the file, and fall back to the first frame
    rm -f "$OUT"; ffmpeg -loglevel quiet -y -ss "$SEEK" -i "$TMP/c.mp4" -frames:v 1 "$OUT" 2>/dev/null
    [ -s "$OUT" ] || ffmpeg -loglevel quiet -y -i "$TMP/c.mp4" -frames:v 1 "$OUT" 2>/dev/null
    [ -s "$OUT" ] && { echo "$OUT"; exit 0; }
  fi
  sleep 0.5
done
echo "no frame (the clip never finished)"; exit 1
