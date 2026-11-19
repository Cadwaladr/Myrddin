#!/usr/bin/env bash
# When frames reach the display, measured in the running game: asks the display about every frame for a
# while (sc-turbo fg-params timing=1), writes what became of them and prints tools/display-timing.py's reading.
# With "tearing", then the same again with KDE's "allow tearing" on for a while. That setting is put back as
# it was however this ends. The player stands still meanwhile.
#
#   tools/display-test.sh [SECONDS] [tearing [SECONDS]]        (default 170, and 90 with tearing on)
cd "$(dirname "$0")/.." || exit 1
secs=${1:-170}; with_tearing=${2:-}; tear_secs=${3:-90}
pid=$(pgrep -x StarCitizen.exe | head -n 1)
[ -n "$pid" ] || { echo "the game is not running"; exit 1; }
waits() { timeout "$1" tail -f /dev/null; }
newest() { ls -t logs/fg/presents-"$pid"-*.bin 2>/dev/null | head -n 1; }
dump() { # writes the record, prints its name
    local before now i
    before=$(newest)
    ./sc-turbo fg-events > /dev/null
    for i in $(seq 1 20); do
        waits 1
        now=$(newest)
        if [ -n "$now" ] && [ "$now" != "$before" ]; then
            waits 1
            echo "$now"
            return 0
        fi
    done
    return 1
}
was=$(kreadconfig6 --file kwinrc --group Compositing --key AllowTearing --default true)
changed=0
set_tearing() {
    kwriteconfig6 --file kwinrc --group Compositing --key AllowTearing --type bool "$1"
    busctl --user call org.kde.KWin /KWin org.kde.KWin reconfigure
}
finish() {
    if [ "$changed" = 1 ]; then
        set_tearing "$was"
        echo "tearing setting back to: $(kreadconfig6 --file kwinrc --group Compositing --key AllowTearing --default true)"
    fi
    pgrep -x StarCitizen.exe > /dev/null && ./sc-turbo fg-params timing=0 > /dev/null
}
trap finish EXIT
trap 'exit 1' INT TERM HUP

./sc-turbo fg-params timing=1 > /dev/null
echo "$(date +%H:%M:%S) measuring as things are, $secs s"
waits "$secs"
first=$(dump) || { echo "no record was written"; exit 1; }
if [ "$with_tearing" = tearing ]; then
    changed=1
    set_tearing true
    echo "$(date +%H:%M:%S) tearing allowed, $tear_secs s"
    waits "$tear_secs"
    second=$(dump) || { echo "no second record was written"; exit 1; }
    set_tearing "$was"
    changed=0
    echo "$(date +%H:%M:%S) tearing setting back to: $(kreadconfig6 --file kwinrc --group Compositing --key AllowTearing --default true)"
fi
./sc-turbo fg-params timing=0 > /dev/null
echo; echo "=== as things are"; python3 -B tools/display-timing.py "$first"
if [ -n "${second:-}" ]; then
    echo; echo "=== with tearing allowed"; python3 -B tools/display-timing.py --last $(( tear_secs - 6 )) "$second"
fi
