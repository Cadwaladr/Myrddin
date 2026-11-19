#!/usr/bin/env python3
"""Frame rates per city, the table in the README, from the layer's logs and the game's own log:

    tools/city-rates.py [PID ...]     (sessions, logs/fg/fg-PID.log; default: the last three)

Only the time on foot in a city counts. The game's log says where the player is when it asks for
the inventory of a place, and when a hangar was granted and the armistice zone left or entered:
  spawned in the city   from a minute after that, until the zone is left, a hangar is granted or
                        a quantum target is chosen, which is the way out
  landed there          from where the zone is entered again after the hangar, until the same
A stretch also ends where the game goes back to its front end (the menu, the character creator).
A line of the layer's log covers ten seconds and counts when all of them lie in such a stretch and
frames were generated in it (a line from a time with generation switched off is left out).
"typical" is the middle of those lines, "busy" the slowest tenth of them.
"""
import collections
import glob
import importlib.machinery
import importlib.util
import os
import re
import statistics
import struct
import sys

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
CITIES = (("Orison", "Orison"), ("Lorville", "Lorville"), ("Levski", "Levski"), ("Area18", "Area 18"), ("NewBabbage", "New Babbage"))
STAT = re.compile(r"\s*([\d.]+) STAT x(\d) real=([\d.]+)/s generated=([\d.]+)/s shown=([\d.]+)/s")
MARKS = (("Hangar Request Completed", "hangar"), ("Entering Armistice Zone", "enter"), ("Leaving Armistice Zone", "leave"))


def iowatch():
    path = os.path.join(HERE, "tools", "sc-iowatch")
    loader = importlib.machinery.SourceFileLoader("iowatch", path)
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader("iowatch", loader))
    loader.exec_module(module)
    return module


def layer_lines(pid):
    with open(os.path.join(HERE, "logs", "fg", f"THREADS-{pid}"), "rb") as f:
        start = struct.unpack_from("<8siId", f.read(32), 0)[3]
    lines = []
    for line in open(os.path.join(HERE, "logs", "fg", f"fg-{pid}.log"), errors="replace"):
        m = STAT.match(line)
        if m and float(m[3]) > 5.0 and float(m[4]) > 0.0:
            lines.append((start + float(m[1]), int(m[2]), float(m[3]), float(m[5])))
    return lines


def game_marks(io, path):
    places, marks = [], []
    for line in open(path, errors="replace"):
        m = io.STAMP.match(line)
        if not m:
            continue
        when = io.stamp(m[1])
        if "requested inventory for Location[" in line:
            place = re.search(r"Location\[([^\]]+)\]", line)[1]
            if place not in ("", "0", "Unknown") and (not places or places[-1][1] != place):
                places.append((when, place))
        if "Added notification" in line:
            marks += [(when, kind) for text, kind in MARKS if text in line]
        if "Player Selected Quantum Target" in line:
            marks.append((when, "quantum"))
        if 'gamerules="SC_Frontend"' in line:
            marks.append((when, "frontend"))
    return places, marks


def stretches(places, marks, last):
    """-> (city, start, stop) for the time on foot in each city."""
    for i, (when, place) in enumerate(places):
        city = next((name for key, name in CITIES if key in place), None)
        if not city:
            continue
        until = places[i + 1][0] if i + 1 < len(places) else last + 1.0
        here = [m for m in marks if when - 1.0 <= m[0] < until]
        landing = [t for t, kind in here if kind == "hangar" and t - when < 70.0]
        start = when + 60.0
        if landing:
            after = [m for m in here if landing[-1] <= m[0] < landing[-1] + 240.0]
            entered = [t for t, kind in after if kind == "enter" and any(k == "leave" and s <= t for s, k in after)]
            if not entered:
                continue
            start = [t for t in entered if t - entered[0] < 5.0][-1]
        stop = min([t for t, kind in here if t >= start + 60.0 and kind in ("leave", "hangar", "quantum")] + [until])
        stop = min([t for t, kind in marks if kind == "frontend" and t > start] + [stop])
        yield city, start, stop


def main():
    pids = sys.argv[1:]
    if not pids:
        logs = sorted(glob.glob(os.path.join(HERE, "logs", "fg", "fg-*.log")), key=os.path.getmtime)[-3:]
        pids = [re.search(r"fg-(\d+)\.log", p)[1] for p in logs]
    io = iowatch()
    found = collections.defaultdict(list)
    for pid in pids:
        lines = layer_lines(pid)
        path = io.game_log_for(lines[0][0] - 10.0) if lines else None
        if not path:
            print(f"{pid}: no log of the game for this session")
            continue
        places, marks = game_marks(io, path)
        for city, start, stop in stretches(places, marks, lines[-1][0]):
            found[city] += [line for line in lines if start + 10.0 <= line[0] <= stop]
    tenth = lambda values: sorted(values)[int(0.1 * (len(values) - 1))]
    print("| City | The game's own frames per second: typical (busy) | Shown with frame generation: typical (busy) |")
    print("|---|---|---|")
    for city, part in sorted(found.items(), key=lambda item: -len(item[1])):
        if len(part) < 6:
            continue
        real, shown = [p[2] for p in part], [p[3] for p in part]
        print(f"| {city} | {statistics.median(real):.0f} ({tenth(real):.0f}) | {statistics.median(shown):.0f} ({tenth(shown):.0f}) |")


if __name__ == "__main__":
    main()
