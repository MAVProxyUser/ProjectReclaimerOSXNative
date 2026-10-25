#!/bin/bash
# Rosetta ahead-of-time vs run-time translation of the same x86-64 code (see aotbench.c). Builds into a temp dir:
#   aotbench          the benchmark (x86_64)
#   benchcode.dylib   the bytes of aotbench's __bench section in a dylib's text, exported as bench_code (.incbin)
# and runs it under Rosetta. Also prints whether the dylib loaded at its requested address (-image_base, like halo3.dll's
# 0x180000000) or was slid.
set -eu
cd "$(dirname "$0")"
T=$(mktemp -d -t aotbench); trap 'rm -rf "$T"' EXIT
clang -arch x86_64 -O2 -fno-stack-protector -o "$T/aotbench" aotbench.c
# the __bench section's bytes: a dylib whose text holds exactly those bytes at the symbol bench_code
otool -arch x86_64 -s __TEXT __bench -X "$T/aotbench" | awk '{for(i=2;i<=NF;i++) printf "%s", $i}' | xxd -r -p > "$T/bench.bin"
printf '.section __TEXT,__text,regular,pure_instructions\n.globl _bench_code\n.p2align 4\n_bench_code:\n.incbin "%s"\n' "$T/bench.bin" > "$T/code.s"
clang -arch x86_64 -dynamiclib -o "$T/benchcode.dylib" "$T/code.s" -Wl,-image_base,0x180000000 -Wl,-no_fixup_chains   # (with chained fixups ld ignores -image_base)
echo "dylib requested at 0x180000000 (the address halo3.dll asks for)"
arch -x86_64 "$T/aotbench" "$T/benchcode.dylib"
