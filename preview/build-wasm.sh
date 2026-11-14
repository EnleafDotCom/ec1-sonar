#!/bin/bash
# Build the browser simulator for a tree: build-wasm.sh <tree> <out.wasm>
set -e
ROOT=$(cd "$1" && pwd); OUT=$2
W=/tmp/claude-0/wasi-sdk-27.0-x86_64-linux
cd "$ROOT"
SRCS=$( (find app web third_party/synth -name '*.c' -not -path 'app/board/*') | sort)
$W/bin/clang --sysroot=$W/share/wasi-sysroot -std=c11 -O2 -fwrapv -fno-fast-math -ffp-contract=off \
  -DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror -Wno-unused-function -Wno-array-parameter -mexec-model=reactor \
  -Iweb/engine -Iweb/synth -Ithird_party/synth -Isdk/include -Iapp/synth -Iapp $SRCS \
  -Wl,--strip-all -Wl,-z,stack-size=131072 -Wl,--initial-memory=4194304 -Wl,--max-memory=33554432 -o "$OUT"
echo "built $OUT $(stat -c %s "$OUT") bytes"
