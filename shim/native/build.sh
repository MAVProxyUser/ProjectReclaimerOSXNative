#!/bin/bash
# Build the native app's hook dylib (arm64): shim/build/nhook.dylib
set -e
cd "$(dirname "$0")/.."
mkdir -p build
clang -arch arm64 -mmacosx-version-min=11.0 -O2 -g -Wall -Wno-unused-function -dynamiclib -o build/nhook.dylib native/nhook.c native/stub.S
echo "built shim/build/nhook.dylib"
