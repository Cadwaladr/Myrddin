#!/usr/bin/env bash
# Pictures of every page in the states that matter, drawn off-screen (no window appears anywhere).
#   tests/shots.sh MYRDDIN_BINARY OUT_DIR
set -eu
bin=${1:?the myrddin binary}; out=$(mkdir -p "${2:?folder for the pictures}" && cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
work="$out/.work"; rm -rf "$work"
eval "$("$here/fake-runtime.sh" "$work")"
shot() {   # NAME STATE PAGE [extra args]
    local name=$1 state=$2 page=$3; shift 3
    MYRDDIN_FAKE=$state QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=${SHOTS_PLATFORM:-offscreen} QT_QUICK_BACKEND=${SHOTS_BACKEND-software} WAYLAND_DISPLAY=${SHOTS_WAYLAND:-} \
        timeout 30 "$bin" --screenshot "$out/$name.png" --page "$page" --settle "${SETTLE:-1600}" "$@" 2> "$out/$name.log" || echo "FAILED: $name"
    grep -v "^QProcess: Destroyed" "$out/$name.log" | sed "s/^/  $name: /" | cut -c1-260
}
shot welcome idle welcome
shot welcome-2 idle welcome --test-eval 'welcome.step = 1'
shot welcome-3 idle welcome --test-eval 'welcome.step = 2; backend.refreshChecks()'
mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmode=auto\npanel=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
shot play-idle idle play
shot play-running running play
shot play-launcher launcher play
shot setup idle setup
shot options idle options
shot options-tall idle options --size 1080x2100
shot options-running running options --size 1080x2100
shot about idle about
shot play-plain plain play
shot play-narrow idle play --size 900x600
shot setup-tall idle setup --size 1080x1400
shot reader-report running options --test-eval 'backend.runTool("Last ten minutes", ["fg-watch", "--last", "60"])'
# the log of a session that ran with logs, offered when the game has closed; then read over the page
rt="$work/runtime"; mkdir -p "$rt/logs/io"; printf 'time,x\n1,2\n' > "$rt/logs/io/io-1234.csv"; touch -d '-20 seconds' "$rt/logs/io/io-1234.csv"
printf 'Frame generation x4\nDisplay: HDMI-A-1, 119.998 Hz\n' > "$rt/logs/launch.out"
printf '[General]\nwelcomed=true\nmode=auto\npanel=true\nwatch=true\nlastLaunchLayer=true\nlogPending=true\nlogSince=0\nlogCommand=myrddin fg 4 panel watch\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
SETTLE=2600 shot log-offer idle play
shot log-read idle play --size 1080x900 --test-eval 'backend.readSessionLog()'
# the desktop's colours: a light scheme and one with colours of its own (a file like KDE's kdeglobals)
printf '[Colors:Window]\nBackgroundNormal=239,240,241\nForegroundNormal=35,38,41\nForegroundPositive=39,174,96\nForegroundNeutral=246,116,0\nForegroundNegative=218,68,83\n[Colors:View]\nBackgroundNormal=255,255,255\n[Colors:Button]\nBackgroundNormal=252,252,252\n[Colors:Selection]\nBackgroundNormal=61,174,233\nForegroundNormal=255,255,255\n' > "$work/light.colors"
printf '[Colors:Window]\nBackgroundNormal=44,24,16\nForegroundNormal=245,222,179\n[Colors:View]\nBackgroundNormal=30,16,10\n[Colors:Button]\nBackgroundNormal=66,38,24\n[Colors:Selection]\nBackgroundNormal=233,150,60\nForegroundNormal=30,16,10\n' > "$work/own.colors"
MYRDDIN_COLORS="$work/light.colors" shot colors-light running play
MYRDDIN_COLORS="$work/own.colors" shot colors-own idle options
# Off on Play, and the limit in Options
printf '[General]\nwelcomed=true\nmode=off\nlimit=3\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
shot play-off idle play
shot options-limit idle options
# a multiplier of one's own with "Run with logs" on: the tab under the launch button, while the launcher is open too
printf '[General]\nwelcomed=true\nmode=3\nwatch=true\nlastLaunchLayer=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
shot play-logs idle play
shot play-logs-launcher launcher play
shot play-logs-running running play
MYRDDIN_COLORS="$work/light.colors" shot play-logs-light idle play
MYRDDIN_COLORS="$work/light.colors" shot play-light-off idle play --test-eval 'backend.prefs.watch = false'
shot play-wide idle play --size 1500x800
shot play-smooth idle play --test-eval 'backend.setOption("performance", false)'
# the launch button under the pointer: lit with its tab; "Stop" while the launcher or the game is up;
# "Stopping" after a click; "Kill" when the session is still there two seconds later
shot play-hover idle play --test-eval '@700 pointer.move(930, 215)'
shot play-stop running play --test-eval '@700 pointer.move(930, 215)'
SETTLE=1900 shot play-stopping launcher play --test-eval '@700 pointer.click(930, 215)' --test-eval '@900 pointer.move(300, 640)'
SETTLE=3600 shot play-kill launcher play --test-eval '@700 pointer.click(930, 215)'
printf '[General]\nwelcomed=true\nmode=auto\npanel=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
shot options-key idle options --size 1080x1000
shot console-running running console
shot console-idle idle console
ls -la "$out"/*.png | awk '{print $5, $9}'
rm -rf "$work"
