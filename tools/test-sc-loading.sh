#!/usr/bin/env bash
# Tests tools/sc-loading without the game: a scratch prefix with the RSI Launcher's log and a Game.log, a
# stand-in for the game's process (a copy of sleep under another name, SC_COMM), and no launcher
# (SC_LAUNCHER_PATTERN matches nothing). The follower runs from a copy in the scratch folder, so nothing of
# the project's logs is written.
#   tools/test-sc-loading.sh [WORK_DIR]        (default: a new folder under ${TMPDIR:-/tmp})
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
work="${1:-$(mktemp -d "${TMPDIR:-/tmp}/sc-loading-test.XXXXXX")}"
rm -rf "$work"; mkdir -p "$work/tree/tools" "$work/bin" "$work/fg"
cp "$here/tools/sc-loading" "$work/tree/tools/"
tool="$work/tree/tools/sc-loading"
fails=0
ok() { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }
wait_for() {   # wait_for SECONDS COMMAND...: until COMMAND succeeds
    local end=$((SECONDS + $1)); shift
    until "$@"; do [ $SECONDS -ge $end ] && return 1; sleep 0.2; done
}
export SC_COMM=fake-sc-game SC_LAUNCHER_PATTERN='[n]o-launcher-in-this-test'
cp "$(command -v sleep)" "$work/bin/fake-sc-game"
pids=()
trap 'for p in "${pids[@]}"; do kill "$p" 2> /dev/null; done' EXIT

echo "1. Which Game.log"
p="$work/prefix"; mkdir -p "$p/drive_c/users/u/AppData/Roaming/rsilauncher/logs" "$p/dosdevices"
ln -s ../drive_c "$p/dosdevices/c:"
[ "$("$tool" where "$p")" = "$p/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/Game.log" ]
check $? "no launcher log: the default LIVE folder"
launcher_log="$p/drive_c/users/u/AppData/Roaming/rsilauncher/logs/log.log"
printf '%s\n' '{ "t":"2026-11-21 13:54:00.000", "[main][info] ": "[Launcher::launch] Launching Star Citizen LIVE from (C:\\Program Files\\Roberts Space Industries\\StarCitizen\\LIVE)"  },' > "$launcher_log"
[ "$("$tool" where "$p")" = "$p/dosdevices/c:/Program Files/Roberts Space Industries/StarCitizen/LIVE/Game.log" ]
check $? "the folder the launcher started the game from, as its log writes it (doubled backslashes)"
printf '%s\n' '{ "t":"2026-11-21 14:00:00.000", "[main][info] ": "[Launcher::launch] Launching Star Citizen PTU from (C:\\Games\\SC\\PTU)"  },' >> "$launcher_log"
[ "$("$tool" where "$p")" = "$p/dosdevices/c:/Games/SC/PTU/Game.log" ]
check $? "the newest start counts (another channel, another folder)"
printf '%s\n' '{ "t":"2026-11-21 14:10:00.000", "[main][info] ": "[Launcher::launch] Launching Star Citizen LIVE from (C:\\Games\\SC\\LIVE)"  },' >> "$launcher_log"
live="$p/drive_c/Games/SC/LIVE"; mkdir -p "$live/logbackups"
printf '<2026-11-21T09:00:00.000Z> Log started on Sat Nov 21 09:00:00 2026\n<2026-11-21T09:00:01.000Z> [CGlobalGameUI::OpenLoadingScreen] Started\n' > "$live/Game.log"

echo "2. Following the game's loading screens"
"$tool" follow "$p" "$work/fg" >> "$work/loading.out" 2>&1 & follower=$!; pids+=("$follower")
sleep 1
[ ! -e "$work/fg/LOADING" ]; check $? "no LOADING before the game runs (the old log's open screen does not count)"
"$work/bin/fake-sc-game" 300 & game=$!; pids+=("$game")
wait_for 5 grep -q "following $p/dosdevices/c:/Games/SC/LIVE/Game.log" "$work/loading.out"; check $? "once the game runs it follows that Game.log (through the prefix's drive C:)"
sleep 0.5
[ ! -e "$work/fg/LOADING" ]; check $? "what the log held before is not read again"
echo '<2026-11-21T09:00:20.000Z> [CGlobalGameUI::OpenLoadingScreen] Started' >> "$live/Game.log"
wait_for 3 test -e "$work/fg/LOADING"; check $? "a loading screen opens: LOADING"
echo '<2026-11-21T09:00:21.000Z> [CGlobalGameUI::OpenLoadingScreen] Request context transition to LoadingScreenView' >> "$live/Game.log"
echo '<2026-11-21T09:01:25.000Z> Loading screen for pu : SC_Frontend closed after 64.26 seconds' >> "$live/Game.log"
wait_for 3 test ! -e "$work/fg/LOADING"; check $? "it closes: LOADING gone"
grep -q "the loading screen has gone (the game's log: after 64.26 seconds)" "$work/loading.out"; check $? "and the follower's log says so"
# the game starts a new log (the old one moved to logbackups): read from its first line
mv "$live/Game.log" "$live/logbackups/Game Build(1) 21 Nov 26 (09 00 00).log"
printf '<2026-11-21T09:05:00.000Z> Log started\n<2026-11-21T09:05:01.000Z> [CGlobalGameUI::OpenLoadingScreen] Started\n' > "$live/Game.log"
wait_for 5 test -e "$work/fg/LOADING"; check $? "a new Game.log is followed from its first line"
kill "$game"
wait_for 6 test ! -e "$work/fg/LOADING"; check $? "the game ends during a loading screen: LOADING let go"
wait_for 6 bash -c "! kill -0 $follower 2> /dev/null"; check $? "no launcher: the follower ends"
sleep 0.3
[ -z "$(pgrep -f "tail -F -n 0 $live/Game.log")" ]; check $? "and what read the log has ended with it"
[ ! -e "$work/tree/logs/loading.follow" ]; check $? "its owner note is gone"

echo "3. A newer launch's follower"
"$work/bin/fake-sc-game" 300 & game=$!; pids+=("$game")
"$tool" follow "$p" "$work/fg" >> "$work/loading.out" 2>&1 & follower=$!; pids+=("$follower")
wait_for 5 grep -q "\[$follower\] following" "$work/loading.out"; check $? "a second follower starts reading"
echo '<2026-11-21T09:06:00.000Z> [CGlobalGameUI::OpenLoadingScreen] Started' >> "$live/Game.log"
wait_for 3 test -e "$work/fg/LOADING"; check $? "LOADING again"
echo 99999999 > "$work/tree/logs/loading.follow"
wait_for 5 bash -c "! kill -0 $follower 2> /dev/null"; check $? "a newer one is named owner: this one ends"
[ -e "$work/fg/LOADING" ]; check $? "and leaves LOADING to the newer one"
grep -q "\[$follower\] a newer launch follows the loading screens now" "$work/loading.out"; check $? "saying why"
kill "$game" 2> /dev/null

echo "4. A loading screen whose end the log never has, and a follower whose game never comes"
rm -f "$work/fg/LOADING" "$work/tree/logs/loading.follow"
grep -q 'ge 600' "$tool"; check $? "a screen with no end is let go after 10 minutes (600 s; not waited for here)"
grep -q 'SECONDS -ge 1800' "$tool"; check $? "without the game and without a launcher it waits 30 minutes (not waited for here)"

echo
if [ "$fails" = 0 ]; then echo "all checks passed"; else echo "$fails check(s) FAILED (work folder: $work)"; fi
[ "$fails" = 0 ]
