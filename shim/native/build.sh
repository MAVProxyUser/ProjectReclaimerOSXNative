#!/bin/bash
# Build the native app's hook dylib (arm64): shim/build/nhook.dylib
#   nhook.c (+ nstats.c, nnet.c, noverlay.c, ../src/h3maps.c included), stub.S, nrender.m (ARC: the Metal frame hook),
#   ../src/rec.m (the loader's clip recorder, manual retain/release, without its GL frame path: REC_NO_GL)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/native-obj
O=build/native-obj; CF="-arch arm64 -mmacosx-version-min=11.0 -O2 -g -Wall"
clang $CF -fobjc-arc -c -o $O/nrender.o native/nrender.m
clang $CF -DREC_NO_GL -Wno-deprecated-declarations -c -o $O/rec.o src/rec.m
clang $CF -fno-objc-arc -c -o $O/nvpad.o native/nvpad.m   # (the walkers' virtual game controller)
clang $CF -Wno-unused-function -c -o $O/nhook.o native/nhook.c
clang $CF -c -o $O/stub.o native/stub.S
# linked to a temporary file and renamed into place: a running game keeps the dylib it mapped (a file rewritten in place under
# a running process can get it killed for an invalid code signature when its pages are read again)
clang $CF -dynamiclib -o build/nhook.dylib.new $O/nhook.o $O/stub.o $O/nrender.o $O/rec.o $O/nvpad.o \
  -framework Foundation -framework Metal -framework QuartzCore -framework CoreVideo -framework AVFoundation -framework CoreMedia -framework AudioToolbox -framework CoreAudio -framework GameController -lz
mv -f build/nhook.dylib.new build/nhook.dylib
echo "built shim/build/nhook.dylib"
