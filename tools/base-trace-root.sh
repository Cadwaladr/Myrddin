#!/usr/bin/env bash
# What the game's threads wait for, who wakes whom, which system calls they make, how much the RAM
# is used and what the CPU draws: a few seconds of the kernel's own records, for tools/base-critpath.py.
# Needs root (scheduler trace points, memory-controller and power counters), so it is run through sudo:
#
#   sudo tools/base-trace-root.sh PID OUT_DIR [SECONDS=10] [STRETCHES=1]
#   sudo tools/base-trace-root.sh wait OUT_DIR [SECONDS=10] [STRETCHES=1]
#
# "wait" in place of the PID: the game is not running yet. The approval is given first, the game is
# launched afterwards (so that no window opens over the starting game), and the script takes the game's
# process when the first go arrives.
#
# It only reads. Nothing is attached to the game (no ptrace, no probes in its code, its memory is not
# read) and no setting of the machine is changed: the kernel notes scheduler events for every CPU
# (which thread ran where, who woke whom), counts system calls of the game's threads at the kernel's
# entry point, and reads the memory controller's and the power meter's counters.
#
# Each stretch starts when the file OUT_DIR/GO.N appears (N = 1, 2, ...; made by tools/base-session.sh
# once the player stands still in a busy place), so the approval can be given beforehand. It gives up
# after 60 minutes without one, or when the game ends. The results go to OUT_DIR/N/, which has to be
# under this project's logs/base, and are handed to the user who called sudo.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
pid="${1:?PID of the game}"; out="${2:?OUT_DIR under logs/base}"; secs="${3:-10}"; stretches="${4:-1}"
[ "$(id -u)" = 0 ] || { echo "run this through sudo"; exit 1; }
comm="StarCitizen.exe"
later=; [ "$pid" = wait ] && { later=1; pid=0; }
for v in "$pid" "$secs" "$stretches"; do case "$v" in ''|*[!0-9]*) echo "PID (or the word wait), SECONDS and STRETCHES must be whole numbers"; exit 1 ;; esac; done
[ "$secs" -le 60 ] && [ "$stretches" -le 6 ] || { echo "at most 60 seconds and 6 stretches"; exit 1; }
[ -n "$later" ] || [ -d "/proc/$pid/task" ] || { echo "no process $pid"; exit 1; }
# the game's process: the one of that name with the most threads
game_pid() { for p in $(pgrep -x "$comm"); do echo "$(ls "/proc/$p/task" 2>/dev/null | wc -l) $p"; done | sort -n | tail -1 | cut -d' ' -f2; }
out="$(readlink -f "$out")"
case "$out" in "$here"/logs/base/*) ;; *) echo "OUT_DIR must be under $here/logs/base"; exit 1 ;; esac
mkdir -p "$out" || exit 1
owner="${SUDO_UID:-$(stat -c %u "$here")}:${SUDO_GID:-$(stat -c %g "$here")}"
hand_over() { chown -R "$owner" "$out"; chmod -R u+rwX,go+rX "$out"; }
trap hand_over EXIT
hand_over
echo "waiting" > "$out/STATE"; chown "$owner" "$out/STATE"

for n in $(seq "$stretches"); do
    waited=0
    echo "stretch $n of $stretches: waiting for $out/GO.$n"
    until [ -e "$out/GO.$n" ]; do
        sleep 0.5; waited=$((waited + 1))
        [ "$pid" = 0 ] || [ -d "/proc/$pid/task" ] || { echo "the game has ended"; echo "ended" > "$out/STATE"; exit 0; }
        [ "$waited" -lt 7200 ] || { echo "no go within 60 minutes"; echo "timeout" > "$out/STATE"; exit 0; }
    done
    if [ "$pid" = 0 ]; then
        pid="$(game_pid)"
        [ -n "$pid" ] || { echo "go given, but $comm is not running"; echo "ended" > "$out/STATE"; exit 0; }
        echo "the game's process: $pid"
    fi
    d="$out/$n"; mkdir -p "$d"
    echo "recording $n" > "$out/STATE"
    # the game's threads by name, as they are now
    for t in /proc/"$pid"/task/*; do printf '%s\t%s\n' "${t##*/}" "$(cat "$t/comm" 2>/dev/null)"; done > "$d/trace-threads.txt"
    {
        echo "kernel $(uname -r)  scheduler $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || echo stock)  date $(date '+%F %T')"
        echo "itmt $(cat /sys/kernel/debug/x86/sched_itmt_enabled 2>/dev/null)  preempt $(cat /sys/kernel/debug/sched/preempt 2>/dev/null)"
        echo "base_slice_ns $(cat /sys/kernel/debug/sched/base_slice_ns 2>/dev/null)  migration_cost_ns $(cat /sys/kernel/debug/sched/migration_cost_ns 2>/dev/null)"
        for c in /sys/devices/system/cpu/cpu[0-9]*; do printf '%s prio %s\n' "${c##*/}" "$(cat "/sys/kernel/debug/x86/sched_core_priority/${c##*/}" 2>/dev/null)"; done | tr '\n' ' '; echo
    } > "$d/trace-kernel.txt" 2>&1
    # 1. scheduler events of every CPU (timestamps on the monotonic clock)
    perf record -q -a -k CLOCK_MONOTONIC -e sched:sched_switch -e sched:sched_waking -o "$d/sched.data" -- sleep "$secs" > "$d/trace-sched.log" 2>&1 &
    p1=$!
    # 2. system calls of the game's threads, counted at the kernel's entry (per thread)
    calls="sched_yield futex ioctl getrusage mmap munmap mprotect madvise read pread64 write pwrite64 ppoll epoll_wait epoll_pwait clock_nanosleep nanosleep recvmsg sendmsg rt_sigprocmask"
    ev="raw_syscalls:sys_enter"; for c in $calls; do [ -d "/sys/kernel/tracing/events/syscalls/sys_enter_$c" ] && ev="$ev,syscalls:sys_enter_$c"; done
    perf stat --per-thread -p "$pid" -x ';' -e "$ev" -o "$d/syscalls.csv" -- sleep "$secs" > "$d/trace-syscalls.log" 2>&1 &
    p2=$!
    # 3. RAM traffic (memory controller) and package power, whole machine
    uev="power/energy-pkg/,power/energy-cores/"
    for u in /sys/bus/event_source/devices/uncore_imc_free_running*; do [ -d "$u" ] && uev="$uev,${u##*/}/data_read/,${u##*/}/data_write/"; done
    perf stat -a -x ';' -e "$uev" -o "$d/uncore.csv" -- sleep "$secs" > "$d/trace-uncore.log" 2>&1 &
    p3=$!
    wait "$p1" "$p2" "$p3"
    # 4. the CPU's power limits as set (and whether they are locked), one reading of clocks and watts
    turbostat --num_iterations 1 --interval 2 > "$d/turbostat.txt" 2>&1 || true
    hand_over
    echo "done $n" > "$out/STATE"; chown "$owner" "$out/STATE"
    echo "stretch $n recorded: $d"
done
echo "finished" > "$out/STATE"
echo "all done: $out"
