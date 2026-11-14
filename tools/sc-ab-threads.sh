#!/usr/bin/env bash
# Two placements of the game's threads compared within one session of the game: switched back
# and forth from outside (tools/sc-threads; the game gets no input and nothing in it is
# touched), the frame rate of each taken from the frame-generation layer's event log.
#
#   tools/sc-ab-threads.sh A B [SECONDS=20] [CYCLES=4]
#   A, B:  asis       as the threads are placed now (Wine's CPU sets and sc-hybrid)
#          all        every thread on every CPU (no placement: stock Wine without sc-hybrid)
#          own-core   as now, but the main thread has one P-core to itself
#          bg-low     as now, but the game's background workers (loading, streaming) give way to
#                     everything else: nice 19 (SC_BG_NICE: another value)
#          all-bg-low both: every thread on every CPU, background workers low
#          main-p     every thread on every CPU, but the main thread on the P-cores only
#   e.g. tools/sc-ab-threads.sh asis all         is the placement there is worth anything?
#        tools/sc-ab-threads.sh asis own-core
#
# The game has to be launched with frame generation (sc-turbo fg ...): that layer is the frame
# counter. Generation is switched off for the test, so that the game runs at its own speed, and
# back on after it (KEEP_FG=1: left as it is; then only a game the layer does not hold to a rate
# shows a difference). sc-hybrid is paused meanwhile, and at the end, however it ends, the
# threads are put back as they were and sc-hybrid goes on.
#
# Hold a steady view for the whole test: about 2 x SECONDS x CYCLES seconds.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
a="${1:?A: asis | all | own-core | bg-low | all-bg-low | main-p}"; b="${2:?B: asis | all | own-core | bg-low | all-bg-low | main-p}"; secs="${3:-20}"; cycles="${4:-4}"
comm="${SC_COMM:-StarCitizen.exe}"   # (another process, to try this out)
dir="${FG_DIR:-$here/logs/fg}"
pgrep -x "${comm:0:15}" >/dev/null || { echo "$comm is not running"; exit 1; }
snap="$(mktemp --suffix=.json)"
"$here/tools/sc-threads" --comm "$comm" save "$snap" || exit 1
placer="$(pgrep -f 'python3 .*sc-hybrid run' | tr '\n' ' ')"
was_off=; [ -e "$dir/OFF" ] && was_off=1
back() {
    "$here/tools/sc-threads" --comm "$comm" restore "$snap"
    # shellcheck disable=SC2086
    [ -n "$placer" ] && kill -CONT $placer 2>/dev/null
    [ -z "${KEEP_FG:-}" ] && [ -z "$was_off" ] && rm -f "$dir/OFF" && echo "generation back on"
    rm -f "$snap"
}
trap back EXIT
trap 'exit 1' INT TERM
# shellcheck disable=SC2086
[ -n "$placer" ] && kill -STOP $placer && echo "sc-hybrid paused"
if [ -z "${KEEP_FG:-}" ] && [ -z "$was_off" ]; then touch "$dir/OFF"; echo "generation off for the test"; sleep 3; fi
cmd() { # what a side does
    case "$1" in
    asis) echo "cmd: tools/sc-threads --comm '$comm' restore '$snap'" ;;
    all|main-p) echo "cmd: tools/sc-threads --comm '$comm' $1 '$snap'" ;;
    own-core) echo "cmd: tools/sc-threads --comm '$comm' own-core '$snap' ${SC_CORE:+--core $SC_CORE}" ;;
    bg-low|all-bg-low) echo "cmd: tools/sc-threads --comm '$comm' ${SC_BG:+--bg $SC_BG} ${SC_BG_NICE:+--nice $SC_BG_NICE} $1 '$snap'" ;;
    *) echo "unknown placement: $1" >&2; exit 1 ;;
    esac
}
ca="$(cmd "$a")" || exit 1; cb="$(cmd "$b")" || exit 1
echo "A = $a, B = $b: $cycles times $secs s each"
FG_DIR="$dir" "$here/tools/fg-ab.sh" "$ca" "$cb" "$secs" "$cycles" | sed -e "s|^A (cmd:[^)]*)|A ($a)|" -e "s|^B (cmd:[^)]*)|B ($b)|"
