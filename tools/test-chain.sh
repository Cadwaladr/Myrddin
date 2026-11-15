#!/usr/bin/env bash
# Runs the two-device test (probe/test_chain.c: a stand-in for how the game presents through
# vkd3d-proton) under the frame-generation layer, in a headless compositor: nothing appears on the
# desktop. Uses build/fg/libsc_fg.new.so when there is one, else the installed layer.
#
#   tools/test-chain.sh NAME [SECONDS] ["ENV=.. ENV=.."] [test_chain options]
#
# Results in logs/test/NAME (layer log, events file, program output). What a build has to pass
# before it is installed, all of it in one go: tools/fg-validate.sh (and tools/fg-lock-test.sh for
# the state in which the present calls and the game slow each other down). Examples:
#   tools/test-chain.sh plain                          30 frames/s held, x2 on a 60 Hz target
#   tools/test-chain.sh dup 12 "" --dup 70             every 70th frame presented twice
#   tools/test-chain.sh nohud 12 "SC_FG_TEST_LAYER=1" --nohud 50    frames without a HUD capture
#   tools/test-chain.sh early 12 "" --early            a chain that is one off (the verifier's case)
#   tools/test-chain.sh stall 12 "" --stall 150 900    a 0.9 s hitch in the presenting thread
#   tools/test-chain.sh gpu 16 "SC_FG_TARGET=120 SC_FG_MULT=4 SC_FG_MAX_BASE=62" --game-ms 2 --gpu-load 20
#   tools/test-chain.sh slow 16 "SC_FG_TARGET=120 SC_FG_MULT=4 SC_FG_MAX_BASE=62 SC_FG_TEST_WAIT_MS=12" --game-ms 33
#   tools/test-chain.sh wild 12 "" --wild              a still stripe with vectors that say it moves (the vector check)
#   TEST_SIZE=2560x1440 TEST_DUMP=6 tools/test-chain.sh hose 12 "" --replay logs/fg/dump-236448-000
#                                                      a dump's pictures and inputs through the layer again
# The limiter (a game that does not protect its shared image, like the real one: --unprotected):
#   tools/test-chain.sh x4 20 "SC_FG_TARGET=120 SC_FG_MULT=4 SC_FG_TEST_WAIT_MS=7" --game-ms 5 --gpu-load 4 --unprotected
#                                                      a game faster than the present call can show its frames
#   tools/test-chain.sh jitter 20 "SC_FG_TARGET=120 SC_FG_MULT=4" --game-ms 12 --jitter 8 --gpu-load 4 --unprotected
#                                                      frame times that differ by up to 8 ms either way
#   tools/test-chain.sh spike 20 "SC_FG_TARGET=120 SC_FG_MULT=4" --game-ms 8 --spike 40 60 --gpu-load 4 --unprotected
#                                                      every 40th frame takes 60 ms longer
#   TEST_SIZE=2560x1440 tools/test-chain.sh real 20 "SC_FG_TARGET=120 SC_FG_MULT=3" --replay logs/fg/dump-236448-000 \
#       --game-ms 12 --jitter 6 --gpu-load 2 --unprotected       the interpolation at the game's own size
#   ... and the same with "SC_FG_GATE=0 SC_FG_GUARD=0 SC_FG_FIRST=0" added: the limiter as it was, to compare
#   (frames in the same time; GPU-bound cases differ by a few percent from run to run, so in turn and more than once)
# The multiplier's choice (SC_FG_MAX_BASE=62 switches it on, as in the game; --sim-ms: a game whose main
# thread is slower than the thread that hands frames over, so that holding it hides what it can do):
#   tools/test-chain.sh mult 40 "SC_FG_TARGET=120 SC_FG_MULT=4 SC_FG_MAX_BASE=62" --sim-ms 21 --sim-jitter 3 \
#       --game-ms 7 --jitter 1 --gpu-load 3 --unprotected          (then: grep multiplier logs/test/mult/fg-*.log)
#   ... --sim-dip 3 400 12 (short slow stretches), --sim-change 20 12 (the scene gets lighter after 20 s)
# TEST_LIB=path/libsc_fg.so: that build of the layer instead.
# TEST_DUMP=N: after N seconds the layer is asked for a dump (logs/test/NAME/dump-*).
# TEST_PANEL="STEP [tl|tr|bl|br]": the performance panel is on from the start (the control file PANEL).
# TEST_AT="N:FILE[=TEXT] ...": after N seconds the control file FILE is made (with TEXT in it), e.g.
#   TEST_PANEL=3 TEST_AT="5:PANELDUMP 7:OFF 9:PANELDUMP 10:PANEL=1" (the panel's part of the frames
#   as they go out: logs/test/NAME/paneldump-*; then of a frame that passes through; then step 1)
#   "pre:FILE": made before the program starts; "N:-FILE": removed after N seconds.
# TEST_SHOT="N ...": after N seconds a picture of the whole (headless) screen: logs/test/NAME/wayland-screenshot-*.png
# What to look at in the STAT lines: chain= (frames matched by the hand-over) with unlinked=0,
# shift=0 and no "FRAME CHAIN OFF" line (except with --early); ahead= near 0 when the limiter holds.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
name=${1:?name}; secs=${2:-12}; envs=${3:-}; shift; shift 2>/dev/null; shift 2>/dev/null
out="$here/logs/test/$name"
waits() { local t=$(( $(date +%s) + $1 )); until [ "$(date +%s)" -ge "$t" ]; do sleep 1; done; }
rm -rf "$out"; mkdir -p "$out/layer"
lib="$here/build/fg/libsc_fg.new.so"; [ -f "$lib" ] || lib="$here/build/fg/libsc_fg.so"
lib=${TEST_LIB:-$lib}   # another build of the layer, to compare
cp "$lib" "$out/layer/libsc_fg.so"; cp "$here/build/fg/sc_fg.json" "$out/layer/"
size=${TEST_SIZE:-1280x720}   # the headless output; a replay of a dump wants the dump's size
debug=; [ -n "${TEST_SHOT:-}" ] && debug=--debug   # (screen pictures are only allowed to a debug session's clients)
# shellcheck disable=SC2086
(weston --backend=headless --renderer=gl $debug --socket=scfg-test --width="${size%x*}" --height="${size#*x}" >/dev/null 2>&1 &)
waits 2
( waits $(( secs - 2 )); touch "$out/EVENTS" ) &
[ -n "${TEST_DUMP:-}" ] && ( waits $(( TEST_DUMP + 2 )); touch "$out/DUMP" ) &
[ -n "${TEST_PANEL:-}" ] && { echo "$TEST_PANEL" > "$out/PANEL"; export SC_FG_PANEL=1; }   # (the panel is off unless asked for)
for at in ${TEST_AT:-}; do
    file=${at#*:}; text=; case "$file" in *=*) text=${file#*=}; file=${file%%=*} ;; esac
    text=${text//,/ }   # (a comma for a space: PANEL=3,br)
    case "$at" in
    pre:*) echo "$text" > "$out/$file" ;;
    *:-*) ( waits $(( ${at%%:*} + 2 )); rm -f "$out/${file#-}" ) & ;;
    *) ( waits $(( ${at%%:*} + 2 )); echo "$text" > "$out/$file.tmp"; mv "$out/$file.tmp" "$out/$file" ) & ;;
    esac
done
for at in ${TEST_SHOT:-}; do
    ( waits $(( at + 2 )); cd "$out" && WAYLAND_DISPLAY=scfg-test weston-screenshooter >/dev/null 2>&1 ) &
done
# shellcheck disable=SC2086
env WAYLAND_DISPLAY=scfg-test DISPLAY= SC_FG=1 SC_FG_MATCH=test_chain SC_FG_TEST_INPUTS=1 SC_FG_TARGET=60 SC_FG_MULT=2 \
    SC_FG_MAX_BASE=0 SC_FG_DIR="$out" VK_ADD_IMPLICIT_LAYER_PATH="$out/layer" DISABLE_LSFGVK=1 $envs \
    timeout $(( secs + 15 )) "$here/build/test_chain/test_chain" "$secs" "$@" > "$out/out.txt" 2>&1
echo "exit $?"
wait
pkill -x weston; waits 1; pgrep -a weston || echo "weston gone"
tail -2 "$out/out.txt"
grep -v STAT "$out"/fg-*.log | grep -v 'instance created\|device created\|device destroyed\|swapchain:\|inputs:\|HUD pass ready\|fg: ready' | cut -c1-240
grep STAT "$out"/fg-*.log | tail -1
