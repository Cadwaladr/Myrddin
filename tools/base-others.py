#!/usr/bin/env python3
"""What else ran on the machine during a trace made by tools/base-trace-root.sh: the CPU time of
everything that is not the game, by thread name, and of the RSI Launcher's kind of threads (Chromium
names) in particular.

    tools/base-others.py DIR [TIDS_FILE]

DIR is one stretch (it holds sched.txt and trace-threads.txt, made by tools/base-critpath.py).
TIDS_FILE, if given, lists thread ids, one a line (first word), to be summed as "listed": for
instance the launcher's, taken while it runs:
    for p in $(pgrep -f '[R]SI Launcher'); do ls /proc/$p/task; done > FILE

Thread names tell programs apart only roughly: a browser's threads carry the same Chromium names as
the launcher's.
"""
import collections
import os
import re
import sys


def cpulist(text):
    out = set()
    for part in text.split(","):
        a, _, b = part.partition("-")
        if a.strip():
            out.update(range(int(a), int(b or a) + 1))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    d = sys.argv[1]
    try:
        ecores = cpulist(open("/sys/devices/cpu_atom/cpus").read().strip())
    except OSError:
        ecores = set()
    game = {int(line.split("\t")[0]) for line in open(os.path.join(d, "trace-threads.txt"))}
    listed = {int(line.split()[0]) for line in open(sys.argv[2]) if line.strip()} if len(sys.argv) > 2 else set()
    names, on = {}, {}
    first = last = None
    cpu_ns, e_ns = collections.Counter(), collections.Counter()

    def add(tid, cpu, ns):
        cpu_ns[tid] += ns
        if cpu in ecores:
            e_ns[tid] += ns

    for line in open(os.path.join(d, "sched.txt")):
        f = line.split()
        if f[0] == "N":
            names[int(f[1])] = " ".join(f[2:])
            continue
        t = int(f[1])
        first = t if first is None else first
        last = t
        if f[0] == "S":
            cpu, prev, nxt = int(f[2]), int(f[3]), int(f[5])
            cur = on.get(cpu)
            if prev:
                add(prev, cpu, t - (cur[1] if cur and cur[0] == prev else first))
            on[cpu] = (nxt, t)
    for cpu, (tid, since) in on.items():
        if tid:
            add(tid, cpu, last - since)
    secs = (last - first) / 1e9

    def cores(tids):
        return sum(cpu_ns[t] for t in tids) / 1e9 / secs

    others = [t for t in cpu_ns if t not in game]
    print(f"{secs:.1f} s; the game {cores([t for t in cpu_ns if t in game]):.2f} CPUs busy; everything else {cores(others):.2f}")
    by = collections.Counter()
    for t in others:
        by[names.get(t, "?")] += cpu_ns[t]
    print("everything else, by thread name (CPUs busy):")
    for n, v in by.most_common(14):
        print(f"  {v / 1e9 / secs:6.3f}  {n}")
    pat = re.compile(r"^(Cr|Chrome_|Compositor|VizCompositor|ThreadPool|Media$|RSI Launcher|GpuWatchdog|NetworkService|Electron)")
    chromium = collections.Counter()
    for t in others:
        if pat.match(names.get(t, "?")):
            chromium[names[t]] += cpu_ns[t]
    print(f"threads with Chromium names outside the game (the launcher, a browser): {sum(chromium.values()) / 1e9 / secs:.3f} CPUs busy")
    for n, v in chromium.most_common(6):
        print(f"  {v / 1e9 / secs:6.4f}  {n}")
    if listed:
        seen = [t for t in cpu_ns if t in listed]
        total = sum(cpu_ns[t] for t in seen)
        print(f"listed threads: {len(seen)} seen, {total / 1e9 / secs:.3f} CPUs busy, {100 * sum(e_ns[t] for t in seen) / max(total, 1):.0f} % of it on E-cores")
    return 0


if __name__ == "__main__":
    sys.exit(main())
