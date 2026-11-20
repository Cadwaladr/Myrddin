#!/usr/bin/env python3
"""Kernel and CPU switches that can be changed while the game runs, for an A/B inside one session.

Root only. Nothing is attached to the game and nothing of it is read or written: these are switches of
the machine. Every one is put back when this helper ends (stop file, timeout, a signal) and none
outlasts a reboot. The switches ("levers"):

  pf=P[:E]      the CPU's hardware prefetchers, bits of the register MSR 0x1A4 through /dev/cpu/N/msr
                (hex; P for the P-cores, E for the E-cores; a set bit switches that prefetcher OFF;
                the same switches as the BIOS's "Hardware Prefetcher" and "Adjacent Cache Line
                Prefetch"). Only on the Intel CPUs whose bits are known here (Alder Lake, Raptor Lake).
                The kernel marks itself "tainted" (flag S, CPU out of specification) at the first write
                to such a register from outside it, until the next boot: a note in its log, nothing else.
  preempt=MODE  the kernel's preemption model, /sys/kernel/debug/sched/preempt (full, lazy, voluntary,
                none: those this kernel offers). Under "lazy" a thread that is woken does not push a
                running one off its CPU at once; under "full" it does.

    sudo tools/base-ab-root.py status
        what every lever is set to now

    sudo tools/base-ab-root.py serve DIR SETTINGS [HOURS=4]
        save every lever named in SETTINGS, then wait. SETTINGS is the list of states this run may
        apply, in order, separated by commas. A state is "base" (everything as it was found) or
        levers joined by "+": "base,pf=2f:2d,base,preempt=full,base,pf=2:0+preempt=full".
        Each time the file DIR/step appears, the next state of the list is applied (round and round;
        levers the state does not name go back to what was found) and DIR/step is removed;
        DIR/restore puts everything back and starts the list again; DIR/stop, the end of HOURS, or a
        signal puts everything back and ends. What was applied when is appended to DIR/ab-timeline.txt.

tools/base-ab.py (not root) gives the steps in time with the frame-rate lines of the layer's log.
AB_FAKE=DIR: stand-ins for the register and the kernel's file, for testing without root.
"""
import glob
import os
import re
import signal
import sys
import time

FAKE = os.environ.get("AB_FAKE")


def cpulist(text):
    out = []
    for part in text.strip().split(","):
        a, _, b = part.partition("-")
        if a.strip():
            out.extend(range(int(a), int(b or a) + 1))
    return out


class Prefetch:
    """MSR 0x1A4. Bits (Intel SDM vol. 4 for Raptor Lake; Intel's "Hardware Prefetch Controls for Intel
    Atom Cores"): 0 L2 streamer, 1 L2 adjacent line (P-cores only), 2 L1 next line, 3 L1 IP stride,
    5 L2 AMP. The E-cores' other tuning registers (0x1320...) are left alone."""
    name = "pf"
    MSR = 0x1A4
    ALLOWED = {"P": 0x2F, "E": 0x2D}
    MODELS = {0x97, 0x9A, 0xB7, 0xBA, 0xBF}   # Alder Lake S/P, Raptor Lake S/P/S-refresh

    def __init__(self):
        try:
            ecores = set(cpulist(open("/sys/devices/cpu_atom/cpus").read()))
        except OSError:
            ecores = set()
        self.kind = {c: ("E" if c in ecores else "P") for c in cpulist(open("/sys/devices/system/cpu/online").read())}
        self.saved = None

    def path(self, cpu):
        return os.path.join(FAKE, f"msr-{cpu}") if FAKE else f"/dev/cpu/{cpu}/msr"

    def why_not(self):
        info = open("/proc/cpuinfo").read()
        vendor = re.search(r"vendor_id\s*:\s*(\S+)", info)
        family = re.search(r"cpu family\s*:\s*(\d+)", info)
        model = re.search(r"^model\s*:\s*(\d+)", info, re.M)
        if not (vendor and vendor[1] == "GenuineIntel" and family and family[1] == "6" and model and int(model[1]) in self.MODELS):
            return "the prefetcher bits of this CPU model are not known to this tool"
        if FAKE:
            for c in self.kind:
                if not os.path.exists(self.path(c)):
                    with open(self.path(c), "wb") as f:
                        f.truncate(self.MSR + 8)
        elif not glob.glob("/dev/cpu/*/msr"):
            return "no /dev/cpu/N/msr (the msr driver is not there)"
        return None

    def rd(self, cpu):
        fd = os.open(self.path(cpu), os.O_RDONLY)
        try:
            return int.from_bytes(os.pread(fd, 8, self.MSR), "little")
        finally:
            os.close(fd)

    def wr(self, cpu, value):
        fd = os.open(self.path(cpu), os.O_WRONLY)
        try:
            os.pwrite(fd, value.to_bytes(8, "little"), self.MSR)
        finally:
            os.close(fd)

    def parse(self, text):
        p, _, e = text.partition(":")
        p = int(p, 16)
        e = int(e, 16) if e else p & self.ALLOWED["E"]
        if p & ~self.ALLOWED["P"] or e & ~self.ALLOWED["E"]:
            raise ValueError(f"only bits {self.ALLOWED['P']:#x} (P-cores) and {self.ALLOWED['E']:#x} (E-cores) may be set")
        return {"P": p, "E": e}

    def save(self):
        self.saved = {c: self.rd(c) for c in self.kind}

    def apply(self, value):
        for c, k in self.kind.items():
            self.wr(c, (self.saved[c] & ~self.ALLOWED[k]) | value[k])

    def restore(self):
        for c, v in self.saved.items():
            self.wr(c, v)

    def show(self, value=None):
        if value is not None:
            return f"pf=P:{value['P']:#x},E:{value['E']:#x}"
        by = {}
        for c in sorted(self.kind):
            by.setdefault((self.kind[c], self.rd(c)), []).append(c)
        return "; ".join(f"{k}-cores {cpus[0]}-{cpus[-1]}: MSR 0x1A4 = {v:#x}" for (k, v), cpus in sorted(by.items()))


class Preempt:
    name = "preempt"
    KNOWN = ("none", "voluntary", "full", "lazy")

    def __init__(self):
        self.file = os.path.join(FAKE, "preempt") if FAKE else "/sys/kernel/debug/sched/preempt"
        self.saved = None

    def why_not(self):
        if FAKE and not os.path.exists(self.file):
            with open(self.file, "w") as f:
                f.write("none voluntary full (lazy)\n")
        return None if os.path.exists(self.file) else f"no {self.file} (a kernel without dynamic preemption, or debugfs not mounted)"

    def read(self):
        words = open(self.file).read().split()
        now = next((w.strip("()") for w in words if w.startswith("(")), None)
        return now, [w.strip("()") for w in words]

    def write(self, mode):
        if FAKE:
            _, offered = self.read()
            with open(self.file, "w") as f:
                f.write(" ".join(f"({w})" if w == mode else w for w in offered) + "\n")
            return
        with open(self.file, "w") as f:
            f.write(mode)

    def parse(self, text):
        _, offered = self.read()
        if text not in self.KNOWN or text not in offered:
            raise ValueError(f"this kernel offers: {' '.join(offered)}")
        return text

    def save(self):
        self.saved, _ = self.read()
        if self.saved not in self.KNOWN:
            raise ValueError(f"cannot tell the mode in force from {self.file}")

    def apply(self, value):
        self.write(value)

    def restore(self):
        self.write(self.saved)

    def show(self, value=None):
        return f"preempt={value}" if value is not None else f"preempt: {self.read()[0]}"


LEVERS = {cls.name: cls for cls in (Prefetch, Preempt)}


def status():
    for cls in LEVERS.values():
        lever = cls()
        problem = lever.why_not()
        print(f"{lever.name}: not available ({problem})" if problem else lever.show())


def serve(directory, settings, hours):
    levers, plan = {}, []
    for item in (s.strip() for s in settings.split(",")):
        state = {}
        if item != "base":
            for part in item.split("+"):
                name, eq, text = part.partition("=")
                if not eq and re.fullmatch(r"[0-9a-fA-F]+(:[0-9a-fA-F]+)?", part):
                    name, text = "pf", part            # (a bare number is a prefetcher setting, as before)
                if name not in LEVERS:
                    sys.exit(f"setting {item}: no lever \"{name}\" (there are: {', '.join(LEVERS)})")
                if name not in levers:
                    lever = LEVERS[name]()
                    problem = lever.why_not()
                    if problem:
                        sys.exit(f"{name}: {problem}")
                    levers[name] = lever
                try:
                    state[name] = levers[name].parse(text)
                except ValueError as err:
                    sys.exit(f"setting {item}: {err}")
        plan.append((item, state))
    if not plan:
        sys.exit("no settings given")
    for lever in levers.values():
        lever.save()
    timeline = os.path.join(directory, "ab-timeline.txt")

    def note(text):
        line = f"{time.time():.3f} {text}"
        print(line, flush=True)
        try:
            fd = os.open(timeline, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o644)
            os.write(fd, (line + "\n").encode())
            os.close(fd)
        except OSError as err:
            print(f"cannot write {timeline}: {err}", flush=True)

    def restore():
        for lever in levers.values():
            lever.restore()

    def apply(state):
        for name, lever in levers.items():
            if name in state:
                lever.apply(state[name])
            else:
                lever.restore()

    def leave(*_):
        restore()
        note("restored (the end)")
        sys.exit(0)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, leave)
    for name in ("step", "restore", "stop"):
        try:
            os.unlink(os.path.join(directory, name))
        except OSError:
            pass
    note("found: " + " | ".join(lever.show() for lever in levers.values()) + f"; settings {settings}; waiting")
    until = time.time() + hours * 3600
    index = 0
    try:
        while time.time() < until:
            if os.path.lexists(os.path.join(directory, "stop")):
                break
            if os.path.lexists(os.path.join(directory, "restore")):
                os.unlink(os.path.join(directory, "restore"))
                restore()
                index = 0
                note("restored")
            if os.path.lexists(os.path.join(directory, "step")):
                item, state = plan[index % len(plan)]
                apply(state)
                os.unlink(os.path.join(directory, "step"))
                note(f"applied {index % len(plan)} {item}: " + (" ".join(levers[n].show(v) for n, v in state.items()) or "as found"))
                index += 1
            time.sleep(0.01)
    finally:
        restore()
        note("restored (the end)")


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("status", "serve"):
        print(__doc__)
        return 1
    if not FAKE and os.geteuid() != 0:
        sys.exit("run this through sudo")
    if sys.argv[1] == "status":
        status()
        return 0
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    serve(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 4.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
