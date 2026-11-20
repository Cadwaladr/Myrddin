#!/usr/bin/env bash
# The portable frame-generation layer (appimage/out/layer/fg/libsc_fg.so) against the native one
# (build/fg/libsc_fg.so, read only) in the project's own two-device test program
# (build/test_chain/test_chain), headless: a weston of its own with the headless backend, on a
# socket of its own; the desktop's WAYLAND_DISPLAY/DISPLAY are never passed on. Two cases, each
# native and portable in turn:
#   plain   tools/fg-validate.sh's first regression case (30 frames/s held, x2 at 60 Hz, 12 s), twice
#           each, interleaved: the frame chain intact (unlinked=0 shift=0, no "CHAIN OFF", lost 0)
#           and the same rates
#   hose    fg-validate.sh's "hose" case: a dump of the game (logs/fg/dump-236448-000) replayed at
#           2560x1440, a generated frame dumped after 6 s; the generated picture of the portable build
#           against the native one's (tools/ppmdiff.py), once both dumps caught the same real picture
#           (up to 6 tries, as fg-validate.sh does)
#
#   unwind  the presenting thread ended inside the present call the way Wine ends a game's threads
#           (test_chain --unwind): the process must not abort. The portable build did, as long as it
#           carried an unwinder of its own (build-layer.sh says why); the native build is the control.
#
#   appimage/test-layer-headless.sh [plain|hose|unwind|all]     (default all; about 3 to 5 minutes)
#
# Like tools/test-chain.sh, from which the run itself is taken, except: results go to
# appimage/work/test-headless/NAME (not logs/test), the weston it stops is its own (test-chain.sh
# stops every weston of the user), and it does not run while the game does (the GPU is shared).
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
project="$(dirname "$here")"
root="$here/work/test-headless"
portable="$here/out/layer/fg/libsc_fg.so"
native="$project/build/fg/libsc_fg.so"
manifest="$project/fg/sc_fg.json"
program="$project/build/test_chain/test_chain"
replay="$project/logs/fg/dump-236448-000"
socket=myrddin-appimage-test
what="${1:-all}"

if pgrep -x StarCitizen.exe > /dev/null; then echo "the game is running: not now (the GPU is shared)"; exit 1; fi
for f in "$portable" "$native" "$program"; do [ -e "$f" ] || { echo "missing: $f"; exit 1; }; done
command -v weston > /dev/null || { echo "weston is needed (the headless compositor)"; exit 1; }
mkdir -p "$root"
waits() { local t=$(( $(date +%s) + $1 )); until [ "$(date +%s)" -ge "$t" ]; do sleep 1; done; }

# one NAME LIB SECONDS "ENV=.." [test_chain options]: one run, as tools/test-chain.sh makes it
one() {
    local name=$1 lib=$2 secs=$3 envs=$4 out wpid rc; shift 4
    out="$root/$name"; rm -rf "$out"; mkdir -p "$out/layer"
    cp "$lib" "$out/layer/libsc_fg.so"; cp "$manifest" "$out/layer/"
    local size=${TEST_SIZE:-1280x720}
    weston --backend=headless --renderer=gl --socket=$socket --width="${size%x*}" --height="${size#*x}" > "$out/weston.txt" 2>&1 &
    wpid=$!
    waits 2
    ( waits $(( secs - 2 )); touch "$out/EVENTS" ) &
    [ -n "${TEST_DUMP:-}" ] && ( waits $(( TEST_DUMP + 2 )); touch "$out/DUMP" ) &
    # shellcheck disable=SC2086
    env WAYLAND_DISPLAY=$socket DISPLAY= SC_FG=1 SC_FG_MATCH=test_chain SC_FG_TEST_INPUTS=1 SC_FG_TARGET=60 SC_FG_MULT=2 \
        SC_FG_MAX_BASE=0 SC_FG_DIR="$out" VK_ADD_IMPLICIT_LAYER_PATH="$out/layer" DISABLE_LSFGVK=1 $envs \
        timeout $(( secs + 15 )) "$program" "$secs" "$@" > "$out/out.txt" 2>&1
    rc=$?
    wait $(jobs -p | grep -vx "$wpid") 2>/dev/null
    kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null
    echo "exit $rc" >> "$out/out.txt"
}
# A line per run, as fg-validate.sh's regression table has it.
line() {
    local n=$1 st late
    st=$(grep -h STAT "$root/$n"/fg-*.log 2>/dev/null | tail -1)
    late=$(grep -ho 'late copies=[0-9]* lost=[0-9]*' "$root/$n"/fg-*.log 2>/dev/null | awk -F'[= ]' '{ l += $3; x += $5 } END { printf "%d/%d", x, l }')
    printf "%-10s %s | %s | off=%s lost/late=%s | %s\n" "$n" "$(grep -v '^exit' "$root/$n/out.txt" | tail -1)" \
        "$(echo "$st" | grep -o 'chain=[0-9]* dup=[0-9]* unlinked=[0-9]* nohud=[0-9]* multi=[0-9]* shift=[-0-9]*')" \
        "$(grep -hc 'CHAIN OFF' "$root/$n"/fg-*.log 2>/dev/null)" "$late" "$(echo "$st" | grep -o 'real=[0-9.]*/s generated=[0-9.]*/s shown=[0-9.]*/s')"
}

if [ "$what" = plain ] || [ "$what" = all ]; then
    echo "## plain (12 s, x2 at 60 Hz), native and portable in turn"
    for i in 1 2; do
        one "plain-native$i" "$native" 12 ""; line "plain-native$i"
        one "plain-portable$i" "$portable" 12 ""; line "plain-portable$i"
    done
fi
if [ "$what" = unwind ] || [ "$what" = all ]; then
    echo "## unwind: the presenting thread ended inside the present call, as Wine does when the game closes"
    for build in native portable; do
        lib=$native; [ $build = portable ] && lib=$portable
        ( ulimit -c 0; one "unwind-$build" "$lib" 12 "SC_FG_TARGET=120 SC_FG_MULT=2" --unwind 5 ) 2> /dev/null
        rc=$(sed -n 's/^exit //p' "$root/unwind-$build/out.txt")
        if [ "$rc" = 0 ] && grep -q 'its stack unwound: ok' "$root/unwind-$build/out.txt"; then
            echo "unwind-$build  PASS (the thread ended, the process went on)"
        else
            echo "unwind-$build  FAIL (exit $rc: 134 is an abort inside the unwinder)"
        fi
    done
fi
if [ "$what" = hose ] || [ "$what" = all ]; then
    echo "## hose: the game's dump replayed at 2560x1440, the generated picture against the native build's"
    pic() { ls "$root/$1"/dump-*-"$2".ppm 2>/dev/null | head -1; }
    TEST_SIZE=2560x1440 TEST_DUMP=6 one hose-native "$native" 12 "" --replay "$replay"; line hose-native
    same=; try=0
    for try in 1 2 3 4 5 6; do
        TEST_SIZE=2560x1440 TEST_DUMP=6 one hose-portable "$portable" 12 "" --replay "$replay"; line hose-portable
        same=$( [ -n "$(pic hose-portable real)" ] && [ -n "$(pic hose-native real)" ] &&
            python3 -I "$project/tools/ppmdiff.py" "$(pic hose-portable real)" "$(pic hose-native real)" | awk '{ print ($2 + 0 < 0.05) ? "yes" : "no" }')
        [ "$same" = yes ] && break
    done
    echo "real pictures the same: ${same:-no dump} (try $try)"
    if [ -n "$(pic hose-portable generated)" ] && [ -n "$(pic hose-native generated)" ]; then
        echo "generated, portable against native: $(python3 -I "$project/tools/ppmdiff.py" "$(pic hose-portable generated)" "$(pic hose-native generated)")"
        echo "generated against the dump's own (the game's run): $(python3 -I "$project/tools/ppmdiff.py" "$(pic hose-portable generated)" "$replay-generated.ppm") portable, $(python3 -I "$project/tools/ppmdiff.py" "$(pic hose-native generated)" "$replay-generated.ppm") native"
    else
        echo "generated: no dump"
    fi
fi
pgrep -a weston | grep -- "--socket=$socket" && echo "NOTE: a weston on $socket is still running"
exit 0
