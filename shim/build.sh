#!/bin/bash
# Build the Wine-free PE loader. Needs the OpenGL framework for the wgl->CGL
# bridge (the engine's menu renders via OpenGL).
set -e
cd "$(dirname "$0")"
# MACOS_MIN: the oldest macOS the binaries start on (13 = Ventura; newer APIs are called only where available)
MIN="-mmacosx-version-min=${MACOS_MIN:-13.0}"
# build to a temp file and rename into place: a running game keeps its (old) binary intact
# the native GL helper's encoders/decoders are generated from gl3.h (committed; regenerated when python3 is around)
command -v python3 >/dev/null && python3 tools/glh_gen.py >/dev/null
# src/rec.m: clip recording (stats page Record; AVFoundation, which Cocoa loads at startup anyway, so linking it costs nothing)
clang -arch x86_64 $MIN -O1 -framework OpenGL -framework Cocoa -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -framework IOSurface -framework ImageIO \
  -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework WebKit -lcurl -o build/peload.new src/peload.c src/rec.m src/webview2.m
mv -f build/peload.new build/peload
# the native GL helper (PELOAD_GL_HELPER=1): a native arm64 process that runs the render context's GL calls
clang -arch arm64 $MIN -O2 -Wno-deprecated-declarations -framework OpenGL -framework IOSurface -framework CoreFoundation -o build/glhelper.new src/glhelper.c && mv -f build/glhelper.new build/glhelper
# Reclaimer.app's program (shim/tools/make-app.sh): a tiny native launcher; built here so Macs without a compiler get it
clang -arch arm64 -arch x86_64 $MIN -O2 -o build/reclaimer-launcher.new src/launcher.c && mv -f build/reclaimer-launcher.new build/reclaimer-launcher
echo "built: build/peload  (run under: arch -x86_64 build/peload <pe> [args])"
# Reclaimer.app next to shim/ (double-click to play; tools/make-app.sh --applications also puts it in ~/Applications)
[ -n "${NO_APP:-}" ] || echo "built: $(bash tools/make-app.sh | sed 's#^made .*/Reclaimer.app#Reclaimer.app#')"
