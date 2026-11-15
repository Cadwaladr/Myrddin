#!/usr/bin/env python3
"""What every thread was doing inside the frame-generation layer: reads the file the layer keeps
for that (THREADS-<pid> in the log directory), while the game runs or after it has ended. The
question it answers after a game that stood still: which thread was in which hook, since when,
holding which of the layer's locks and waiting for which.

    tools/fg-where.py                 the newest THREADS file in logs/fg
    tools/fg-where.py PID | FILE      that one
    tools/fg-where.py -a [...]        also the threads that are in no hook

Times are wall-clock. "line" is how far the thread had come in the hook: a line of fg/sc_fg.cpp
(900000 + n: the present hook's stage n, see Stage() there; 0: just entered).
"""
import glob
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SLOT = 64


def main():
    args = [a for a in sys.argv[1:] if a != "-a"]
    everything = "-a" in sys.argv[1:]
    directory = os.path.join(HERE, "logs", "fg")
    if args and os.path.isfile(args[0]):
        path = args[0]
    elif args:
        path = os.path.join(directory, "THREADS-" + args[0])
    else:
        files = sorted(glob.glob(os.path.join(directory, "THREADS-*")), key=os.path.getmtime)
        if not files:
            sys.exit("no THREADS file in " + directory)
        path = files[-1]
    data = open(path, "rb").read()
    magic, pid, slots, wall_at_zero = struct.unpack_from("<8siId", data, 0)
    if magic != b"SCFGTHR1":
        sys.exit(path + ": not a THREADS file")
    text = data[SLOT * slots:].split(b"\0")[0].decode("ascii", "replace")
    locks = text.split()[1:] if text.startswith("locks:") else []

    def clock(t):
        return time.strftime("%H:%M:%S", time.localtime(wall_at_zero + t)) + ("%.3f" % (t % 1.0))[1:]

    def lock_names(bits):
        names = [locks[i] if i < len(locks) else "lock%d" % i for i in range(32) if bits & (1 << i)]
        return "+".join(names) if names else "-"

    alive = os.path.isdir("/proc/%d" % pid)
    print("%s: process %d (%s), layer started %s" % (
        os.path.basename(path), pid, "running" if alive else "ended",
        time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(wall_at_zero))))
    stage = os.path.join(os.path.dirname(path), "STAGE-%d" % pid)
    if os.path.isfile(stage):
        words = open(stage, "rb").read().split(b"\0")[0].decode("ascii", "replace").split()
        if len(words) >= 4:
            print("present hook: stage %s since %s" % (words[1], clock(float(words[3]))))
    rows = []
    for i in range(1, slots):
        tid, waiting, held, step, calls, since, wait_since, name, lost = struct.unpack_from("<iiIIQdd16sQ", data, SLOT * i)
        if not tid:
            continue
        rows.append((since, tid, waiting, held, step, calls, wait_since, name.split(b"\0")[0].decode("ascii", "replace"), lost))
    inside = [r for r in rows if r[0] > 0.0]
    print("%d threads have called into the layer, %d are inside a hook%s" % (
        len(rows), len(inside), "" if alive else " (where they were when the process ended)"))
    newest = max([r[0] for r in rows] + [r[6] for r in rows] + [0.0])
    for since, tid, waiting, held, step, calls, wait_since, name, lost in sorted(rows, key=lambda r: (r[0] <= 0.0, r[0])):
        if since <= 0.0 and not everything and not held and not waiting and not lost:
            continue
        comm = ""
        if alive:
            try:
                comm = " (" + open("/proc/%d/task/%d/comm" % (pid, tid)).read().strip() + ")"
            except OSError:
                comm = " (gone)"
        where = "in %-16s since %s line %-6d" % (name, clock(since), step) if since > 0.0 else "in no hook (last: %s)" % name
        line = "  thread %d%s %s  holds %s" % (tid, comm, where, lock_names(held))
        if waiting:
            line += "  WAITS for %s since %s" % (locks[waiting - 1] if waiting - 1 < len(locks) else waiting, clock(wait_since))
        if lost:
            line += "  [%d call(s) never came back]" % lost
        print(line + "  (%d calls)" % calls)
    if newest > 0.0:
        print("newest time in the file: %s" % clock(newest))


if __name__ == "__main__":
    main()
