#!/usr/bin/env bash
# Builds the project's two Vulkan layers (fg/sc_fg.cpp -> libsc_fg.so with AMD's FidelityFX
# library, probe/sc_probe.c -> libsc_probe.so) so that they load on any x86-64 Linux with glibc
# 2.35 or newer and need no libstdc++ of the host: in a podman container with Ubuntu 22.04
# (glibc 2.35), libstdc++ linked in statically, only the Vulkan entry point exported. The
# unwinder is NOT linked in (no -static-libgcc for the C++ layer): it is the system's
# libgcc_s.so.1, which glibc itself loads whenever a thread ends (see below). The same compiler commands and optimisation flags as ../build.sh and
# ../third_party/ffx_src/build.sh, but nothing is written into the source tree: it is mounted
# read-only at /src, and everything is built under appimage/work.
#
#   appimage/build-layer.sh [--clean]      --clean: the FFX library and the layer are built anew
#                                           (the downloaded and provisioned tools are kept)
#
# Result: appimage/out/layer/{fg/libsc_fg.so, fg/sc_fg.json, libsc_probe.so, sc_probe.json,
# BUILDINFO, licenses/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt}. Then appimage/verify-layer.sh
# (symbol versions, exports, loading in clean containers) and appimage/test-layer-headless.sh (the
# project's own test program, on the GPU, headless). Needs podman (rootless), curl, sha256sum and
# the network the first time.
#
# What is pinned (and checked by sha256): the Vulkan headers 1.4.341 (the version the native build
# uses), glslang 16.2.0 (compiles AMD's shaders; the same version as the native build's, so the
# SPIR-V in the library is the same), the key of apt.llvm.org. clang 22 comes from apt.llvm.org
# (the native build's major version; that repository only keeps the newest 22.x, see BUILDINFO).
#
# Environment: JOBS (default 6), PODMAN (default podman).
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"   # appimage/
project="$(dirname "$here")"
work="$here/work"
out="$here/out/layer"
name=myrddin-layer-build
image=docker.io/library/ubuntu:22.04
mirror=mirrors.edge.kernel.org/ubuntu/
jobs="${JOBS:-6}"
podman="${PODMAN:-podman}"
llvm=22
provision_version=1   # raise when the package list below changes: the container is provisioned again

vh_version=1.4.341
vh_url="https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$vh_version.tar.gz"
vh_sha=8876aad926b2e72bdefddc34885472d5332e2d20aa3ed57313466f0bc982cf3f
gl_version=16.2.0
gl_url="https://github.com/KhronosGroup/glslang/archive/refs/tags/$gl_version.tar.gz"
gl_sha=01985335785c97906a91afe3cb5ee015997696181ec6c125bab5555602ba08e2
key_url=https://apt.llvm.org/llvm-snapshot.gpg.key
key_sha=8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d   # 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421

clean=0
for arg in "$@"; do
    case "$arg" in
    --clean) clean=1 ;;
    *) sed -n '2,/^set -e/{/^set -e/d;s/^# \{0,1\}//;p}' "$0"; exit 1 ;;
    esac
done

# Downloads go to work/dl/NAME/ (each in its own folder) and are checked against the pinned sum.
fetch() {   # fetch DIR FILE URL SHA256
    local dir="$work/dl/$1" file="$work/dl/$1/$2"
    mkdir -p "$dir"
    if [ ! -f "$file" ]; then
        echo "download: $3"
        curl -fsSL -m 300 -o "$file.part" "$3"
        mv -f "$file.part" "$file"
    fi
    if ! echo "$4  $file" | sha256sum -c --quiet -; then
        echo "checksum of $file does not match the pinned one: remove it to download again, or check the source"
        exit 1
    fi
}
mkdir -p "$work" "$out/fg"
fetch vulkan-headers "Vulkan-Headers-v$vh_version.tar.gz" "$vh_url" "$vh_sha"
fetch glslang "glslang-$gl_version.tar.gz" "$gl_url" "$gl_sha"
fetch llvm llvm-snapshot.gpg.key "$key_url" "$key_sha"

# The container: made once, then reused. The project is mounted read-only at /src, the work
# folder read-write at /work; a container with other mounts (the project moved) is made anew.
mounts_ok() {
    local m
    m="$("$podman" inspect --format '{{range .Mounts}}{{.Source}}={{.Destination}}:{{.RW}} {{end}}' "$name" 2>/dev/null)" || return 1
    case " $m " in *" $project=/src:false "*) ;; *) return 1 ;; esac
    case " $m " in *" $work=/work:true "*) ;; *) return 1 ;; esac
}
if "$podman" container exists "$name" && ! mounts_ok; then
    echo "container $name has other mounts: made anew"
    "$podman" rm -f "$name" > /dev/null
fi
if ! "$podman" container exists "$name"; then
    "$podman" image exists "$image" || nice -n 19 "$podman" pull -q "$image" > /dev/null
    "$podman" run -d --name "$name" --security-opt label=disable \
        -v "$project:/src:ro" -v "$work:/work" -w /work "$image" sleep infinity > /dev/null
    echo "container $name made ($image)"
elif [ "$("$podman" inspect --format '{{.State.Running}}' "$name")" != true ]; then
    "$podman" start "$name" > /dev/null
fi

# Everything else runs inside it.
"$podman" exec -i -e JOBS="$jobs" -e LLVM="$llvm" -e CLEAN="$clean" -e PROVISION="$provision_version" \
    -e MIRROR="$mirror" -e VH="$vh_version" -e GL="$gl_version" "$name" bash -s <<'INSIDE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
nice="nice -n 19"

# --- provisioning (once per provision_version) ---
if [ "$(cat /opt/provisioned 2>/dev/null)" != "$PROVISION" ]; then
    # Ubuntu's own servers were unreachable from the build machine; kernel.org's mirror is used.
    # First over http (apt checks the signed Release files, so that is safe) to get the CA
    # certificates the base image lacks, then over https.
    sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?#http://$MIRROR#" /etc/apt/sources.list
    $nice apt-get -qq update
    $nice apt-get -qq install -y --no-install-recommends ca-certificates > /dev/null
    sed -i -E "s#http://$MIRROR#https://$MIRROR#" /etc/apt/sources.list
    mkdir -p /etc/apt/keyrings
    cp /work/dl/llvm/llvm-snapshot.gpg.key /etc/apt/keyrings/apt.llvm.org.asc
    echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-$LLVM main" \
        > /etc/apt/sources.list.d/llvm.list
    $nice apt-get -qq update
    # gcc: the C compiler of the probe (build.sh's "cc") and the C runtime files; libstdc++-12-dev:
    # the C++ library clang builds against (and links in statically); libvulkan1: only so that
    # the FFX recipe's find_library(vulkan) is satisfied (a static library links nothing).
    $nice apt-get -qq install -y --no-install-recommends clang-$LLVM gcc libc6-dev libstdc++-12-dev \
        binutils cmake ninja-build python3 libvulkan1 file > /dev/null
    echo "$PROVISION" > /opt/provisioned
    echo "container provisioned"
fi
CXX=clang++-$LLVM

# --- dependencies: Vulkan headers, glslang (built once into /work/deps) ---
deps=/work/deps
mkdir -p $deps/src
if [ ! -f $deps/vulkan-headers-$VH/include/vulkan/vulkan_core.h ]; then
    rm -rf $deps/src/Vulkan-Headers-$VH $deps/vulkan-headers-$VH
    tar xzf /work/dl/vulkan-headers/Vulkan-Headers-v$VH.tar.gz -C $deps/src
    mkdir -p $deps/vulkan-headers-$VH
    cp -r $deps/src/Vulkan-Headers-$VH/include $deps/vulkan-headers-$VH/
fi
VKINC=$deps/vulkan-headers-$VH/include
GLSLANG=$deps/glslang-$GL/bin/glslangValidator
if [ ! -x $GLSLANG ]; then
    rm -rf $deps/src/glslang-$GL $deps/build/glslang-$GL
    tar xzf /work/dl/glslang/glslang-$GL.tar.gz -C $deps/src
    # Without SPIRV-Tools (ENABLE_OPT=OFF): only its optimiser needs it, which nothing here asks for.
    $nice cmake -S $deps/src/glslang-$GL -B $deps/build/glslang-$GL -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_C_COMPILER=clang-$LLVM -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
        -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=$deps/glslang-$GL > /dev/null
    $nice cmake --build $deps/build/glslang-$GL --parallel "$JOBS" > /dev/null
    $nice cmake --install $deps/build/glslang-$GL > /dev/null
fi

# --- AMD FidelityFX frame interpolation (third_party/ffx_src), as its build.sh does it ---
b=/work/build
[ "$CLEAN" = 1 ] && rm -rf $b/ffx $b/layer $b/probe
mkdir -p $b/ffx $b/layer $b/probe
$nice cmake -S /src/third_party/ffx_src -B $b/ffx -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=$CXX -DFFX_VULKAN_INCLUDE_DIR=$VKINC -DFFX_GLSLANG=$GLSLANG \
    -DFFX_VULKAN_LOADER=/usr/lib/x86_64-linux-gnu/libvulkan.so.1 > $b/ffx-configure.log
$nice cmake --build $b/ffx --target ffx_fg --parallel "$JOBS" > $b/ffx-build.log
echo "ffx: $b/ffx/ffx_fg/libffx_fg.a"

# --- the layers. Only the Vulkan entry point is exported (see exports.map); libstdc++ is linked
# in and its symbols kept inside (--exclude-libs). ---
cat > $b/exports.map <<'MAP'
/* The Vulkan loader finds a layer through this one function (and its manifest); everything else,
   the C++ library's template instances among it, stays inside the library. */
{
  global: vkNegotiateLoaderLayerInterfaceVersion;
  local: *;
};
MAP
# The passive probe layer, build.sh's command (+ the Vulkan headers' path, static libgcc, the export list).
$nice cc -O2 -Wall -fPIC -fvisibility=hidden -shared -isystem $VKINC -o $b/probe/libsc_probe.so /src/probe/sc_probe.c \
    -lpthread -static-libgcc -Wl,--version-script=$b/exports.map
echo "probe: $b/probe/libsc_probe.so"
# The frame-generation layer, build.sh's command (+ the Vulkan headers' path, static libstdc++,
# the export list). The shader headers fg/*.spv.h are used as they are in the source tree.
#
# Why libgcc is not linked in statically here: Wine ends a game's threads with pthread_exit
# wherever they are, and glibc then unwinds their stacks with the SYSTEM's libgcc_s. For each of
# the layer's frames that has something to tidy up, that unwinder calls the C++ library's
# personality routine, which is inside this library; with a libgcc of its own linked in, the
# routine would answer through its own copy of the unwinder (Ubuntu 22.04's), which reads the
# system unwinder's records wrongly on a newer system and aborts the game as it closes
# (StarCitizen.exe, SIGABRT in _Unwind_SetGR, a core dump of 25 GB: 2026-11-20). Linked this way,
# the personality routine calls the one unwinder that is at work. libgcc_s.so.1 is on every
# system that runs Wine: glibc cannot end a thread without it. The case is in
# test-layer-headless.sh ("unwind").
$nice $CXX -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fPIC \
    -fvisibility=hidden -shared -I/src/fg -include /src/third_party/lichii/ffx_linux_compat.h \
    -isystem /src/third_party/ffx/include -isystem $VKINC -o $b/layer/libsc_fg.so /src/fg/sc_fg.cpp \
    $b/ffx/ffx_fg/libffx_fg.a -static-libstdc++ -Wl,--exclude-libs,ALL -Wl,-z,defs \
    -Wl,--version-script=$b/exports.map -lpthread
echo "layer: $b/layer/libsc_fg.so"

{
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) in $(. /etc/os-release; echo "$PRETTY_NAME") (glibc $(ldd --version | head -1 | awk '{print $NF}'))"
    echo "c++: $($CXX --version | head -1)"
    echo "cc: $(cc --version | head -1)"
    echo "libstdc++ (static): $(dpkg-query -W -f '${Package} ${Version}' libstdc++-12-dev)"
    echo "glslang: $($GLSLANG --version | head -1)"
    echo "vulkan headers: $VH"
    echo "cmake: $(cmake --version | head -1); ninja $(ninja --version)"
} > $b/BUILDINFO
# libstdc++ and helper routines of libgcc are linked in: their licence is GPLv3 with this exception (for THIRD-PARTY.md).
sed -n '/^GCC RUNTIME LIBRARY EXCEPTION$/,/requirements of the license of GCC\.$/p' /usr/share/doc/gcc-12-base/copyright \
    > $b/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt
INSIDE

# Into appimage/out/layer, laid out as the launcher expects them (build/fg/, build/).
cp -p "$work/build/layer/libsc_fg.so" "$out/fg/libsc_fg.so"
cp -p "$project/fg/sc_fg.json" "$out/fg/sc_fg.json"
cp -p "$work/build/probe/libsc_probe.so" "$out/libsc_probe.so"
cp -p "$project/probe/sc_probe.json" "$out/sc_probe.json"
mkdir -p "$out/licenses"
cp -p "$work/build/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt" "$out/licenses/"
{
    cat "$work/build/BUILDINFO"
    echo "source: $(git -C "$project" describe --always --dirty 2>/dev/null || echo unknown)"
    (cd "$out" && sha256sum fg/libsc_fg.so libsc_probe.so && stat -c '%n %s bytes' fg/libsc_fg.so libsc_probe.so)
} > "$out/BUILDINFO"
cat "$out/BUILDINFO"
