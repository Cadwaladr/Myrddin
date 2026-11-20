#!/usr/bin/env python3
"""Samples of the frame rate and the delay, taken while the game runs, for the page "Frame rates and latency":

    tools/fg-sample.py [set|now] [--seconds N] [--place NAME] [--note TEXT]

  set (default)  2×, 3× and 4× in turn, each held, N seconds each (default 60) once it has settled;
                 then the multiplier goes back to what the window has chosen
  now            what runs now, for N seconds
  --place NAME   where the sample is taken (otherwise: the place the game's log last named)

Each sample is a line of logs/research/fps-samples.jsonl. Nothing touches the game: the layer's own
log and the game's log are read, and the multiplier is set the way the window sets it (sc-turbo
fg-mult). The layer's display-timing reports are switched on (fg-params timing=1), so that the time
from a frame's present to the display is in the figures.
"""
import argparse
import configparser
import datetime
import glob
import importlib.machinery
import importlib.util
import json
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
RUNTIMES = (os.path.expanduser("~/.local/share/myrddin/runtime"), HERE)
SAMPLES = os.path.join(HERE, "logs", "research", "fps-samples.jsonl")
CITIES = (("Orison", "Orison"), ("Lorville", "Lorville"), ("Levski", "Levski"), ("Area18", "Area 18"), ("NewBabbage", "New Babbage"),
          # (the stations in low orbit, by the game's own names for them)
          ("RR_MIC_LEO", "Port Tressler"), ("RR_HUR_LEO", "Everus Harbor"), ("RR_CRU_LEO", "Seraphim Station"), ("RR_ARC_LEO", "Baijini Point"))
STAT = re.compile(r"\s*([\d.]+) STAT x(\d) ")


def module(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    found = importlib.util.module_from_spec(spec)
    loader.exec_module(found)
    return found


LATENCY = module("latency_report", os.path.join(HERE, "tools", "latency-report.py"))


def say(text):
    print(text, flush=True)


def session():
    """-> (runtime, the layer's log) of the game that runs now: the newest log, written in the last 30 s."""
    best = None
    for runtime in RUNTIMES:
        for path in glob.glob(os.path.join(runtime, "logs", "fg", "fg-*.log")):
            age = time.time() - os.path.getmtime(path)
            if age < 30.0 and (best is None or os.path.getmtime(path) > os.path.getmtime(best[1])):
                best = (runtime, path)
    return best


def tweaks():
    """What the advanced tweaks were while the sample was taken (docs/advanced-performance-tweaking.md)."""
    read = lambda path: open(path).read().strip() if os.path.exists(path) else ""
    state = read("/run/myrddin-tune/state").split()
    scheduler = read("/sys/kernel/sched_ext/root/ops") if read("/sys/kernel/sched_ext/state") == "enabled" else ""
    thp = re.search(r"\[(\w+)\]", read("/sys/kernel/mm/transparent_hugepage/enabled"))
    return {"scheduler": scheduler or "stock", "huge_pages": thp[1] if thp else "",
            "prefetchers": next((w.split("=")[1] for w in state if w.startswith("prefetch=")), "as booted")}


def game_up():
    return subprocess.run(["pgrep", "-x", "StarCitizen.exe"], capture_output=True).returncode == 0


def prefix_of(runtime):
    text = open(os.path.join(runtime, "sc-turbo.conf"), errors="replace").read() if os.path.exists(os.path.join(runtime, "sc-turbo.conf")) else ""
    m = re.search(r"^SC_PREFIX=(.+)$", text, re.M)
    prefix = m[1].strip().strip("'\"") if m else ""
    return prefix.replace("$HOME", os.path.expanduser("~")) or os.path.expanduser("~/Games/star-citizen")


def place_now(runtime):
    """-> (city or the game's own name for the place, the raw name, in the armistice zone or None)."""
    log = os.path.join(prefix_of(runtime), "drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/Game.log")
    raw, zone = None, None
    try:
        for line in open(log, errors="replace"):
            if "requested inventory for Location[" in line:
                m = re.search(r"Location\[([^\]]+)\]", line)
                if m and m[1] not in ("", "0", "Unknown"):
                    raw = m[1]
            elif "Added notification" in line and "Entering Armistice Zone" in line:
                zone = True
            elif "Added notification" in line and "Leaving Armistice Zone" in line:
                zone = False
    except OSError:
        return None, None, None
    city = next((name for key, name in CITIES if raw and key in raw), raw)
    return city, raw, zone


def stat_lines(path, start_size):
    """The ten-second lines the layer wrote after start_size bytes."""
    with open(path, errors="replace") as f:
        f.seek(start_size)
        text = f.read()
    return [line for line in text.splitlines() if STAT.match(line)]


def window(line):
    """One ten-second line, read as latency-report.py reads it."""
    w = {name: LATENCY.number(pattern, line) for name, pattern in LATENCY.FIELDS.items()}
    m = STAT.match(line)
    limit = re.search(r"limit (\d+) of (\d+)", line)
    w.update(time=float(m[1]), x=int(m[2]),
             held=bool(limit and int(limit[2]) and int(limit[1]) > 0.5 * int(limit[2])))
    for key in ("display", "display95", "generated_display"):
        w[key] = w[key] or None
    return w


def summary(part):
    """The middle values of the lines of one sample, and the delay worked out as latency-report.py does."""
    mid = lambda key: (statistics.median([w[key] for w in part if w.get(key) is not None])
                       if any(w.get(key) is not None for w in part) else None)
    age, ready, hook, display, real_out = (mid(k) for k in ("age", "ready", "hook", "display", "real_out"))
    spreading = hook - ready if hook is not None and ready is not None else None
    if real_out is not None:
        ready = min(ready or 0.0, real_out)
        spreading = real_out - ready
    total = sum(v for v in (age, ready, spreading, display) if v is not None)
    real = [w["real"] for w in part if w.get("real")]
    return {"real": mid("real"), "real_busy": sorted(real)[int(0.1 * (len(real) - 1))] if real else None,
            "shown": mid("shown"), "waiting": age, "generating": ready, "spreading": spreading, "to_display": display,
            "total": total if age is not None else None, "held_by_layer": sum(w["held"] for w in part) > len(part) / 2}


def sc_turbo(runtime, *words):
    out = subprocess.run([os.path.join(runtime, "sc-turbo"), *words], capture_output=True, text=True, timeout=20)
    return (out.stdout + out.stderr).strip().splitlines()[-1:] or [""]


def record(runtime, path, x, seconds, settle, args, place):
    """Waits for `settle` lines at x, then takes `seconds` worth of lines at x. -> the sample, or None."""
    want = max(1, round(seconds / 10.0))
    size = os.path.getsize(path)
    deadline = time.time() + (want + settle) * 10.0 + 40.0
    while time.time() < deadline:
        if not game_up():
            say("the game has closed: no sample")
            return None
        lines = [window(line) for line in stat_lines(path, size)]
        lines = [w for w in lines if w.get("real") and w["real"] >= 5.0 and w["x"] >= 2]
        ours = lines[settle:] if x is None else [w for w in lines[settle:] if w["x"] == x]
        if len(ours) >= want:
            part = ours[:want]
            most = max({w["x"] for w in part}, key=lambda n: sum(w["x"] == n for w in part))
            sample = {"taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                      "session": int(re.search(r"fg-(\d+)\.log", path)[1]), "x": most, "seconds": 10 * len(part),
                      "mixed": any(w["x"] != most for w in part),
                      "place": place[0], "place_raw": place[1], "armistice_zone": place[2], "note": args.note,
                      "tweaks": tweaks()}
            sample.update(summary(part))
            return sample
        time.sleep(1.0)
    say(f"no {want} ten-second lines at x{x} in time: no sample")
    return None


def chosen():
    """What the window has chosen: (mode, limit), to go back to after a set."""
    conf = configparser.ConfigParser()
    conf.read(os.path.expanduser("~/.config/myrddin/myrddin.conf"))
    general = conf["General"] if conf.has_section("General") else {}
    return general.get("mode", "auto"), general.get("limit", "4")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("what", nargs="?", default="set", choices=("set", "now"))
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--place")
    parser.add_argument("--note", default="")
    args = parser.parse_args()
    if not game_up():
        sys.exit("the game is not running")
    found = session()
    if not found:
        sys.exit("no layer log written in the last 30 s: is the game running with frame generation?")
    runtime, path = found
    city, raw, zone = place_now(runtime)
    place = (args.place or city, raw, zone)
    say(f"session {os.path.basename(path)} ({'AppImage runtime' if runtime == RUNTIMES[0] else 'this tree'}); "
        f"place: {place[0] or 'not known'}{'' if zone is None else (', in the armistice zone' if zone else ', outside the armistice zone')}")
    sc_turbo(runtime, "fg-params", "timing=1")
    off = os.path.exists(os.path.join(os.path.dirname(path), "OFF"))
    samples = []
    if args.what == "now":
        if off:
            sys.exit("generation is off in this session: nothing to sample")
        sample = record(runtime, path, None, args.seconds, 1, args, place)
        samples += [sample] if sample else []
    else:
        mode, limit = chosen()
        if off:
            sc_turbo(runtime, "fg-on")
        try:
            for x in (2, 3, 4):
                sc_turbo(runtime, "fg-mult", str(x), "fixed")
                say(f"x{x} held: settling, then {args.seconds} s")
                sample = record(runtime, path, x, args.seconds, 1, args, place)
                if not sample:
                    break
                samples.append(sample)
                say(f"  x{x}: game {sample['real']:.1f}/s, shown {sample['shown']:.0f}/s, delay {sample['total']:.0f} ms" if sample["total"] else
                    f"  x{x}: game {sample['real']:.1f}/s, shown {sample['shown']:.0f}/s")
        finally:
            if mode == "auto":
                back = sc_turbo(runtime, "fg-mult", limit, "auto")
            elif mode.isdigit():
                back = sc_turbo(runtime, "fg-mult", mode, "fixed")
            else:
                back = sc_turbo(runtime, "fg-off")
            if off and mode != "off":
                sc_turbo(runtime, "fg-off")
            say(f"back to the window's choice ({mode}{' up to x' + limit if mode == 'auto' else ''}): {back[0]}")
    if samples:
        os.makedirs(os.path.dirname(SAMPLES), exist_ok=True)
        with open(SAMPLES, "a") as f:
            for sample in samples:
                f.write(json.dumps(sample) + "\n")
        say(f"{len(samples)} sample(s) added to {os.path.relpath(SAMPLES, HERE)}")
    return 0 if samples else 1


if __name__ == "__main__":
    sys.exit(main())
