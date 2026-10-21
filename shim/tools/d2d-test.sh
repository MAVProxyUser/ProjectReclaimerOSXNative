#!/bin/bash
# Offline test of the loader's Direct2D / DirectWrite / WIC (shim/src/d2d.c, dwrite.c, wic.c): the loader calls them through their
# COM vtables the way a game does and checks the pixels (PELOAD_D2D_SELFTEST). No game, no window, no input.
#   1 a CPU target bitmap: shapes, strokes with dashes, gradients, a path with an arc, a clip, an opacity layer, text from the system
#     collection, a wrapped centred layout + its metrics, an in-memory font collection, a WIC-decoded PNG
#   2 + 3 the texture path (GL offscreen; in-process, then through the native GL helper): GL clear, canvas composites, a texture drawn
#     as a GPU quad under canvas content, the present latch
#     bash shim/tools/d2d-test.sh          # pictures: /tmp/d2d_test.bmp and /tmp/d2d_test_gpu.bmp
cd "$(dirname "$0")/../.."
[ -x shim/build/peload ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
OUT="${D2D_TEST_OUT:-/tmp/d2d_test.bmp}"; fail=0
run(){ local name="$1"; shift; echo "== $name"; env "$@" PELOAD_D2D_SELFTEST="$OUT" arch -x86_64 shim/build/peload 2>/dev/null | grep -E '^(PASS|FAIL|ALL|FAILED|texture|picture|\(texture)'
  local rc=${PIPESTATUS[0]}; [ "$rc" = 0 ] || fail=1; }
run "CPU target"
run "texture target, in-process GL" PELOAD_D3DGL=1 PELOAD_D3DGL_FORCE_OFFSCREEN=1 PELOAD_GL_HELPER=0
run "texture target, GL helper" PELOAD_D3DGL=1 PELOAD_D3DGL_FORCE_OFFSCREEN=1 PELOAD_GL_HELPER=1
echo "----"; [ $fail = 0 ] && echo "d2d-test: PASS" || echo "d2d-test: FAIL"; exit $fail
