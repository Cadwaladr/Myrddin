#!/usr/bin/env bash
# Two settings compared while the game runs: switched back and forth, each side's frames taken
# from the frame-generation layer's event log. The game gets no input. Switching within one
# session is the only comparison that holds: two sessions differ by more than most settings do
# (the server, the place, what else the GPU is doing).
#
#   tools/fg-ab.sh "A" "B" [SECONDS=20] [CYCLES=4]
#
# A and B are each either settings of the layer (name=value ..., what "sc-turbo fg-params"
# takes) or "cmd: COMMAND", a command run at every switch to that side (thread placement, a
# scheduler: anything that can be changed from outside).
#   e.g. tools/fg-ab.sh "gate=1 guard=1 first=1" "gate=0 guard=0 first=0"    the limiter: as it is, as it was
#        tools/fg-ab.sh "gpuwait=0" "gpuwait=1"
#        tools/fg-ab.sh "cmd: tools/sc-threads restore /tmp/t.json" "cmd: tools/sc-threads all"   (see sc-ab-threads.sh)
#
# Ends on A. Prints, per side: real frames a second (and per stretch), the frame time's median /
# 95th / 99th percentile / longest, the multiplier, frames lost / copied out late. The first
# seconds after every switch are left out. FG_DIR=folder: the layer's folder (default logs/fg;
# a headless test's folder to try this without the game).
#
# A side of real frames means something only where the game is not held to a rate: for the
# game's own speed (thread placement and the like) switch generation off for the test
# ("sc-turbo fg-off", and "fg-on" after), or the layer's limit ("sc-turbo fg-cap off").
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
dir="${FG_DIR:-$here/logs/fg}"
a="${1:?A: name=value ... or cmd: COMMAND}"; b="${2:?B: name=value ... or cmd: COMMAND}"; secs="${3:-20}"; cycles="${4:-4}"
if [ -z "${FG_DIR:-}" ] && ! pgrep -f 'StarCitizen\.exe' >/dev/null; then echo "StarCitizen.exe is not running"; exit 1; fi
log="$(ls -t "$dir"/fg-*.log 2>/dev/null | head -1)"
[ -n "$log" ] || { echo "no fg-*.log in $dir"; exit 1; }
marks="$(mktemp)"; trap 'rm -f "$marks"' EXIT
applied() { grep -c 'fg: frame chain ' "$log"; }
files() { ls "$dir"/events-*.txt 2>/dev/null | wc -l; }
stamp() { # the layer's clock now: the time of the last present in a fresh copy of its event log
    local n i
    n=$(files)
    touch "$dir/EVENTS"
    for i in $(seq 50); do [ "$(files)" -gt "$n" ] && break; sleep 0.1; done
    [ "$(files)" -gt "$n" ] || return 1
    sleep 0.3 # (written in one go: complete by now)
    tail -1 "$(ls -t "$dir"/events-*.txt | head -1)" | awk '{ print $1 }'
}
switch() { # "what": the settings sent and read by the layer, or the command run
    case "$1" in
    cmd:*)
        ( cd "$here" && bash -c "${1#cmd:}" ) > /dev/null || { echo "failed: ${1#cmd:}"; return 1; } ;;
    *)
        local before i; before=$(applied)
        # shellcheck disable=SC2086
        printf '%s\n' $1 > "$dir/PARAMS.tmp" && mv "$dir/PARAMS.tmp" "$dir/PARAMS"
        for i in $(seq 40); do [ "$(applied)" -gt "$before" ] && break; sleep 0.1; done
        [ "$(applied)" -gt "$before" ] || { echo "the layer did not read PARAMS (is the game presenting?)"; return 1; } ;;
    esac
}
side() { # SIDE "what": the moment noted (before the switch: what follows is this side's), then switched
    local t; t=$(stamp) || { echo "the layer wrote no event log (is the game presenting?)"; return 1; }
    echo "$1 $t" >> "$marks"
    switch "$2"
}
started="$(date +%s)"
failed=
for c in $(seq "$cycles"); do
    for s in A B; do
        [ "$s" = A ] && p="$a" || p="$b"
        side "$s" "$p" || { failed=1; break 2; }
        sleep "$secs"
    done
done
side end "$a" || failed=1 # (back on A whatever happened)
[ -z "$failed" ] || { switch "$a"; exit 1; }
python3 - "$marks" "$dir" "$started" "$a" "$b" <<'EOF'
import sys, glob, os, statistics as st
SKIP = 1.5 # seconds after a mark that are not counted: the switch itself comes ~0.4 s after it
marks = [l.split() for l in open(sys.argv[1])]
marks = [(m[0], float(m[1])) for m in marks]
rows = {}
for f in glob.glob(os.path.join(sys.argv[2], "events-*.txt")):
    if os.path.getmtime(f) < float(sys.argv[3]) - 1: continue
    for line in open(f):
        if line.startswith('#'): continue
        p = [x.strip() for x in line.split('|')]
        if len(p) < 5: continue
        try: rows[float(p[0])] = p[4].split()
        except ValueError: continue
times = sorted(rows)
sides = {"A": [], "B": []}
for (side, t0), (_, t1) in zip(marks, marks[1:]):
    ts = [t for t in times if t0 + SKIP <= t < t1]
    if len(ts) < 3: continue
    per = [(y - x) * 1000 for x, y in zip(ts, ts[1:])]
    flags = "".join(rows[t][-1] for t in ts)
    sides[side].append(dict(fps=(len(ts) - 1) / (ts[-1] - ts[0]), per=per, lost=flags.count("X"), late=flags.count("O"),
                            mult=st.mode(rows[t][0] for t in ts), n=len(ts)))
def pct(v, q): return sorted(v)[min(int(len(v) * q), len(v) - 1)]
for side, label in (("A", sys.argv[4]), ("B", sys.argv[5])):
    s = sides[side]
    if not s: print(f"{side}: no frames"); continue
    per = [x for seg in s for x in seg["per"]]
    fps = [seg["fps"] for seg in s]
    print(f"{side} ({label}): real {st.mean(fps):.1f}/s (per stretch: {' '.join(f'{x:.1f}' for x in fps)}) | frame ms p50={pct(per, .5):.1f} "
          f"p95={pct(per, .95):.1f} p99={pct(per, .99):.1f} max={max(per):.1f} | {' '.join(sorted(set(seg['mult'] for seg in s)))} | "
          f"lost {sum(seg['lost'] for seg in s)} late {sum(seg['late'] for seg in s)} of {sum(seg['n'] for seg in s)} frames")
a, b = sides["A"], sides["B"]
if a and b:
    fa, fb = st.mean(x["fps"] for x in a), st.mean(x["fps"] for x in b)
    pairs = [y["fps"] / x["fps"] - 1 for x, y in zip(a, b)]
    # What the same side differs by from one stretch to the next is the noise: a difference
    # between the sides smaller than that says nothing.
    def spread(s): return max(abs(y["fps"] / x["fps"] - 1) for x, y in zip(s, s[1:])) if len(s) > 1 else 0.0
    print(f"B against A: {100 * (fb / fa - 1):+.1f} % real frames (stretch by stretch: {' '.join(f'{100 * p:+.1f}' for p in pairs)} %); "
          f"the same side from stretch to stretch: up to {100 * max(spread(a), spread(b)):.1f} %")
EOF
grep STAT "$log" | tail -1 | grep -o 'age ms p50=[0-9.]* p99=[0-9.]*' | sed 's/^/last statistics line: /'
