#!/usr/bin/env bash
# Provokes the limiter's slow state at the game's own size: a game the limiter has to hold, with
# slow stretches and hitches, while the multiplier is switched between x3 and x4 by hand every few
# seconds (every switch restarts what the layer knows about its own timing). A build that can lock
# into the slow state shows it as seconds at x4 with clearly fewer than 30 real frames (24-25), which
# then stay that way; one that cannot comes back to 30 after every switch.
#
#   tools/fg-lock-test.sh NAME [SECONDS] ["ENV=.."]      (TEST_LIB=path: that build)
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
name=${1:?name}; secs=${2:-70}; envs=${3:-}
( sleep 9; m=3; while [ -d "logs/test/$name" ] && pgrep -x test_chain > /dev/null; do echo $m > "logs/test/$name/MULT"; m=$(( 7 - m )); sleep 6; done ) &
TEST_SIZE=2560x1440 tools/test-chain.sh "$name" "$secs" "SC_FG_TARGET=120 SC_FG_MULT=4 $envs" --replay logs/fg/dump-236448-000 \
    --sim-ms 22 --sim-jitter 4 --sim-dip 7 500 15 --game-ms 8 --jitter 2 --gpu-load 4 --spike 400 120 --unprotected > "logs/test/_$name.out" 2>&1
wait
f=$(ls logs/test/$name/events-*.txt 2>/dev/null | head -1)
# per second: the multiplier, real frames, how long the generated frames took to be ready, how long frames waited for their hook
awk -F'|' 'NR > 1 { split($1, a, " "); t = a[1] + 0; if (!t0) t0 = t; b = int(t - t0); split($4, m, " "); split($5, x, " "); n[b]++; r[b] += m[7]; h[b] += m[1]; mult[b] = x[1]; if (b > mx) mx = b }
    END { for (b = 0; b <= mx; b++) if (n[b]) { slow = (mult[b] == "x4" && n[b] < 27) || (mult[b] == "x3" && n[b] < 33); bad += slow
              printf "%3d %s %2d frames  ready %5.1f ms  hand-over to hook %5.1f ms%s\n", b, mult[b], n[b], r[b] / n[b], h[b] / n[b], slow ? "  <-- slow" : "" }
          printf "slow seconds: %d of %d\n", bad, mx + 1 }' "$f" > "logs/test/$name/seconds.txt"
tail -1 "logs/test/$name/seconds.txt"; grep -c 'CHAIN OFF' logs/test/$name/fg-*.log | sed 's/^/chain-off lines: /'
pgrep -a weston || echo "no weston running"
