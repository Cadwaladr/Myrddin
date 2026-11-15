#!/usr/bin/env bash
# Runs a headless test only while the game is closed: the tests use the GPU the game is rendering
# on, which costs the game frames and makes the test's own numbers worthless. Not started when the
# game runs; ended at once (the whole process group, weston and the test program with it) when the
# game is started.
#
#   tools/while-game-closed.sh COMMAND [ARGUMENTS]      e.g.  tools/while-game-closed.sh tools/fg-validate.sh all
#
# Output goes to logs/test/_guarded.out. Exit: the command's own; 2 not started; 3 stopped.
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
[ $# -ge 1 ] || { sed -n '2,9p' "$0"; exit 1; }
running() { pgrep -x StarCitizen.exe > /dev/null; }
if running; then echo "the game is running: not started"; exit 2; fi
mkdir -p logs/test
setsid "$@" > logs/test/_guarded.out 2>&1 &
job=$!
while kill -0 "$job" 2> /dev/null; do
    if running; then
        kill -TERM -- "-$job" 2> /dev/null; sleep 1; kill -KILL -- "-$job" 2> /dev/null
        pkill -x test_chain; pkill -x weston
        echo "$(date +%H:%M:%S) STOPPED: the game was started"; exit 3
    fi
    sleep 2
done
wait "$job"; status=$?
echo "$(date +%H:%M:%S) ended ($status)"
tail -n 5 logs/test/_guarded.out | cut -c1-200
exit "$status"
