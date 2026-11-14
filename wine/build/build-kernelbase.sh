#!/usr/bin/env bash
# Rebuild ONLY kernelbase.dll (x86_64 PE) from upstream Wine + our patches, inside a
# rootless podman container, so it can be dropped into a copy of the matching LUG runner.
#
#   ./build-kernelbase.sh            # clone (if needed), patch, configure, build, strip
#   JOBS=6 ./build-kernelbase.sh     # use more cores (default 3, always nice 19)
#
# Output: out/kernelbase.dll (+ out/kernelbase.dll.debug with symbols)
#
# The Wine tag must match the runner you patch (lug-wine-tkg-...-11.20-2 -> wine-11.20).
# LUG's 11.20-2 runner ships upstream kernelbase sources, which is what makes a
# single-DLL swap safe; re-check that before using a newer tag.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
WINE_TAG=${WINE_TAG:-wine-11.20}
JOBS=${JOBS:-3}
IMAGE=${IMAGE:-sc-wine-builder}
SRC=$here/$WINE_TAG
OBJ=$here/obj-$WINE_TAG-x86_64
PATCH_DIR=$here/../patches

if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$WINE_TAG" https://gitlab.winehq.org/wine/wine.git "$SRC"
fi

for p in "$PATCH_DIR"/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$SRC" apply --check "$p" 2>/dev/null; then
        echo "applying $(basename "$p")"
        git -C "$SRC" apply "$p"
    elif git -C "$SRC" apply --check -R "$p" 2>/dev/null; then
        echo "already applied: $(basename "$p")"
    else
        echo "ERROR: $(basename "$p") neither applies nor is already applied" >&2
        exit 1
    fi
done

if ! podman image exists "$IMAGE"; then
    nice -n 19 podman build -t "$IMAGE" -f "$here/Containerfile" "$here"
fi

mkdir -p "$OBJ" "$here/out"
nice -n 19 podman run --rm --security-opt label=disable \
    --cpu-quota=$((JOBS * 100000)) --cpu-period=100000 \
    -v "$here:/build" -w "/build/$(basename "$OBJ")" "$IMAGE" bash -euc "
        if [ ! -f Makefile ]; then
            nice -n 19 ../$WINE_TAG/configure --enable-win64 --without-x --without-freetype --disable-tests \
                CROSSCFLAGS='-O2 -ftree-vectorize -g' > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
        fi
        nice -n 19 make -j$JOBS dlls/kernelbase/x86_64-windows/kernelbase.dll
        cp dlls/kernelbase/x86_64-windows/kernelbase.dll /build/out/kernelbase.dll.debug
        x86_64-w64-mingw32-strip -o /build/out/kernelbase.dll /build/out/kernelbase.dll.debug
    "
ls -l "$here/out"
