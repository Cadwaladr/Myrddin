#!/usr/bin/env bash
# Builds Mesa's Vulkan layer VK_LAYER_MESA_vram_report_limit into build/vram-limit, for systems whose
# distribution does not package it (Fedora 44 does not; Arch has it in vulkan-mesa-layers). The layer
# tells a program that the graphics card has less memory than it has: the LUG wiki's remedy for frame
# drops when an NVIDIA card's memory runs out, which sc-turbo places at every launch (SC_VRAM_LIMIT).
#
#   tools/build-vram-layer.sh [--clean] [--test]
#
#   --clean   throws away the container and the build folder first
#   --test    only tests the layer that is in build/vram-limit (see the end of this file)
#
# What it uses, all pinned:
#   Mesa's source, release MESA below, from archive.mesa3d.org, checked against the SHA256 of Mesa's
#   own release notes (docs.mesa3d.org/relnotes/<version>.html). Nothing of Mesa but this layer and the
#   small helper libraries it links (src/util, src/vulkan/util) is built: no driver, no OpenGL. One
#   line of Mesa's build files is changed for that (see below); the layer's code is Mesa's, unchanged.
#   An Ubuntu 22.04 container (podman, rootless), so that the layer loads on any x86-64 Linux with
#   glibc 2.35 or newer, as the project's own layer does. meson, Mako and PyYAML come from PyPI at the
#   versions below.
# The source and the build stay in appimage/work/vram-layer (about 600 MB, left out of git).
# The layer's notice (MIT) goes beside it as LICENSE, Mesa's own account of its licences as
# MESA-LICENSE.rst.
#
# It does nothing while the game or the RSI Launcher runs, and stops if one of them starts: a build
# takes the memory bandwidth the game needs.
set -euo pipefail
MESA=26.2.7
MESA_SHA256=04294f75d22e9f61cd1e050206bc48b3d39366e788f1803a3368e3d5a872ce18
MESON=1.7.2; MAKO=1.3.10; PYYAML=6.0.2
image=docker.io/library/ubuntu:22.04
mirror=mirrors.edge.kernel.org/ubuntu/
name=myrddin-vram-build
provision_version=1
jobs="${JOBS:-6}"
podman="${PODMAN:-podman}"

here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
work="$here/appimage/work/vram-layer"
out="$here/build/vram-limit"
clean=0; test_only=0
for a in "$@"; do
    case "$a" in
    --clean) clean=1 ;;
    --test) test_only=1 ;;
    *) awk 'NR > 1 { if (/^#/) print; else exit }' "$0"; exit 1 ;;
    esac
done

game_runs() { pgrep -x 'StarCitizen.exe' > /dev/null || pgrep -f '[R]SI Launcher\.exe' > /dev/null; }

# --- the test: what a program is told with the layer and without it (no window, nothing drawn) ---
test_layer() {
    [ -f "$out/VkLayer_MESA_vram_report_limit.json" ] || { echo "no layer in $out"; return 1; }
    command -v vulkaninfo > /dev/null || { echo "vulkaninfo is not installed: not tested"; return 0; }
    local line total dev id want=4096 plain limited
    line="$(nvidia-smi --query-gpu=memory.total,pci.device_id --format=csv,noheader,nounits 2>/dev/null | head -1)"
    total="${line%%,*}"; dev="$(printf '%s' "${line##*, }" | tr 'A-F' 'a-f')"
    [ "${#dev}" = 10 ] || { echo "no NVIDIA card to ask: not tested"; return 0; }
    id="0x${dev:6:4}:0x${dev:2:4}"
    # every memory heap size reported, in MiB, one per line
    heaps() { grep -A2 'memoryHeaps\[' | grep -o 'size *= *[0-9]*' | grep -o '[0-9]*$' | awk '{ printf "%d\n", $1 / 1048576 }'; }
    plain="$(vulkaninfo 2>/dev/null | heaps | tr '\n' ' ')"
    limited="$(VK_ADD_LAYER_PATH="$out" VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_vram_report_limit \
        VK_VRAM_REPORT_LIMIT_HEAP_SIZE=$want VK_VRAM_REPORT_LIMIT_DEVICE_ID="$id" vulkaninfo 2>/dev/null | heaps | tr '\n' ' ')"
    echo "card $id, $total MiB. Heap sizes reported (all cards, MiB): without the layer: $plain"
    echo "  with a limit of $want for that card: $limited"
    case " $plain" in *" $want "*) echo "TEST VOID: a heap of $want MiB is there without the layer"; return 1 ;; esac
    case " $limited" in *" $want "*) ;; *) echo "TEST FAILED: the layer did not limit what is reported"; return 1 ;; esac
    echo "test passed"
}
if [ "$test_only" = 1 ]; then test_layer; exit; fi

if game_runs; then echo "the game or the RSI Launcher runs: not building now"; exit 3; fi
mkdir -p "$work"

# --- Mesa's source ---
tarball="$work/mesa-$MESA.tar.xz"
if [ ! -s "$tarball" ]; then
    echo "fetching mesa-$MESA.tar.xz"
    nice -n 19 curl -fsSL -A curl --max-time 600 -o "$tarball.part" "https://archive.mesa3d.org/mesa-$MESA.tar.xz"
    mv "$tarball.part" "$tarball"
fi
echo "$MESA_SHA256  $tarball" | sha256sum -c --quiet - || { echo "mesa-$MESA.tar.xz does not have the pinned checksum"; exit 1; }

# --- the container ---
if [ "$clean" = 1 ]; then
    "$podman" rm -f "$name" > /dev/null 2>&1 || true
    rm -rf "$work/build" "$work/src"
fi
if ! "$podman" container exists "$name"; then
    "$podman" image exists "$image" || nice -n 19 "$podman" pull -q "$image" > /dev/null
    "$podman" run -d --name "$name" --security-opt label=disable -v "$work:/work" -w /work "$image" sleep infinity > /dev/null
    echo "container $name made ($image)"
elif [ "$("$podman" inspect --format '{{.State.Running}}' "$name")" != true ]; then
    "$podman" start "$name" > /dev/null
fi

# Stops the build the moment the game starts.
( while sleep 5; do
      kill -0 $$ 2> /dev/null || exit 0
      if game_runs; then echo "the game started: build stopped"; "$podman" stop -t 1 "$name" > /dev/null 2>&1; exit 0; fi
  done ) &
watcher=$!
trap 'kill $watcher 2> /dev/null || true' EXIT

"$podman" exec -i -e JOBS="$jobs" -e PROVISION="$provision_version" -e MIRROR="$mirror" -e MESA="$MESA" \
    -e MESON="$MESON" -e MAKO="$MAKO" -e PYYAML="$PYYAML" "$name" bash -s <<'INSIDE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
nice="nice -n 19"
if [ "$(cat /opt/provisioned 2>/dev/null)" != "$PROVISION" ]; then
    # (Ubuntu's own servers were unreachable from the build machine, and kernel.org's mirror at times
    # only over http. apt checks the signed Release files and every package against them, so plain
    # http is safe for it. The CA certificates are for pip.)
    sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?#http://$MIRROR#; s#https://$MIRROR#http://$MIRROR#" /etc/apt/sources.list
    apt="apt-get -qq -o Acquire::Retries=3"
    $nice $apt update
    $nice $apt install -y --no-install-recommends ca-certificates gcc g++ libc6-dev binutils pkg-config ninja-build \
        python3 python3-pip xz-utils bison flex file > /dev/null
    $nice pip3 install -q --no-cache-dir "meson==$MESON" "Mako==$MAKO" "PyYAML==$PYYAML" packaging
    echo "$PROVISION" > /opt/provisioned
    echo "container provisioned: $(gcc --version | head -1), meson $(meson --version)"
fi
src=/work/src/mesa-$MESA
if [ ! -f "$src/meson.build" ] || [ ! -d "$src/src/util" ]; then
    rm -rf /work/src; mkdir -p /work/src
    $nice tar -xJf /work/mesa-$MESA.tar.xz -C /work/src
fi
# Mesa builds its layers only together with a Vulkan driver (src/meson.build enters src/vulkan only
# then). One condition more lets this layer be built alone; nothing of the layer's own code is changed.
if ! grep -q 'with_gallium_zink or with_vulkan_vram_report_limit_layer' "$src/src/meson.build"; then
    sed -i 's/^if with_any_vk or with_gallium_zink$/if with_any_vk or with_gallium_zink or with_vulkan_vram_report_limit_layer/' "$src/src/meson.build"
    grep -q 'with_gallium_zink or with_vulkan_vram_report_limit_layer' "$src/src/meson.build" ||
        { echo "src/meson.build of Mesa $MESA is not as expected: the layer cannot be built alone"; exit 1; }
    rm -rf /work/build
fi
b=/work/build
if [ ! -f $b/build.ninja ]; then
    # Only the layer: no drivers, no OpenGL, no window systems, and nothing optional that would make
    # the layer need a library a system may lack.
    $nice meson setup $b "$src" --buildtype=release -Db_ndebug=true -Dstrip=false \
        -Dvulkan-layers=vram-report-limit -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= \
        -Dopengl=false -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
        -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=disabled \
        -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dlmsensors=disabled \
        -Dvideo-codecs= -Dbuild-tests=false > $b.setup.log 2>&1 || { tail -30 $b.setup.log; exit 1; }
fi
target=src/vulkan/vram-report-limit-layer/libVkLayer_MESA_vram_report_limit.so
$nice ninja -C $b -j "$JOBS" $target > $b.ninja.log 2>&1 || { tail -40 $b.ninja.log; exit 1; }
rm -rf /work/out; mkdir -p /work/out
cp $b/$target /work/out/
strip --strip-unneeded /work/out/libVkLayer_MESA_vram_report_limit.so
sed 's|@library_path@|./libVkLayer_MESA_vram_report_limit.so|' \
    "$src/src/vulkan/vram-report-limit-layer/VkLayer_MESA_vram_report_limit.json.in" > /work/out/VkLayer_MESA_vram_report_limit.json
layer_c="$src/src/vulkan/vram-report-limit-layer/vram_report_limit_layer.c"
{
    echo "VK_LAYER_MESA_vram_report_limit, from Mesa $MESA (src/vulkan/vram-report-limit-layer), unchanged."
    echo "The notice of its source file:"
    echo
    awk '/^\/\*/ { on = 1; next } on && /\*\// { exit } on { sub(/^ \* ?/, ""); print }' "$layer_c"
    echo
    echo "The library also contains parts of Mesa's src/util and src/vulkan/util, which are under the same"
    echo "licence with notices of their own; Mesa's licences as a whole are in MESA-LICENSE.rst."
} > /work/out/LICENSE
grep -q 'Permission is hereby granted, free of charge' /work/out/LICENSE || { echo "the layer's notice was not found in its source"; exit 1; }
cp "$src/docs/license.rst" /work/out/MESA-LICENSE.rst
echo "Mesa $MESA, src/vulkan/vram-report-limit-layer, built in ubuntu:22.04 with $(gcc --version | head -1 | sed 's/.*) //')" > /work/out/VERSION
echo "needs: $(readelf -d /work/out/libVkLayer_MESA_vram_report_limit.so | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
echo "newest glibc symbol: $(objdump -T /work/out/libVkLayer_MESA_vram_report_limit.so | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)"
INSIDE

mkdir -p "$out"
cp -p "$work/out/libVkLayer_MESA_vram_report_limit.so" "$work/out/VkLayer_MESA_vram_report_limit.json" \
      "$work/out/LICENSE" "$work/out/MESA-LICENSE.rst" "$work/out/VERSION" "$out/"
echo "layer in $out: $(stat -c %s "$out/libVkLayer_MESA_vram_report_limit.so") bytes, sha256 $(sha256sum < "$out/libVkLayer_MESA_vram_report_limit.so" | cut -c1-16)"
test_layer
