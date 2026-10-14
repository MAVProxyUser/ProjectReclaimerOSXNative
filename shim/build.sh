#!/bin/bash
# Build the Wine-free PE loader. Needs the OpenGL framework for the wgl->CGL
# bridge (the engine's menu renders via OpenGL).
set -e
cd "$(dirname "$0")"
clang -arch x86_64 -O1 -framework OpenGL -framework Cocoa -framework CoreText -framework CoreGraphics -framework CoreFoundation -o build/peload src/peload.c
echo "built: build/peload  (run under: arch -x86_64 build/peload <pe> [args])"
