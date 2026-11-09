#!/bin/bash
# Build the native app's hook dylib (arm64): shim/build/nhook.dylib
#   nhook.c (+ nstats.c, nnet.c, noverlay.c, ../src/h3maps.c included), stub.S, nrender.m (ARC: the Metal frame hook),
#   ../src/rec.m (the loader's clip recorder, manual retain/release, without its GL frame path: REC_NO_GL)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/native-obj
# (2026-11-09: a // comment appended mid-line once commented out the stuck alarm's reset for ~6 hours: warn about any such line)
python3 tools/comment-scan.py >&2 || echo "build.sh: WARNING -- code after a mid-line // comment (above)" >&2
O=build/native-obj; CF="-arch arm64 -mmacosx-version-min=11.0 -O2 -g -Wall"
clang $CF -fobjc-arc -c -o $O/nrender.o native/nrender.m
clang $CF -DREC_NO_GL -Wno-deprecated-declarations -c -o $O/rec.o src/rec.m
clang $CF -fno-objc-arc -c -o $O/nvpad.o native/nvpad.m   # (the walkers' virtual game controller)
clang $CF -Wno-unused-function -c -o $O/nhook.o native/nhook.c
clang $CF -c -o $O/stub.o native/stub.S
# linked to a temporary file and renamed into place: a running game keeps the dylib it mapped (a file rewritten in place under
# a running process can get it killed for an invalid code signature when its pages are read again)
clang $CF -dynamiclib -o build/nhook.dylib.new $O/nhook.o $O/stub.o $O/nrender.o $O/rec.o $O/nvpad.o \
  -framework Foundation -framework Metal -framework QuartzCore -framework CoreVideo -framework AVFoundation -framework CoreMedia -framework AudioToolbox -framework CoreAudio -framework GameController -framework AppKit -lz
mv -f build/nhook.dylib.new build/nhook.dylib
echo "built shim/build/nhook.dylib"
# the walker's BRAIN as a reloadable module (2026-11-09): the same C code with -DNH_BRAIN -- no start-up, no hooks, no interposing; the
# body's exports (nhx_*, the ObjC / stub symbols) bound at load (-undefined dynamic_lookup). A running game's body loads a newer one
# by itself within 2 s (nbot.c brain_tick): code changes reach a walker without a restart
# (2026-11-09, the user: "each training session reload you should describe what the new brain is trying to show that it learned"):
# native/brain-lesson.txt = what this brain is meant to show, compiled in (the walker logs it when it loads the brain: "[brain] lesson")
# and appended to battles/follow/brain-lessons.jsonl with the build time (follow-study.py prints the lesson in play with each window)
# EVERY BRAIN UPDATE DESCRIBES ITS LESSON (2026-11-09, the user: "every update should describe its lesson"): when the brain's code
# changed since the last brain build (a hash of native/*.c *.h, src/nav.c, src/h3maps.c) but native/brain-lesson.txt did not, the brain
# is NOT built -- the running walker keeps its brain until the lesson says what the new one is meant to show. A rebuild of unchanged
# code needs nothing. BRAIN_LESSON_SAME=1 builds anyway (logged as such in brain-lessons.jsonl). State: build/brain-state.txt
SRCH=$(cat native/*.c native/*.h src/nav.c src/h3maps.c 2>/dev/null | shasum -a 256 | cut -c1-16)
LESH=$( [ -f native/brain-lesson.txt ] && shasum -a 256 < native/brain-lesson.txt | cut -c1-16 || echo none)
read -r OSRC OLES 2>/dev/null < build/brain-state.txt || { OSRC=""; OLES=""; }
if [ ! -s native/brain-lesson.txt ]; then echo "build.sh: NO BRAIN BUILT -- native/brain-lesson.txt is empty: describe what this brain is meant to show" >&2; exit 1; fi
SAME=""
if [ -n "$OSRC" ] && [ "$SRCH" != "$OSRC" ] && [ "$LESH" = "$OLES" ]; then
  if [ "${BRAIN_LESSON_SAME:-0}" = 1 ]; then SAME=1; echo "build.sh: the brain's code changed, its lesson did not (BRAIN_LESSON_SAME=1: built anyway)" >&2
  else echo "build.sh: NO BRAIN BUILT -- the brain's code changed but native/brain-lesson.txt still describes the last brain: write this update's lesson (what it changes and what it should show), then build again (BRAIN_LESSON_SAME=1 overrides)" >&2; exit 1; fi
fi
LES=""; [ -f native/brain-lesson.txt ] && LES=$(tr '\n' ' ' < native/brain-lesson.txt | sed 's/["\\]/\\&/g')
printf '#define NH_BRAIN_LESSON "%s"\n' "$LES" > build/brain_lesson.h
clang $CF -DNH_BRAIN -include build/brain_lesson.h -Wno-unused-function -Wno-unused-variable -dynamiclib -undefined dynamic_lookup -o build/nbrain.dylib.new native/nhook.c -lz
if [ -n "$LES" ]; then BL="$HOME/Library/Application Support/ReclaimerMac/battles/follow/brain-lessons.jsonl"; mkdir -p "$(dirname "$BL")"
  python3 -c 'import json,sys,time; open(sys.argv[1],"a").write(json.dumps({"t": round(time.time()), "built": time.strftime("%H:%M:%S"), "lesson": open(sys.argv[2]).read().strip(), "same_lesson": sys.argv[3] == "1"}) + "\n")' "$BL" native/brain-lesson.txt "${SAME:-0}"; fi
mv -f build/nbrain.dylib.new build/nbrain.dylib
echo "$SRCH $LESH" > build/brain-state.txt
echo "built shim/build/nbrain.dylib"
