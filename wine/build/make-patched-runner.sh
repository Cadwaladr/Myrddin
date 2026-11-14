#!/usr/bin/env bash
# Make a copy of a LUG runner with the CPU-sets kernelbase.dll swapped in.
#
#   ./make-patched-runner.sh /path/to/runners/lug-wine-tkg-experimental-wayland-git-11.20-2 [dest-dir]
#
# Default destination: ../runners/<runner-name>-cpusets.  The original runner is only
# read.  The copy keeps the original DLL as kernelbase.dll.lug-orig, so
#   cp kernelbase.dll.lug-orig kernelbase.dll
# inside lib/wine/x86_64-windows reverts it.  cp -a keeps wine.inf's timestamp, so
# switching a prefix between the original and the copy does not trigger a prefix update.
#
# The DLL must come from the same Wine version as the runner (build-kernelbase.sh,
# WINE_TAG=wine-<runner version>); the version check below refuses a mismatch.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: $0 <lug-runner-dir> [dest-dir]}
src=$(cd "$src" && pwd)
dest=${2:-$here/../runners/$(basename "$src")-cpusets}
dll=$here/out/kernelbase.dll
target=lib/wine/x86_64-windows/kernelbase.dll

[ -f "$src/$target" ] || { echo "not a 64-bit Wine runner: $src" >&2; exit 1; }
[ -f "$dll" ] || { echo "build the DLL first: $here/build-kernelbase.sh" >&2; exit 1; }
[ -e "$dest" ] && { echo "already exists: $dest" >&2; exit 1; }

runner_version=$("$src/bin/wine" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)
built_version=$(sed -n 's/^WINE_TAG=\${WINE_TAG:-wine-\(.*\)}$/\1/p' "$here/build-kernelbase.sh")
if [ -n "${WINE_TAG:-}" ]; then built_version=${WINE_TAG#wine-}; fi
if [ "$runner_version" != "$built_version" ]; then
    echo "version mismatch: runner is wine-$runner_version, DLL was built from wine-$built_version" >&2
    exit 1
fi

nice -n 19 ionice -c3 cp -a "$src" "$dest"
cp -a "$dest/$target" "$dest/$target.lug-orig"
install -m 755 "$dll" "$dest/$target"
echo "patched runner: $dest"
echo "use it with:    export wine_path=\"$dest/bin\""
