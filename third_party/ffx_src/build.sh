#!/usr/bin/env bash
# Configures and builds libffx_fg.a from third_party/ffx_src into <project>/build/ffx (out of
# tree) and prints the path of the library. Needs cmake, ninja or make, a C++20 compiler (clang++
# preferred, as the prebuilt third_party/ffx/libffx_fg.a was built with it), glslangValidator,
# python3 and the Vulkan headers and loader. It does not touch third_party/ffx.
#
# usage: build.sh [BUILD_DIR]      (CXX=... overrides the compiler)
set -euo pipefail

SRC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${1:-$SRC_DIR/../../build/ffx}
mkdir -p "$BUILD_DIR"
BUILD_DIR=$(cd "$BUILD_DIR" && pwd)

if [ -z "${CXX:-}" ]; then
    if command -v clang++ >/dev/null; then CXX=clang++; else CXX=c++; fi
fi
if command -v ninja >/dev/null; then GENERATOR=Ninja; else GENERATOR="Unix Makefiles"; fi

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER="$CXX" >&2
cmake --build "$BUILD_DIR" --target ffx_fg --parallel >&2

echo "$BUILD_DIR/ffx_fg/libffx_fg.a"
