#!/bin/bash
# The native GL helper experiment (tools/glring.c): the glbench draw stream encoded by an x86_64 producer (Rosetta,
# like the loader) into a shared-memory ring and replayed by a native arm64 consumer that owns the GL context; then
# glbench in-process (x86_64 and arm64) on the same stream for comparison.
#     bash shim/tools/glring.sh [draws/frame] [frames] [textures/draw] [constant buffers/draw]
# Run it with no game running (CPU time per draw; a busy CPU or GPU skews it).
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/glring.$$"; mkdir -p "$OUT"
clang -O2 -arch x86_64 glring.c -framework OpenGL -o "$OUT/glring_x86_64" || exit 1
clang -O2 -arch arm64  glring.c -framework OpenGL -o "$OUT/glring_arm64"  || exit 1
for a in x86_64 arm64; do clang -O2 -arch $a glbench.c -framework OpenGL -o "$OUT/glbench_$a" || exit 1; done
NAME="/glring.$$"
for r in 1 2; do
  echo "-- run $r"
  arch -arm64 "$OUT/glring_arm64" consume "$NAME" & C=$!
  sleep 1
  arch -x86_64 "$OUT/glring_x86_64" produce "$NAME" "$@"
  wait $C
  echo "in-process (glbench, same stream):"; arch -x86_64 "$OUT/glbench_x86_64" "$@" | sed 's/^/  /'; arch -arm64 "$OUT/glbench_arm64" "$@" | sed 's/^/  /'
done
rm -rf "$OUT"
