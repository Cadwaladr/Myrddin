#!/usr/bin/env bash
# Assembles appimage/out/payload/: the runtime tree the AppImage carries, laid out as sc-turbo
# expects it beside itself (it finds everything relative to its own folder):
#
#   sc-turbo, sc-turbo.conf.example, LICENSE, README.md, docs/, THIRD-PARTY.md, VERSION
#   build/fg/libsc_fg.so, build/fg/sc_fg.json, build/libsc_probe.so, build/sc_probe.json, build/BUILDINFO
#       (the portable layers from appimage/out/layer, made by build-layer.sh; never the project's
#       own build/, which holds the owner's development builds)
#   build/vram-limit/   Mesa's layer VK_LAYER_MESA_vram_report_limit, from the project's
#       build/vram-limit (made by tools/build-vram-layer.sh, in a container like the layers above):
#       what sc-turbo's graphics memory limit works through where the system has no such layer
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
    "sc-launch-copy|sc-turbo: the copy of the launch script that every launch starts (own_launch)"
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
vram="$project/build/vram-limit"
[ -f "$vram/libVkLayer_MESA_vram_report_limit.so" ] && [ -f "$vram/LICENSE" ] || { echo "no memory layer in $vram: tools/build-vram-layer.sh first"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
new="$payload.new"
rm -rf "$new"; mkdir -p "$new/build/fg" "$new/tools" "$new/licenses"

cp -p "$project/sc-turbo" "$project/sc-turbo.conf.example" "$project/LICENSE" "$project/README.md" "$new/"
cp -pr "$project/gui" "$new/gui"
cp -pr "$project/docs" "$new/docs"   # the pages README.md links to
for entry in "${TOOLS[@]}"; do cp -p "$project/tools/${entry%%|*}" "$new/tools/"; done
cp -p "$layer/fg/libsc_fg.so" "$layer/fg/sc_fg.json" "$new/build/fg/"
cp -p "$layer/libsc_probe.so" "$layer/sc_probe.json" "$layer/BUILDINFO" "$new/build/"
mkdir -p "$new/build/vram-limit"
cp -p "$vram/libVkLayer_MESA_vram_report_limit.so" "$vram/VkLayer_MESA_vram_report_limit.json" "$vram/VERSION" "$new/build/vram-limit/"
cp -p "$vram/LICENSE" "$new/licenses/Mesa-vram-report-limit-MIT.txt"
cp -p "$vram/MESA-LICENSE.rst" "$new/licenses/Mesa-licences.rst"
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
The frame generation from the game's motion vectors is Myrddin's own implementation. The technique
itself, taking the motion vectors a game already makes and running FSR's frame interpolation on
them, was found in a PS5 emulator that had it built in. What was first written for Lichii, an
emulator by the same authors made to run Nintendo Switch 2 games, and carried over is in the
second row. The frame-generation layer, `build/fg/libsc_fg.so`, contains this code and data from
elsewhere:

| Component | Where it is | Licence | Text |
|---|---|---|---|
| AMD FidelityFX SDK 1.1.4 (frame interpolation, optical flow), Copyright (C) 2024 Advanced Micro Devices, Inc., with our changes that yield several generated frames per pair | linked into `libsc_fg.so` | MIT | `licenses/FidelityFX-SDK-MIT.txt` |
| Our Linux build recipe of the SDK, its compatibility header and swapchain stub, first written for Lichii; the files carry the notice of the project they were written in, Copyright 2026 Eden Emulator Project | linked into `libsc_fg.so` | GPL-3.0-or-later | `LICENSE` |
| Adwaita Sans (the performance panel's letters, as a glyph atlas), Copyright the Inter Project Authors, Renzhi Li, Jamie Gravendeel, Florian Müllner | compiled into `libsc_fg.so` | SIL Open Font License 1.1 | `licenses/AdwaitaSans-OFL-1.1.txt` |
| GNU C++ library (libstdc++) and GCC's runtime library (libgcc), Copyright Free Software Foundation, Inc. | linked statically into `libsc_fg.so` (and libgcc into `libsc_probe.so`) so that they need no C++ library of the system | GPL-3.0 with the GCC Runtime Library Exception 3.1 | `LICENSE`, `licenses/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt` |
| Khronos Vulkan headers 1.4.341 | used when compiling (types and constants) | Apache-2.0 OR MIT | not required for compiled use |

`build/BUILDINFO` says how and from which source the libraries were built.

A library of its own, not part of the layer above:

| Component | Where it is | Licence | Text |
|---|---|---|---|
| The Vulkan layer `VK_LAYER_MESA_vram_report_limit` of the Mesa project, unchanged, with the parts of Mesa's utility code it links; Copyright 2017 Google, 2019 Red Hat, 2024 Igalia S.L. and Mesa's contributors | `build/vram-limit/libVkLayer_MESA_vram_report_limit.so`, loaded into the game when a graphics memory limit is set (`SC_VRAM_LIMIT`) and the system has no such layer of its own | MIT | `licenses/Mesa-vram-report-limit-MIT.txt`, `licenses/Mesa-licences.rst` |

`build/vram-limit/VERSION` names the Mesa release it was built from.

Star Citizen is a trademark of Cloud Imperium Games; FidelityFX and FSR are AMD's; DLSS is
NVIDIA's; Nintendo Switch is Nintendo's; PS5 is Sony Interactive Entertainment's. Myrddin is an
independent project: none of them is affiliated with it or endorses it.
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
# Nothing that is published asks for root: no script in the payload runs sudo, pkexec, doas, run0 or su,
# and none asks polkit in a way that may open a password dialog. (Lines that are only a comment do not
# count: tools/sc-sched's header tells how a user may install a polkit rule themselves.)
found="$(for f in "${shells[@]}" "${pythons[@]}"; do
             grep -n -E '\b(sudo|pkexec|doas|run0)\b|\bsu +-c?\b|allow-user-interaction|pkttyagent' "$f" | grep -v -E '^[0-9]+:[[:space:]]*#' | sed "s|^|${f#$payload/}:|"
         done | head -5)"
[ -z "$found" ]; check $? "nothing in the payload asks for root${found:+: $found}"
before="$(cd "$payload" && find . | sort)"
usage="$("$payload/sc-turbo" 2>&1)"; rc=$?   # (set +e: a failing command is only reported)
# (no "echo | head -1" here: head leaves after one line, echo is then cut off, and pipefail calls that a failure)
[ $rc = 1 ] && [[ "${usage%%$'\n'*}" == '# sc-turbo: launches Star Citizen'* ]]; check $? "sc-turbo without arguments: usage ($(wc -l <<< "$usage") lines), exit $rc"
displays="$("$payload/sc-turbo" displays 2>&1)"; rc=$?
check $rc "sc-turbo displays (exit $rc):"; echo "$displays" | sed 's/^/       /'
after="$(cd "$payload" && find . | sort)"
[ "$before" = "$after" ]; check $? "running them wrote nothing into the payload"
echo "size: $(du -sh "$payload" | cut -f1) in $(find "$payload" -type f | wc -l) files"
exit $fail
