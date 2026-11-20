#!/usr/bin/env bash
# Builds the frame-generation layer, the probe layer and the two-device test program.
#
#   ./build.sh           layer -> build/fg/libsc_fg.new.so, test -> build/test_chain/test_chain
#   ./build.sh install   also puts the new layer in place (build/fg/libsc_fg.so); the build it
#                        replaces is kept in build/fg-prev ("sc-turbo fg N old" launches with it).
#                        Not while the game runs.
# Needs clang++, a C compiler, glslangValidator, cmake, python3, the Vulkan headers and loader;
# for the test program also the Wayland client library, wayland-scanner and wayland-protocols.
# Tests: tools/test-chain.sh (headless, never on the desktop).
set -eu
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p build/fg build/test_chain
# AMD's frame interpolation and optical flow (third_party/ffx_src): built at every build, which
# costs nothing when nothing changed, so that a changed header (the most generated frames, which
# sizes structures the layer shares with it) rebuilds what uses it. Built once, a library made for
# 3 generated frames was linked with a layer compiled for 7.
third_party/ffx_src/build.sh > build/ffx-build.log 2>&1 || { tail -30 build/ffx-build.log; exit 1; }
# The manifests go beside the libraries: the Vulkan loader is pointed at these folders.
cmp -s fg/sc_fg.json build/fg/sc_fg.json || cp fg/sc_fg.json build/fg/sc_fg.json
cmp -s probe/sc_probe.json build/sc_probe.json || cp probe/sc_probe.json build/sc_probe.json
# The passive probe layer ("sc-turbo probe"), when it is missing or its source is the newer one.
if [ ! -f build/libsc_probe.so ] || [ probe/sc_probe.c -nt build/libsc_probe.so ]; then
    cc -O2 -Wall -fPIC -fvisibility=hidden -shared -o build/libsc_probe.so.tmp probe/sc_probe.c -lpthread
    mv -f build/libsc_probe.so.tmp build/libsc_probe.so
    echo "probe: build/libsc_probe.so"
fi
# Shaders: fg/NAME.comp -> fg/NAME.spv.h (the array NAME_SPV), when the source is the newer one.
for shader in fg/*.comp; do
    header="${shader%.comp}.spv.h"
    if [ ! -f "$header" ] || [ "$shader" -nt "$header" ]; then
        name="$(basename "${shader%.comp}")"
        glslangValidator -V --vn "${name^^}_SPV" -o "$header" "$shader"
    fi
done
clang++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC \
    -fvisibility=hidden -shared -Ifg -include third_party/lichii/ffx_linux_compat.h \
    -isystem third_party/ffx/include -o build/fg/libsc_fg.new.so fg/sc_fg.cpp \
    build/ffx/ffx_fg/libffx_fg.a -Wl,--exclude-libs,ALL -Wl,-z,defs -lpthread
echo "layer: build/fg/libsc_fg.new.so"
# (also where "sc-turbo fg N next" finds it; put there in one step, in case a game runs on it)
mkdir -p build/fg-next
cp -p build/fg/sc_fg.json build/fg-next/
cp -p build/fg/libsc_fg.new.so build/fg-next/libsc_fg.so.tmp
mv -f build/fg-next/libsc_fg.so.tmp build/fg-next/libsc_fg.so

xml=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
[ -f "$xml" ] || xml=/usr/share/qt6/wayland/protocols/xdg-shell/xdg-shell.xml
if [ -f "$xml" ]; then
    wayland-scanner client-header "$xml" build/test_chain/xdg-shell-client-protocol.h
    wayland-scanner private-code "$xml" build/test_chain/xdg-shell-protocol.c
    cc -O2 -Wall -Wno-unused-parameter -Ibuild/test_chain -o build/test_chain/test_chain probe/test_chain.c \
        build/test_chain/xdg-shell-protocol.c -lvulkan -lwayland-client -lpthread
    echo "test: build/test_chain/test_chain"
else
    echo "no xdg-shell.xml: test program not built"
fi

if [ "${1:-}" = install ]; then
    # (by the process's own name: a match on command lines also finds a shell or an editor that names it)
    if pgrep -x StarCitizen.exe >/dev/null; then
        echo "the game is running: not installed"; exit 1
    fi
    if [ -f build/fg/libsc_fg.so ] && ! cmp -s build/fg/libsc_fg.so build/fg/libsc_fg.new.so; then
        mkdir -p build/fg-prev
        cp -p build/fg/libsc_fg.so build/fg/sc_fg.json build/fg-prev/
    fi
    # (put in place in one step: a program that still has the old one open, the launcher, keeps it)
    cp -p build/fg/libsc_fg.new.so build/fg/libsc_fg.so.tmp
    mv -f build/fg/libsc_fg.so.tmp build/fg/libsc_fg.so
    echo "installed: build/fg/libsc_fg.so"
fi
