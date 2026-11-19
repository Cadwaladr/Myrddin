#!/usr/bin/env python3
"""The frame's critical path, from a recording made by tools/base-trace-root.sh: of the time between two
presents, which threads was the frame really waiting on, on what kind of CPU did they run, and how
much of it was waiting for a CPU. Also the system calls the game's threads make, the RAM traffic and
the package power of the same ten seconds.

    tools/base-critpath.py DIR [--frames N] [--wakes-per-frame K]

How the path is found: a frame ends when a game thread wakes the thread that presents (vkd3d_queue).
The game may do that more than once a frame; how often is read from the rhythm of the gaps, and the
report's first lines say what was found. If its frame rate is not the one the game showed, give K.
From there the recording is walked backwards. While the thread on the path runs, the time is its
own. Where it had been asleep, the path goes on in the thread that woke it, at the moment of the
wake-up (that thread's work was what the frame waited for). Where it was pushed off its CPU or had
to wait for one after a wake-up, the time is the scheduler's. Where it was woken by an interrupt
(a timer, the disk, the network, the GPU), the wait is "outside". This is repeated back to the frame
before.

What it cannot see: a thread that waits by spinning counts as running (tools/base-probe gives the
share of spin-wait loops in each kind of thread).
"""
import bisect
import collections
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def cpulist(text):
    out = set()
    for part in text.split(","):
        a, _, b = part.partition("-")
        if a.strip():
            out.update(range(int(a), int(b or a) + 1))
    return out


ECORES = cpulist(read("/sys/devices/cpu_atom/cpus"))
SIBLING = {}
for d in os.listdir("/sys/devices/system/cpu"):
    if d[3:].isdigit():
        s = cpulist(read(f"/sys/devices/system/cpu/{d}/topology/thread_siblings_list")) - {int(d[3:])}
        if s:
            SIBLING[int(d[3:])] = min(s)


def group(name, tid, pid):
    if tid == pid:
        return "main thread"
    for prefix, label in (("BatchWorker", "BatchWorkers"), ("BackgroundWorke", "BackgroundWorkers"), ("Physics", "Physics"),
                          ("CAudioThread", "Audio"), ("vkd3d", "vkd3d"), ("sc_fg", "layer (sc_fg)"), ("Network", "Network")):
        if name.startswith(prefix):
            return label
    return "other game threads"


def dump(d):
    """The recording as short lines (cached in sched.txt)."""
    txt = os.path.join(d, "sched.txt")
    if not os.path.exists(txt) or os.path.getsize(txt) == 0:
        with open(txt, "w") as f:
            r = subprocess.run(["perf", "script", "-i", os.path.join(d, "sched.data"), "-s", os.path.join(HERE, "tools/base-sched-dump.py")],
                               stdout=f, stderr=subprocess.PIPE, text=True)
        if os.path.getsize(txt) == 0:
            sys.exit("perf script wrote nothing: " + r.stderr.strip()[:500])
    return txt


def other_reports(d, game, say):
    # --- system calls
    path = os.path.join(d, "syscalls.csv")
    if os.path.exists(path):
        import re
        per = collections.defaultdict(collections.Counter)
        secs = 0.0
        for line in open(path):
            f = line.rstrip("\n").split(";")
            m = re.match(r"(.*)-(\d+)$", f[0]) if len(f) > 4 else None
            if not m:
                continue
            try:
                n = float(f[1])
            except ValueError:
                continue
            tid = int(m.group(2))
            ev = f[3].replace("syscalls:sys_enter_", "").replace("raw_syscalls:sys_enter", "ALL")
            per[group(game.get(tid, m.group(1)), tid, PID)][ev] += n
            secs = max(secs, float(f[4]) / 1e9 if f[4] else 0)
        secs = SECS or secs or 10
        say()
        say("system calls a second, by kind of thread (counted at the kernel's entry)")
        cols = ["ALL", "futex", "sched_yield", "getrusage", "ioctl", "mmap", "munmap", "mprotect", "madvise", "read", "pread64", "write", "ppoll", "epoll_wait", "clock_nanosleep", "recvmsg", "sendmsg", "rt_sigprocmask"]
        cols = [c for c in cols if any(per[g][c] for g in per)]
        say(f"  {'threads':<20}" + "".join(f"{c[:10]:>11}" for c in cols))
        tot = collections.Counter()
        for g in ["main thread", "BatchWorkers", "Physics", "Audio", "BackgroundWorkers", "vkd3d", "Network", "layer (sc_fg)", "other game threads"]:
            if g in per:
                say(f"  {g:<20}" + "".join(f"{per[g][c] / secs:>11.0f}" for c in cols))
                tot.update(per[g])
        say(f"  {'all':<20}" + "".join(f"{tot[c] / secs:>11.0f}" for c in cols))
    # --- RAM traffic and power
    path = os.path.join(d, "uncore.csv")
    if os.path.exists(path):
        rd = wr = pkg = cores = 0.0
        for line in open(path):
            f = line.rstrip("\n").split(";")
            if len(f) < 4:
                continue
            try:
                v = float(f[0])
            except ValueError:
                continue
            if "data_read" in f[2]:
                rd += v
            elif "data_write" in f[2]:
                wr += v
            elif "energy-pkg" in f[2]:
                pkg = v
            elif "energy-cores" in f[2]:
                cores = v
        secs = SECS or 10
        say()
        say(f"RAM traffic (memory controller): read {rd / 1024 / secs:.1f} GB/s, written {wr / 1024 / secs:.1f} GB/s, together {(rd + wr) / 1024 / secs:.1f} GB/s")
        say(f"  (two channels of DDR4-2133 carry 34 GB/s at the very most, DDR4-3200 51 GB/s; real code seldom gets past two thirds of that)")
        say(f"package power: {pkg / secs:.0f} W (cores {cores / secs:.0f} W); the long-term limit is {int(read('/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw', '0')) / 1e6:.0f} W")
    for line in open(os.path.join(d, "turbostat.txt")) if os.path.exists(os.path.join(d, "turbostat.txt")) else []:
        if "MSR_PKG_POWER_LIMIT" in line or "PKG Limit" in line or "MSR_TURBO_RATIO_LIMIT" in line:
            say("  " + line.strip())


def main():
    global PID, SECS
    args = sys.argv[1:]
    nframes = None
    if "--frames" in args:
        i = args.index("--frames"); nframes = int(args[i + 1]); del args[i:i + 2]
    wakes_given = 0
    if "--wakes-per-frame" in args:
        i = args.index("--wakes-per-frame"); wakes_given = int(args[i + 1]); del args[i:i + 2]
    d = args[0]
    report = open(os.path.join(d, "critpath.txt"), "w")

    def say(text=""):
        print(text, flush=True)
        report.write(text + "\n")

    game = {}
    for line in open(os.path.join(d, "trace-threads.txt")):
        t, _, n = line.rstrip("\n").partition("\t")
        game[int(t)] = n
    PID = min(game)
    SECS = 0
    txt = dump(d)
    names = {}
    # per thread: when it was on a CPU, and its wake-ups; per CPU: when it was busy
    run = collections.defaultdict(list)      # tid -> [in, out, cpu, state it left in]
    wakes = collections.defaultdict(list)    # tid -> (t, waker, flags)
    busy = collections.defaultdict(list)     # cpu -> [in, out] of anything but idle
    occ = collections.defaultdict(list)      # cpu -> [in, out, tid]
    on = {}                                  # cpu -> (tid, since)
    t_first = t_last = None
    for line in open(txt):
        f = line.split()
        if f[0] == "N":
            names[int(f[1])] = " ".join(f[2:])
            continue
        t = int(f[1])
        t_first = t if t_first is None else t_first
        t_last = t
        if f[0] == "S":
            cpu, prev, state, nxt = int(f[2]), int(f[3]), int(f[4]), int(f[5])
            cur = on.get(cpu)
            if cur and cur[0] == prev:
                if prev:
                    run[prev].append((cur[1], t, cpu, state))
                    busy[cpu].append((cur[1], t))
                    occ[cpu].append((cur[1], t, prev))
            elif prev:
                run[prev].append((t_first, t, cpu, state))  # was already running when the recording began
                busy[cpu].append((t_first, t))
                occ[cpu].append((t_first, t, prev))
            on[cpu] = (nxt, t)
        elif f[0] == "W":
            wakes[int(f[4])].append((t, int(f[3]), int(f[6])))
    for cpu, (tid, since) in on.items():
        if tid:
            run[tid].append((since, t_last, cpu, -1))
            busy[cpu].append((since, t_last))
            occ[cpu].append((since, t_last, tid))
    SECS = (t_last - t_first) / 1e9
    run_start = {t: [r[0] for r in v] for t, v in run.items()}
    wake_t = {t: [w[0] for w in v] for t, v in wakes.items()}
    busy_start = {c: [b[0] for b in v] for c, v in busy.items()}

    def busy_time(cpu, a, b):
        v, s = busy.get(cpu), busy_start.get(cpu)
        if not v:
            return 0
        i = max(bisect.bisect_right(s, a) - 1, 0)
        total = 0
        while i < len(v) and v[i][0] < b:
            total += max(0, min(v[i][1], b) - max(v[i][0], a))
            i += 1
        return total

    # --- while a thread of the path waited for a CPU: what ran on the CPU it then got, and were others idle?
    all_cpus = sorted(set(busy) | set(SIBLING) | ECORES)
    wait_ns = [0]
    held_by = collections.Counter()          # who had the CPU the waiter got afterwards -> ns
    idle_cpu_ns = collections.Counter()      # "P" / "E" -> idle CPU time during the waits
    idle_whole = collections.Counter()       # waits during which a CPU of that kind stayed idle throughout -> ns

    def note_wait(x, w0, w1):
        if w1 <= w0:
            return
        st = run_start.get(x) or []
        k = bisect.bisect_left(st, w1)
        if k >= len(st) or st[k] != w1:
            return
        cpu = run[x][k][2]
        wait_ns[0] += w1 - w0
        v, s0 = occ.get(cpu, []), busy_start.get(cpu, [])
        i = max(bisect.bisect_right(s0, w0) - 1, 0)
        while i < len(v) and v[i][0] < w1:
            o = min(v[i][1], w1) - max(v[i][0], w0)
            if o > 0:
                tid = v[i][2]
                held_by[group(game[tid], tid, PID) if tid in game else "not the game: " + names.get(tid, "?")] += o
            i += 1
        whole = set()
        for c in all_cpus:
            b = busy_time(c, w0, w1)
            kind = "E" if c in ECORES else "P"
            idle_cpu_ns[kind] += (w1 - w0) - b
            if b == 0:
                whole.add(kind)
        if whole:
            idle_whole["P" if "P" in whole else "E"] += w1 - w0

    # --- the presenting thread: the vkd3d thread that game threads wake most evenly
    cand = [t for t, n in game.items() if n.startswith("vkd3d")]
    best = max(cand, key=lambda t: sum(1 for w in wakes.get(t, []) if w[1] in game and w[1] != t), default=None)
    if best is None or not wakes.get(best):
        say("no vkd3d thread is woken by game threads in this recording: cannot tell the frames apart")
        other_reports(d, game, say)
        return
    ends = [(w[0], w[1]) for w in wakes[best] if w[1] in game and w[1] != best]
    gaps = [b[0] - a[0] for a, b in zip(ends, ends[1:])]
    med = statistics.median(gaps) if gaps else 0
    # several wake-ups close together are one frame: keep the last of each cluster
    big = [g for g in gaps if g > 3e6]
    period = statistics.median(big) if big else med
    marks = []
    for i, e in enumerate(ends):
        if i + 1 == len(ends) or ends[i + 1][0] - e[0] > 0.35 * period:
            marks.append(e)
    # The game can hand the presenting thread work more than once a frame (seen: twice, a short gap and a
    # long one in turn). The gaps then repeat every K marks and not from one mark to the next: K is the
    # smallest step at which a gap is clearly more like the one K places on than like its neighbour.
    # Two hand-overs exactly half a frame apart cannot be told from two frames: --wakes-per-frame says it.
    mgaps = [b[0] - a[0] for a, b in zip(marks, marks[1:])]

    def unlike(k):
        diffs = [abs(a - b) for a, b in zip(mgaps, mgaps[k:])]
        return statistics.median(diffs) / statistics.median(mgaps) if diffs else 9.0

    per_frame_wakes, how = wakes_given, "as given"
    if not per_frame_wakes:
        per_frame_wakes, how = 1, "found from the gaps"
        if len(mgaps) > 40 and unlike(1) > 0.15:
            for k in (2, 3, 4):
                if unlike(k) < 0.5 * unlike(1):
                    per_frame_wakes = k
                    break
    # a frame ends at the hand-over that follows the longest of its gaps
    phase = 0
    if per_frame_wakes > 1:
        phase = max(range(per_frame_wakes), key=lambda ph: statistics.median(mgaps[ph::per_frame_wakes] or [0])) + 1
    frames = marks[phase % per_frame_wakes::per_frame_wakes]
    fgaps = [b[0] - a[0] for a, b in zip(frames, frames[1:])]
    say(f"recording: {SECS:.1f} s, {sum(len(v) for v in run.values())} stretches on a CPU, {sum(len(v) for v in wakes.values())} wake-ups")
    say(f"presenting thread: {names.get(best, game[best])} {best}, woken {len(ends)} times by game threads, in {len(marks)} hand-overs; "
        f"{per_frame_wakes} to a frame ({how}); frames taken: {len(frames)} "
        f"({len(fgaps) / ((frames[-1][0] - frames[0][0]) / 1e9) if fgaps else 0:.1f} a second, {statistics.median(fgaps) / 1e6 if fgaps else 0:.1f} ms apart at the median)")
    if nframes:
        frames = frames[:nframes + 1]

    # --- walk each frame backwards
    cat = collections.Counter()     # (kind, group) -> ns
    hops = []
    per_frame = []
    wake_lat = []
    for (t0, _), (t1, waker) in zip(frames, frames[1:]):
        x, t, n = waker, t1, 0
        guard = 0
        frame = collections.Counter()
        while t > t0 and guard < 20000:
            guard += 1
            rs, st = run.get(x), run_start.get(x)
            i = bisect.bisect_right(st, t) - 1 if st else -1
            if i < 0:  # not seen on a CPU before t: asleep since before the recording began
                a, b, cpu, state = t_first, t_first, -1, 1
            else:
                a, b, cpu, state = rs[i]
                if t <= b:  # on a CPU at t
                    a2 = max(a, t0)
                    g = group(game.get(x, names.get(x, "?")), x, PID) if x in game else "not the game"
                    if cpu in ECORES:
                        frame[("on an E-core", g)] += t - a2
                    else:
                        sib = SIBLING.get(cpu)
                        shared = busy_time(sib, a2, t) if sib is not None else 0
                        frame[("on a P-core, sharing it", g)] += shared
                        frame[("on a P-core alone", g)] += t - a2 - shared
                    t = a
                    if t <= t0:
                        break
                    i -= 1
                    a, b, cpu, state = rs[i] if i >= 0 else (t_first, t_first, -1, 1)
            # the thread was off the CPU from b to t
            g = group(game.get(x, names.get(x, "?")), x, PID) if x in game else "not the game"
            if state == 0 or state & 0x100:  # still able to run (pushed off): the scheduler's time
                frame[("waiting for a CPU (pushed off)", g)] += t - max(b, t0)
                note_wait(x, max(b, t0), t)
                t = b
                continue
            ws, wt = wakes.get(x), wake_t.get(x)
            j = bisect.bisect_right(wt, t) - 1 if wt else -1
            if j < 0 or ws[j][0] < b:
                frame[("asleep, wake-up not seen", g)] += t - max(b, t0)
                t = b
                continue
            tw, w, flags = ws[j]
            frame[("waiting for a CPU (after a wake-up)", g)] += t - max(tw, t0)
            note_wait(x, max(tw, t0), t)
            wake_lat.append(t - tw)
            irq = flags > 0 and flags & 0x18
            if w in game and w != x and not irq:
                x, t = w, tw  # the frame was waiting for the waker's work
                n += 1
                continue
            frame[("asleep until an interrupt or an outside process", g)] += max(0, min(tw, t) - max(b, t0))
            t = b
        cat.update(frame)
        hops.append(n)
        per_frame.append((t1 - t0, frame))
    total = sum(cat.values())
    if not total:
        say("no frame could be walked")
        other_reports(d, game, say)
        return
    say(f"walked {len(per_frame)} frames; a frame's path passes through {statistics.mean(hops):.0f} wake-ups on average (median {statistics.median(hops):.0f})")
    say()
    say("the frame's critical path: share of the time between two presents")
    kinds = collections.Counter()
    for (k, g), v in cat.items():
        kinds[k] += v
    for k, v in kinds.most_common():
        say(f"  {100 * v / total:>5.1f} %  {k}")
        sub = sorted(((vv, g) for (kk, g), vv in cat.items() if kk == k), reverse=True)
        for vv, g in sub[:6]:
            if vv / total >= 0.004:
                say(f"           {100 * vv / total:>5.1f}  {g}")
    by_group = collections.Counter()
    for (k, g), v in cat.items():
        if k.startswith("on a"):
            by_group[g] += v
    say()
    say("running time on the path, by kind of thread (share of the frame)")
    for g, v in by_group.most_common(8):
        say(f"  {100 * v / total:>5.1f} %  {g}")
    if wake_lat:
        wl = sorted(wake_lat)
        say()
        say(f"wake-up to running, on the path: median {wl[len(wl) // 2] / 1e3:.0f} us, 95th {wl[int(len(wl) * 0.95)] / 1e3:.0f} us, longest {wl[-1] / 1e3:.0f} us ({len(wl)} wake-ups)")
    if wait_ns[0]:
        w = wait_ns[0]
        say()
        say(f"while the path waited for a CPU ({100 * w / total:.1f} % of the frame looked at):")
        say(f"  CPUs idle at the same time, on average: {idle_cpu_ns['P'] / w:.1f} of {sum(1 for c in all_cpus if c not in ECORES)} P-core CPUs, "
            f"{idle_cpu_ns['E'] / w:.1f} of {len(ECORES)} E-cores")
        say(f"  share of the waiting during which some CPU stayed idle from its start to its end: {100 * (idle_whole['P'] + idle_whole['E']) / w:.0f} % "
            f"(a P-core CPU: {100 * idle_whole['P'] / w:.0f} %, only E-cores: {100 * idle_whole['E'] / w:.0f} %)")
        say("  what ran meanwhile on the CPU the waiting thread then got (share of the waiting):")
        for g, v in held_by.most_common(8):
            if v / w >= 0.01:
                say(f"    {100 * v / w:>5.1f} %  {g}")
        rest = w - sum(held_by.values())
        say(f"    {100 * rest / w:>5.1f} %  nothing: that CPU was idle (the thread had not been put there yet)")
    # --- what a perfect placement of the path would be worth (a bound, not a promise)
    e = kinds["on an E-core"]; sh = kinds["on a P-core, sharing it"]; al = kinds["on a P-core alone"]
    sched = kinds["waiting for a CPU (pushed off)"] + kinds["waiting for a CPU (after a wake-up)"]
    for label, re_, rs_ in (("E-core 0.55 and shared P-core 0.65 of a P-core alone", 0.55, 0.65), ("E-core 0.65 and shared P-core 0.75", 0.65, 0.75)):
        new = total - e * (1 - re_) - sh * (1 - rs_) - sched
        say()
        say(f"if every stretch on the path had a P-core to itself and never waited for a CPU ({label}):")
        say(f"  frame time x {new / total:.2f}, frame rate + {100 * (total / new - 1):.0f} %   (upper bound: the path would move to other threads before that)")
    other_reports(d, game, say)
    say()
    say(f"kept in {os.path.join(d, 'critpath.txt')}")


PID, SECS = 0, 0
if __name__ == "__main__":
    main()
