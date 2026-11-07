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
clang $CF -Wno-unused-function -c -o $O/nhook.o native/nhook.c
clang $CF -c -o $O/stub.o native/stub.S
clang $CF -dynamiclib -o build/nhook.dylib $O/nhook.o $O/stub.o $O/nrender.o $O/rec.o \
  -framework Foundation -framework Metal -framework QuartzCore -framework CoreVideo -framework AVFoundation -framework CoreMedia -framework AudioToolbox
echo "built shim/build/nhook.dylib"
