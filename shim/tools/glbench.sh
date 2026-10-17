#!/bin/bash
# How much of the loader's per-draw GL cost is Apple's GL driver running under Rosetta: the same GL draw loop
# (tools/glbench.c) built for x86_64 (as the loader runs) and arm64 (as a native GL helper process would).
#     bash shim/tools/glbench.sh [draws/frame] [frames] [textures/draw]
# Run it with no game running (it measures CPU time per draw; a busy GPU or CPU skews it).
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/glbench.$$"; mkdir -p "$OUT"
for a in x86_64 arm64; do
  clang -O2 -arch $a glbench.c -framework OpenGL -o "$OUT/glbench_$a" || exit 1
done
for r in 1 2; do
  arch -x86_64 "$OUT/glbench_x86_64" "$@"
  arch -arm64  "$OUT/glbench_arm64"  "$@"
done
rm -rf "$OUT"
