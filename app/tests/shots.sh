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
mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmultiplier=4\npanel=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
shot play-idle idle play
shot play-running running play
shot play-launcher launcher play
shot setup idle setup
shot options idle options
shot options-running running options --size 1080x1500
shot session-running running session
shot about idle about
shot play-plain plain play
shot play-narrow idle play --size 900x600
shot setup-tall idle setup --size 1080x1400
shot session-tool running session --size 1080x1100 --test-eval 'backend.runTool("Last ten minutes", ["fg-watch", "--last", "60"])'
ls -la "$out"/*.png | awk '{print $5, $9}'
rm -rf "$work"
