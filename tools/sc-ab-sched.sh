#!/usr/bin/env bash
# The kernel's CPU scheduler compared within one session of the game: the stock one against a
# sched_ext scheduler (tools/sc-sched), switched back and forth from outside; the game gets no
# input and nothing in it is touched. The frame rate of each side is taken from the
# frame-generation layer's event log (tools/fg-ab.sh).
#
#   tools/sc-ab-sched.sh SCHED [MODE=gaming] [SECONDS=25] [CYCLES=5]
#   e.g. tools/sc-ab-sched.sh lavd             scx_lavd in scx_loader's gaming mode (--performance) against stock
#        tools/sc-ab-sched.sh bpfland
#        tools/sc-ab-sched.sh stock            stock against stock: the noise (A/A)
#
# A = stock, B = SCHED. scxctl must work without a password (see "tools/sc-sched rule"). The game has
# to be launched with frame generation (that layer is the frame counter); generation is switched off
# for the test, so that the game runs at its own speed, and back on after it (KEEP_FG=1: left as it
# is). However the test ends, the scheduler that ran before it is put back (a game launched with
# sched=lavd goes on with scx_lavd). The first AB_SKIP seconds after every
# switch are not counted (default 3: starting a sched_ext scheduler moves every task once).
#
# REVERSE=1: SCHED is A (first in every cycle), stock B.
#
# Hold a steady view for the whole test: about 2 x SECONDS x CYCLES seconds.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
sched="${1:?SCHED: lavd | bpfland | cosmos | ... | stock}"; mode="${2:-gaming}"; secs="${3:-25}"; cycles="${4:-5}"
comm="${SC_COMM:-StarCitizen.exe}"   # (another process, to try this out)
dir="${FG_DIR:-$here/logs/fg}"
pgrep -x "${comm:0:15}" >/dev/null || { echo "$comm is not running"; exit 1; }
# What runs now ("running Lavd in Gaming mode" / "no scx scheduler running"): put back at the end.
before="$(scxctl get 2>/dev/null)"
case "$before" in
"running "*) set -- $before; prev="$(echo "$2" | tr '[:upper:]' '[:lower:]') $(echo "$4" | tr '[:upper:]' '[:lower:]')" ;;
*) prev=stock ;;
esac
"$here/tools/sc-sched" set stock >/dev/null || { echo "could not put the stock scheduler in place"; exit 1; }
was_off=; [ -e "$dir/OFF" ] && was_off=1
back() {
    # shellcheck disable=SC2086
    "$here/tools/sc-sched" set $prev >/dev/null && echo "scheduler back as it was: $("$here/tools/sc-sched" state)" ||
        echo "SCHEDULER NOT BACK ($prev wanted): $("$here/tools/sc-sched" state)"
    [ -z "${KEEP_FG:-}" ] && [ -z "$was_off" ] && rm -f "$dir/OFF" && echo "generation back on"
}
trap back EXIT
trap 'exit 1' INT TERM
if [ -z "${KEEP_FG:-}" ] && [ -z "$was_off" ]; then touch "$dir/OFF"; echo "generation off for the test"; sleep 3; fi
ca="cmd: tools/sc-sched set stock"
[ "$sched" = stock ] && cb="$ca" || cb="cmd: tools/sc-sched set $sched $mode"
[ "$sched" = stock ] && label=stock || label="$sched ($mode)"
if [ -n "${REVERSE:-}" ]; then # SCHED first in every cycle: what drifts over the test then favours stock
    echo "A = $label, B = stock: $cycles times $secs s each"
    AB_SKIP="${AB_SKIP:-3}" FG_DIR="$dir" "$here/tools/fg-ab.sh" "$cb" "$ca" "$secs" "$cycles" | sed -e "s|^A (cmd:[^)]*)|A ($sched)|" -e "s|^B (cmd:[^)]*)|B (stock)|"
else
    echo "A = stock, B = $label: $cycles times $secs s each"
    AB_SKIP="${AB_SKIP:-3}" FG_DIR="$dir" "$here/tools/fg-ab.sh" "$ca" "$cb" "$secs" "$cycles" | sed -e "s|^A (cmd:[^)]*)|A (stock)|" -e "s|^B (cmd:[^)]*)|B ($sched)|"
fi
