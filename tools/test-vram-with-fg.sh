#!/usr/bin/env bash
# Mesa's VK_LAYER_MESA_vram_report_limit (build/vram-limit) together with this project's
# frame-generation layer in one program: the project's own test program (build/test_chain/test_chain)
# with the installed layer (build/fg), headless, twice without the memory layer and twice with it, in
# turn. Checks that the Vulkan loader took the memory layer in, that the frame chain is intact
# (unlinked=0 shift=0, no "CHAIN OFF", nothing lost) and prints the rates of both.
# The run itself is appimage/test-layer-headless.sh's: a weston of its own with the headless backend
# on a socket of its own, never the desktop's display. It does not run while the game does.
#   tools/test-vram-with-fg.sh        (about one minute; results in appimage/work/vram-layer/test-fg)
set -u
project="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
root="$project/appimage/work/vram-layer/test-fg"
native="$project/build/fg/libsc_fg.so"
manifest="$project/fg/sc_fg.json"
program="$project/build/test_chain/test_chain"
vram="$project/build/vram-limit"
socket=myrddin-vram-test

if pgrep -x StarCitizen.exe > /dev/null; then echo "the game is running: not now (the GPU is shared)"; exit 1; fi
for f in "$native" "$manifest" "$program" "$vram/VkLayer_MESA_vram_report_limit.json"; do [ -e "$f" ] || { echo "missing: $f"; exit 1; }; done
command -v weston > /dev/null || { echo "weston is needed (the headless compositor)"; exit 1; }
card="$(nvidia-smi --query-gpu=pci.device_id --format=csv,noheader 2>/dev/null | head -1 | tr 'A-F' 'a-f')"
[ "${#card}" = 10 ] || { echo "no NVIDIA card to ask"; exit 1; }
with="VK_ADD_LAYER_PATH=$vram VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_vram_report_limit VK_VRAM_REPORT_LIMIT_HEAP_SIZE=9216 VK_VRAM_REPORT_LIMIT_DEVICE_ID=0x${card:6:4}:0x${card:2:4}"
mkdir -p "$root"
waits() { local t=$(( $(date +%s) + $1 )); until [ "$(date +%s)" -ge "$t" ]; do sleep 1; done; }

one() {   # one NAME SECONDS "ENV=.."
    local name=$1 secs=$2 envs=$3 out wpid rc
    out="$root/$name"; rm -rf "$out"; mkdir -p "$out/layer"
    cp "$native" "$out/layer/libsc_fg.so"; cp "$manifest" "$out/layer/"
    weston --backend=headless --renderer=gl --socket=$socket --width=1280 --height=720 > "$out/weston.txt" 2>&1 &
    wpid=$!
    waits 2
    ( waits $(( secs - 2 )); touch "$out/EVENTS" ) &
    # shellcheck disable=SC2086
    env WAYLAND_DISPLAY=$socket DISPLAY= SC_FG=1 SC_FG_MATCH=test_chain SC_FG_TEST_INPUTS=1 SC_FG_TARGET=60 SC_FG_MULT=2 \
        SC_FG_MAX_BASE=0 SC_FG_DIR="$out" VK_ADD_IMPLICIT_LAYER_PATH="$out/layer" DISABLE_LSFGVK=1 VK_LOADER_DEBUG=warn $envs \
        timeout $(( secs + 15 )) "$program" "$secs" > "$out/out.txt" 2>&1
    rc=$?
    wait $(jobs -p | grep -vx "$wpid") 2>/dev/null
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    echo "exit $rc" >> "$out/out.txt"
}
line() {
    local n=$1 st late
    st=$(grep -h STAT "$root/$n"/fg-*.log 2>/dev/null | tail -1)
    late=$(grep -ho 'late copies=[0-9]* lost=[0-9]*' "$root/$n"/fg-*.log 2>/dev/null | awk -F'[= ]' '{ l += $3; x += $5 } END { printf "%d/%d", x, l }')
    printf "%-8s %s | %s | off=%s lost/late=%s | %s | memory layer loaded: %s\n" "$n" "$(grep -v -E '^exit|Loader Message' "$root/$n/out.txt" | tail -1)" \
        "$(echo "$st" | grep -o 'chain=[0-9]* dup=[0-9]* unlinked=[0-9]* nohud=[0-9]* multi=[0-9]* shift=[-0-9]*')" \
        "$(grep -hc 'CHAIN OFF' "$root/$n"/fg-*.log 2>/dev/null)" "$late" "$(echo "$st" | grep -o 'real=[0-9.]*/s generated=[0-9.]*/s shown=[0-9.]*/s')" \
        "$(grep -q 'VK_LAYER_MESA_vram_report_limit.*forced enabled' "$root/$n/out.txt" && echo yes || echo no)"
}
fails=0
healthy() {   # healthy NAME yes|no
    local n=$1 st; st=$(grep -h STAT "$root/$n"/fg-*.log 2>/dev/null | tail -1)
    echo "$st" | grep -q 'unlinked=0 ' && echo "$st" | grep -q 'shift=0' && [ "$(grep -hc 'CHAIN OFF' "$root/$n"/fg-*.log 2>/dev/null)" = 0 ] &&
        grep -q '^exit 0' "$root/$n/out.txt" && [ -n "$(echo "$st" | grep -o 'generated=[1-9]')" ] || { echo "  NOT HEALTHY: $n"; fails=$((fails + 1)); }
    local loaded=no; grep -q 'VK_LAYER_MESA_vram_report_limit.*forced enabled' "$root/$n/out.txt" && loaded=yes
    [ "$loaded" = "$2" ] || { echo "  memory layer loaded is $loaded in $n, expected $2"; fails=$((fails + 1)); }
}
for i in 1 2; do
    one "plain$i" 12 ""; line "plain$i"; healthy "plain$i" no
    one "vram$i" 12 "$with"; line "vram$i"; healthy "vram$i" yes
done
pgrep -a weston | grep -- "--socket=$socket" && echo "NOTE: a weston on $socket is still running"
if [ "$fails" = 0 ]; then echo "both layers work together in the test program"; else echo "$fails problem(s), see $root"; fi
exit "$((fails > 0))"
