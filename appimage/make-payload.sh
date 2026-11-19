#!/usr/bin/env bash
# Assembles appimage/out/payload/: the runtime tree the AppImage carries, laid out as sc-turbo
# expects it beside itself (it finds everything relative to its own folder):
#
#   sc-turbo, sc-turbo.conf.example, LICENSE, README.md, THIRD-PARTY.md, VERSION
#   build/fg/libsc_fg.so, build/fg/sc_fg.json, build/libsc_probe.so, build/sc_probe.json, build/BUILDINFO
#       (the portable layers from appimage/out/layer, made by build-layer.sh; never the project's
#       own build/, which holds the owner's development builds)
#   gui/      as it is in the project
#   tools/    only what sc-turbo can run (see TOOLS below, one reason each)
#   licenses/ the third-party licence texts the libraries need (see THIRD-PARTY.md)
#
#   appimage/make-payload.sh          (then checks the payload; exit 1 when a check fails)
#
# Reads the project, writes only appimage/out/payload (made anew each time) and a temporary folder.
set -euo pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"   # appimage/
project="$(dirname "$here")"
layer="$here/out/layer"
payload="$here/out/payload"

# What sc-turbo runs from tools/ (every "$here/tools/..." in it), and what those run in turn.
TOOLS=(
    "sc-monitor|sc-turbo: the display's name and rate at every launch (display_settings), and \"sc-turbo displays\""
    "sc-sched|sc-turbo: \"sched=NAME\" / SC_SCHED at launch (allowed, follow); \"tools/sc-sched rule\" is named in the usage and sc-turbo.conf.example"
    "sc-iowatch|sc-turbo: \"fg N watch\" starts it in the background, \"sc-turbo iowatch [report]\""
    "sc-panel-key|sc-turbo: \"fg-panel-key\""
    "fg-where.py|sc-turbo: \"fg-where\""
    "fg-watch.py|sc-turbo: \"fg-watch\""
    "fg-ab.sh|sc-turbo: \"fg-ab\"; also run by sc-ab-threads.sh"
    "latency-report.py|sc-turbo: \"latency\""
    "sc-threads|sc-turbo: \"threads\" and \"check\"; also run by sc-ab-threads.sh (through fg-ab.sh's \"cmd:\" sides)"
    "sc-ab-threads.sh|sc-turbo: \"threads-ab\""
    "display-timing.py|not run by sc-turbo, but its usage names it as the reader of what \"fg-events\" writes (presents-*.bin)"
)

[ -f "$layer/fg/libsc_fg.so" ] && [ -f "$layer/libsc_probe.so" ] || { echo "no portable layers in $layer: appimage/build-layer.sh first"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
new="$payload.new"
rm -rf "$new"; mkdir -p "$new/build/fg" "$new/tools" "$new/licenses"

cp -p "$project/sc-turbo" "$project/sc-turbo.conf.example" "$project/LICENSE" "$project/README.md" "$new/"
cp -pr "$project/gui" "$new/gui"
for entry in "${TOOLS[@]}"; do cp -p "$project/tools/${entry%%|*}" "$new/tools/"; done
cp -p "$layer/fg/libsc_fg.so" "$layer/fg/sc_fg.json" "$new/build/fg/"
cp -p "$layer/libsc_probe.so" "$layer/sc_probe.json" "$layer/BUILDINFO" "$new/build/"
cp -p "$project/third_party/ffx_src/FidelityFX-SDK/LICENSE.txt" "$new/licenses/FidelityFX-SDK-MIT.txt"
cp -p "$here/licenses/AdwaitaSans-OFL-1.1.txt" "$new/licenses/"
cp -p "$layer/licenses/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt" "$new/licenses/"
{
    git -C "$project" describe --always --dirty 2>/dev/null || echo unknown
    date -u '+%Y-%m-%dT%H:%M:%SZ'
} > "$new/VERSION"

cat > "$new/THIRD-PARTY.md" <<'EOF'
# Third-party code in Myrddin

Myrddin is free software under the GNU General Public License, version 3 or later (`LICENSE`).
The frame-generation layer, `build/fg/libsc_fg.so`, contains code and data from elsewhere:

| Component | Where it is | Licence | Text |
|---|---|---|---|
| AMD FidelityFX SDK 1.1.4 (frame interpolation, optical flow), Copyright (C) 2024 Advanced Micro Devices, Inc., with changes that yield several generated frames per pair | linked into `libsc_fg.so` | MIT | `licenses/FidelityFX-SDK-MIT.txt` |
| Linux build recipe of the SDK, its compatibility header and swapchain stub, from the Lichii project, Copyright 2026 Eden Emulator Project | linked into `libsc_fg.so` | GPL-3.0-or-later | `LICENSE` |
| Adwaita Sans (the performance panel's letters, as a glyph atlas), Copyright the Inter Project Authors, Renzhi Li, Jamie Gravendeel, Florian Müllner | compiled into `libsc_fg.so` | SIL Open Font License 1.1 | `licenses/AdwaitaSans-OFL-1.1.txt` |
| GNU C++ library (libstdc++) and GCC's runtime library (libgcc), Copyright Free Software Foundation, Inc. | linked statically into `libsc_fg.so` (and libgcc into `libsc_probe.so`) so that they need no C++ library of the system | GPL-3.0 with the GCC Runtime Library Exception 3.1 | `LICENSE`, `licenses/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt` |
| Khronos Vulkan headers 1.4.341 | used when compiling (types and constants) | Apache-2.0 OR MIT | not required for compiled use |

`build/BUILDINFO` says how and from which source the libraries were built.

Star Citizen is a trademark of Cloud Imperium Games; FidelityFX and FSR are AMD's; DLSS is
NVIDIA's. Myrddin is an independent project: none of them is affiliated with it or endorses it.
EOF

rm -rf "$payload"; mv "$new" "$payload"
echo "payload: $payload"

# --- checks (each one reported; the script goes on after a failure) ---
set +e
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; fail=1; fi; }
shells=(); pythons=()
while IFS= read -r f; do
    case "$(head -1 "$f")" in
    '#!'*bash*|'#!'*/sh) shells+=("$f") ;;
    '#!'*python3*) pythons+=("$f") ;;
    esac
done < <(find "$payload" -type f ! -name '*.so' | sort)
r=0; for f in "${shells[@]}"; do bash -n "$f" || r=1; done
check $r "bash -n on ${#shells[@]} shell scripts: $(printf '%s ' "${shells[@]#$payload/}")"
r=0; python3 -I - "$tmp/pyc" "${pythons[@]}" <<'PY' || r=1
import os, py_compile, sys
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
for f in sys.argv[2:]:
    py_compile.compile(f, cfile=os.path.join(out, os.path.basename(f) + ".pyc"), doraise=True)
PY
check $r "py_compile (into a temporary folder) on ${#pythons[@]} Python tools: $(printf '%s ' "${pythons[@]#$payload/}")"
prefix="$(echo "$project" | cut -d/ -f1-3)"   # the machine's own folder the project lives under (e.g. /mnt/xyz)
# (the home folder and the project's own path are searched too, but not printed: they name the machine's user)
for pattern in "/home/|/home/" "$prefix|$prefix" "$HOME|the home folder" "$project|the project's path"; do
    found="$(grep -rnF -- "${pattern%%|*}" "$payload" | head -5)"
    [ -z "$found" ]; check $? "no \"${pattern#*|}\" in the payload${found:+: $found}"
done
r=0; [ -z "$(find "$payload" -name __pycache__ -o -name '*.pyc')" ] || r=1
check $r "no __pycache__ or .pyc in the payload"
before="$(cd "$payload" && find . | sort)"
usage="$("$payload/sc-turbo" 2>&1)"; rc=$?   # (set +e: a failing command is only reported)
[ $rc = 1 ] && echo "$usage" | head -1 | grep -q '^# sc-turbo: launches Star Citizen'; check $? "sc-turbo without arguments: usage ($(echo "$usage" | wc -l) lines), exit $rc"
displays="$("$payload/sc-turbo" displays 2>&1)"; rc=$?
check $rc "sc-turbo displays (exit $rc):"; echo "$displays" | sed 's/^/       /'
after="$(cd "$payload" && find . | sort)"
[ "$before" = "$after" ]; check $? "running them wrote nothing into the payload"
echo "size: $(du -sh "$payload" | cut -f1) in $(find "$payload" -type f | wc -l) files"
exit $fail
