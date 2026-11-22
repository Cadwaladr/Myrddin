#!/usr/bin/env bash
# Tests tools/sc-monitor without a desktop: stand-ins for kscreen-doctor (KDE Plasma), cosmic-randr
# (COSMIC) and niri that answer as those programs do, and checks of the display it picks.
#   tools/test-monitor.sh [WORK_DIR]        (default: a new folder under ${TMPDIR:-/tmp})
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
tool="$here/tools/sc-monitor"
work="${1:-$(mktemp -d "${TMPDIR:-/tmp}/monitor-test.XXXXXX")}"
rm -rf "$work"; mkdir -p "$work/bin"
fails=0
ok() { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails + 1)); }
is() {   # WHAT EXPECTED DESCRIPTION
    if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (got \"$1\", wanted \"$2\")"; fi
}
# (only the stand-ins on the path: none of the real tools may answer)
run() { env -i HOME="$work" PATH="$work/bin:/usr/bin:/bin" "$@"; }
for real in kscreen-doctor cosmic-randr niri; do
    printf '#!/bin/sh\nexit 1\n' > "$work/bin/$real"; chmod +x "$work/bin/$real"
done

echo "1. niri: niri msg --json outputs"
cat > "$work/bin/niri" <<'EOF'
#!/bin/sh
[ "$*" = "msg --json outputs" ] || exit 1
cat <<'JSON'
{"DP-1":{"name":"DP-1","make":"Some","model":"TV","serial":null,"physical_size":[1210,680],"modes":[{"width":3840,"height":2160,"refresh_rate":60000,"is_preferred":true},{"width":3840,"height":2160,"refresh_rate":30000,"is_preferred":false}],"current_mode":0,"is_custom_mode":false,"vrr_supported":false,"vrr_enabled":false,"logical":{"x":2560,"y":0,"width":2560,"height":1440,"scale":1.5,"transform":"Normal"}},
 "HDMI-A-1":{"name":"HDMI-A-1","make":"Some","model":"Monitor","serial":null,"physical_size":[600,340],"modes":[{"width":2560,"height":1440,"refresh_rate":143995,"is_preferred":false},{"width":2560,"height":1440,"refresh_rate":119998,"is_preferred":true}],"current_mode":1,"is_custom_mode":false,"vrr_supported":true,"vrr_enabled":false,"logical":{"x":0,"y":0,"width":2560,"height":1440,"scale":1.0,"transform":"Normal"}},
 "DP-2":{"name":"DP-2","make":"Off","model":"Display","serial":null,"physical_size":null,"modes":[{"width":1920,"height":1080,"refresh_rate":240000,"is_preferred":true}],"current_mode":null,"is_custom_mode":false,"vrr_supported":false,"vrr_enabled":false,"logical":null}}
JSON
EOF
chmod +x "$work/bin/niri"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x "$tool")" "HDMI-A-1" "the display with the highest refresh rate (niri has no primary one)"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x "$tool" rate)" "119.998" "its current rate, not its highest one"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x SC_MONITOR=DP-1 "$tool")" "DP-1" "SC_MONITOR names another display that is on"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x SC_MONITOR=DP-2 "$tool")" "HDMI-A-1" "a display that is off is not taken, even when named"
is "$(run XDG_CURRENT_DESKTOP=niri "$tool")" "" "without niri's socket niri is not asked: nothing, the runner picks"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x SC_MONITOR=off "$tool")" "" "SC_MONITOR=off: nothing"

echo "2. KDE Plasma: kscreen-doctor -j"
cat > "$work/bin/kscreen-doctor" <<'EOF'
#!/bin/sh
[ "$*" = "-j" ] || exit 1
cat <<'JSON'
{"outputs":[{"name":"DP-1","enabled":true,"connected":true,"priority":2,"currentModeId":"8","modes":[{"id":"8","size":{"width":3840,"height":2160},"refreshRate":60.0}],"pos":{"x":2560,"y":0},"scale":1.5},
 {"name":"HDMI-A-1","enabled":true,"connected":true,"priority":1,"currentModeId":"39","modes":[{"id":"39","size":{"width":2560,"height":1440},"refreshRate":119.99800109863281}],"pos":{"x":0,"y":0},"scale":1.0}]}
JSON
EOF
chmod +x "$work/bin/kscreen-doctor"
is "$(run XDG_CURRENT_DESKTOP=KDE "$tool")" "HDMI-A-1" "KDE's primary display (priority 1)"
is "$(run XDG_CURRENT_DESKTOP=KDE "$tool" rate)" "119.998" "and its rate"
is "$(run XDG_CURRENT_DESKTOP=niri NIRI_SOCKET=/x "$tool" show | grep "^choice:")" "choice: HDMI-A-1 (the highest refresh rate)" "on niri with KDE's tool there too, niri is asked first"
is "$(run XDG_CURRENT_DESKTOP=KDE NIRI_SOCKET=/x "$tool" show | grep "^choice:")" "choice: HDMI-A-1 (KDE Plasma's primary display)" "on KDE, KDE's tool first"

echo "3. COSMIC: cosmic-randr list --kdl"
cat > "$work/bin/cosmic-randr" <<'EOF'
#!/bin/sh
[ "$*" = "list --kdl" ] || exit 1
cat <<'KDL'
output "DP-1" enabled=#true {
    description make="Some" model="TV"
    position 2560 0
    scale 1.5
    modes {
        mode 3840 2160 60000 current=#true
    }
}
output "HDMI-A-1" enabled=#true {
    description make="Some" model="Monitor"
    position 0 0
    scale 1.0
    xwayland_primary #true
    modes {
        mode 2560 1440 143995
        mode 2560 1440 119998 current=#true
    }
}
KDL
EOF
chmod +x "$work/bin/cosmic-randr"
is "$(run XDG_CURRENT_DESKTOP=COSMIC "$tool")" "HDMI-A-1" "COSMIC's Xwayland primary"
is "$(run XDG_CURRENT_DESKTOP=COSMIC "$tool" rate)" "119.998" "and its current rate"

[ $fails = 0 ] && echo "all checks passed" || echo "$fails check(s) FAILED"
[ $fails = 0 ]
