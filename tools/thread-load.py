#!/usr/bin/env python3
"""Where a process's threads run and how busy the CPUs are, sampled from /proc over a while (nothing is attached to
the process): per thread its CPU time as a share of one logical CPU and how much of that was on E-cores, per CPU
how busy it was.

  tools/thread-load.py PID [SECONDS=60]
"""
import os
import sys
import time

pid, secs = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
hz = os.sysconf("SC_CLK_TCK")


def cpus(path):
    try:
        out = set()
        for part in open(path).read().strip().split(","):
            a, _, b = part.partition("-")
            out.update(range(int(a), int(b or a) + 1))
        return out
    except OSError:
        return set()


e_cores = cpus("/sys/devices/cpu_atom/cpus")


def threads():
    d = {}
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return d
    for tid in tids:
        try:
            s = open(f"/proc/{pid}/task/{tid}/stat").read()
            f = s[s.rindex(")") + 2:].split()
            d[tid] = (s[s.index("(") + 1:s.rindex(")")], int(f[11]) + int(f[12]), int(f[36]))
        except (OSError, ValueError, IndexError):
            pass
    return d


def cores():
    out = {}
    for line in open("/proc/stat"):
        if line.startswith("cpu") and line[3].isdigit():
            p = line.split()
            v = list(map(int, p[1:]))
            out[int(p[0][3:])] = (sum(v), v[3] + v[4])
    return out


start, core_start, t0 = threads(), cores(), time.time()
last, on_e, total = dict(start), {}, {}
while time.time() - t0 < secs:
    time.sleep(0.25)
    now = threads()
    if not now:
        break
    for tid, (name, ticks, cpu) in now.items():
        spent = ticks - last.get(tid, (name, ticks, cpu))[1]
        total[tid] = total.get(tid, 0) + spent
        on_e[tid] = on_e.get(tid, 0) + (spent if cpu in e_cores else 0)
    last = now
dt = time.time() - t0
core_end = cores()
names = {tid: v[0] for tid, v in last.items()}
rows = sorted(((ticks / hz / dt * 100, 100 * on_e.get(tid, 0) / ticks if ticks else 0, names.get(tid, "?"))
               for tid, ticks in total.items() if ticks), reverse=True)
print(f"pid {pid}, {dt:.0f} s, {len(names)} threads; CPU time of all of them: {sum(r[0] for r in rows):.0f} % of one logical CPU"
      f" ({os.cpu_count()} logical CPUs)")
print("  busy %  of that on E-cores  thread")
for busy, e, name in rows[:24]:
    print(f"  {busy:6.1f}  {e:16.0f} %  {name}")
busy = {c: 100 * (1 - (core_end[c][1] - core_start[c][1]) / max(core_end[c][0] - core_start[c][0], 1)) for c in core_end}
print("each logical CPU busy, % (E-cores marked *):", " ".join(f"{c}{'*' if c in e_cores else ''}:{busy[c]:.0f}" for c in sorted(busy)))
