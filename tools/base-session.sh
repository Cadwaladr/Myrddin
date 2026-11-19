#!/usr/bin/env bash
# One base-rate measurement in a running session of the game, in a busy place with a steady view:
# tools/base-probe (counters, stacks; no root) and one stretch of tools/base-trace-root.sh (scheduler
# trace, system calls, RAM traffic, power; started beforehand through sudo, it waits for the go).
#
#   tools/base-session.sh new [STRETCHES=3] [SECONDS=10]   make the folder; prints the sudo command to get approved
#   tools/base-session.sh early [STRETCHES=3] [SECONDS=10] the same before the game is launched: the approval is given
#                                                            first, so that no window opens over the starting game
#   tools/base-session.sh measure DIR N [PROBE_SECONDS=20]  stretch N: probe, then the trace, then the report
#   tools/base-session.sh trace DIR N                        only the trace of stretch N (no probe)
#
# Generation is off from the start of a stretch to its end (logs/fg/OFF, removed again only if this made
# it). About 2 x PROBE_SECONDS + 35 s of steady view for "measure", 15 s for "trace".
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
fg="${FG_DIR:-$here/logs/fg}"
comm="${SC_COMM:-StarCitizen.exe}"
pid_of() { for p in $(pgrep -x "${comm:0:15}"); do echo "$(ls "/proc/$p/task" 2>/dev/null | wc -l) $p"; done | sort -n | tail -1 | cut -d' ' -f2; }
case "${1:-}" in
new)
    pid="$(pid_of)"; [ -n "$pid" ] || { echo "$comm is not running"; exit 1; }
    dir="$here/logs/base/session-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$dir"
    echo "$pid" > "$dir/PID"
    echo "$dir"
    echo "sudo command: $here/tools/base-trace-root.sh $pid $dir ${3:-10} ${2:-3}"
    ;;
early)
    dir="$here/logs/base/session-$(date +%Y%m%d-%H%M%S)"; mkdir -p "$dir"
    echo "$dir"
    echo "sudo command: $here/tools/base-trace-root.sh wait $dir ${3:-10} ${2:-3}"
    ;;
measure|trace)
    dir="${2:?DIR}"; n="${3:?N}"; secs="${4:-20}"
    if [ ! -s "$dir/PID" ]; then pid="$(pid_of)"; [ -n "$pid" ] || { echo "$comm is not running"; exit 1; }; echo "$pid" > "$dir/PID"; fi
    [ -e "$dir/STATE" ] || { echo "the root trace has not been started for $dir (no STATE file)"; exit 1; }
    case "$(cat "$dir/STATE")" in waiting|done*) ;; *) echo "the root trace is not waiting: $(cat "$dir/STATE")"; exit 1 ;; esac
    made=
    if [ -d "$fg" ] && [ ! -e "$fg/OFF" ]; then touch "$fg/OFF"; made=1; echo "generation off"; sleep 3; fi
    back() { [ -n "$made" ] && rm -f "$fg/OFF" && echo "generation back on"; made=; }
    trap back EXIT
    trap 'exit 1' INT TERM
    echo "scheduler: $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || echo stock)"
    if [ "$1" = measure ]; then
        "$here/tools/base-probe" "$secs" --keep-fg --pid "$(cat "$dir/PID")" | tee "$dir/probe-$n.txt"
    fi
    touch "$dir/GO.$n"
    for i in $(seq 120); do [ "$(cat "$dir/STATE" 2>/dev/null)" = "done $n" ] || [ "$(cat "$dir/STATE" 2>/dev/null)" = finished ] && break; sleep 0.5; done
    back
    case "$(cat "$dir/STATE")" in "done $n"|finished) ;; *) echo "the trace of stretch $n did not finish: $(cat "$dir/STATE")"; exit 1 ;; esac
    echo "STEADY VIEW NO LONGER NEEDED"
    "$here/tools/base-critpath.py" "$dir/$n"
    ;;
*) sed -n 2,14p "$0"; exit 1 ;;
esac
