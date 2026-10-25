#!/bin/bash
# A picture of a game client's window WITH its title bar (fps / draws / frame time are in the title), never the desktop:
#     bash shim/tools/titleshot.sh <out.png> [pid]     (pid: default the newest shim/build/peload that owns a window)
# Uses shim/build/winid (built here from shim/tools/winid.c) and `screencapture -l<window id> -o -x`. macOS asks once for
# Screen Recording permission for the app running this; without it the picture has no window content. (shim/tools/winshot.sh
# = the window's content only, from the stats server's clip recorder, no permission needed.)
set -u
cd "$(dirname "$0")/../.."
OUT="${1:?usage: titleshot.sh <out.png> [pid]}"; PID="${2:-}"
{ [ -x shim/build/winid ] && [ shim/build/winid -nt shim/tools/winid.c ]; } || clang -O2 -framework CoreGraphics -framework CoreFoundation -o shim/build/winid shim/tools/winid.c || exit 1
if [ -z "$PID" ]; then
  for p in $(pgrep -f '^[^ ]*build/peload' | sort -rn); do shim/build/winid "$p" > /dev/null 2>&1 && { PID=$p; break; }; done
fi
[ -n "$PID" ] || { echo "titleshot: no peload process with a window" >&2; exit 1; }
ID=$(shim/build/winid "$PID" | head -1 | cut -d' ' -f1)
[ -n "$ID" ] || { echo "titleshot: pid $PID has no on-screen window" >&2; exit 1; }
screencapture -l"$ID" -o -x "$OUT" && echo "$OUT (pid $PID, window $ID: $(shim/build/winid "$PID" | head -1 | cut -d' ' -f2-))"
