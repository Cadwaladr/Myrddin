#!/usr/bin/env bash
# Makes a stand-in for everything the window touches, under DIR: a runtime with a fake sc-turbo (it
# writes down how it was called), a game folder with a launch script and a runner, the layer's log with
# frame-rate lines, and empty settings. Prints the environment to run the window in:
#
#   eval "$(tests/fake-runtime.sh DIR)"; QT_QPA_PLATFORM=offscreen myrddin --screenshot out.png
#
# Nothing outside DIR is read or written by a window started that way.
set -eu
dir=$(mkdir -p "${1:?folder}" && cd "$1" && pwd)
rt="$dir/runtime"; prefix="$dir/prefix"
runner="$prefix/runners/lug-wine-tkg-experimental-wayland-git-11.22-1"
mkdir -p "$rt/tools" "$rt/build/fg" "$rt/logs/fg" "$runner/bin" "$runner/lib/wine/x86_64-windows" \
         "$dir/config/starcitizen-lug" "$dir/data" "$dir/bin"
cat > "$rt/sc-turbo" <<'SC'
#!/usr/bin/env bash
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
echo "$*" >> "$here/calls.log"
env | grep -E '^(APPDIR|APPIMAGE|LD_LIBRARY_PATH|QT_PLUGIN_PATH|QML2?_IMPORT_PATH|QT_QPA_PLATFORM|MYRDDIN_[A-Z_]*)=' >> "$here/calls.env" || true
case "${1:-}" in
displays) echo "HDMI-A-1   2560x1440 at 119.998 Hz, scale 100 %, at 0,0, KDE Plasma primary  <- the game's main display"
          echo "DP-2       3840x2160 at 60.000 Hz, scale 150 %, at 2560,0"
          echo "choice: HDMI-A-1 (KDE Plasma's primary display)" ;;
fg|run)   echo "Display: HDMI-A-1, 119.998 Hz"; [ "$1" = fg ] && echo "Frame generation x${2:-2}; log: $here/logs/fg"
          echo "CPU scheduler: scx_lavd (gaming) while the game runs, the stock one after it (log: logs/sched.out)" ;;
fg-mult)  echo "multiplier x$2 requested" ;;
fg-off)   touch "$here/logs/fg/OFF"; echo "generation off" ;;
fg-on)    rm -f "$here/logs/fg/OFF"; echo "generation on" ;;
fg-params) shift; echo "parameters sent: $*" ;;
fg-panel) echo "panel: shown rate (${2:-tl})" ;;
fg-watch) printf '%s\n' "  time  mult  game  shown  p50   p99" "  10 s  x4    30.1  119.8  33.2  41.0" "  20 s  x4    30.4  120.0  32.9  39.6" ;;
latency)  printf '%s\n' "x4: game frame to display 41.5 ms (the game's frame 33.1, generation 4.2, wait for the display 4.2)" ;;
check)    printf '%s\n' "== ntsync ==" "in use by the game (pid 1234)" ;;
iowatch)  printf '%s\n' "io-1234.csv: 46 minutes" "GPU memory: 10.0 GB at the most" "(asked with: $*; home: $HOME/Games)" ;;
"")       echo "usage"; exit 1 ;;
*)        echo "$1: done" ;;
esac
SC
printf '#!/usr/bin/env bash\n[ "$1" = allowed ] && exit 0\necho stock\n' > "$rt/tools/sc-sched"
chmod +x "$rt/sc-turbo" "$rt/tools/sc-sched"
: > "$rt/build/fg/libsc_fg.so"; echo '{}' > "$rt/build/fg/sc_fg.json"
printf 'v0.1-test\n2026-11-19 UTC\n' > "$rt/VERSION"
# the game's own settings file, with DLSS as its upscaler (the system check reads that one value)
profile="$prefix/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/user/client/0/Profiles/default"
mkdir -p "$profile"
printf '<Attributes Version="1">\n <Attr name="Upscaling" value="4"/>\n <Attr name="UpscalingTechnique" value="2"/>\n <Attr name="WindowMode" value="2"/>\n</Attributes>\n' > "$profile/attributes.xml"
# ten minutes of the layer's frame-rate lines: a game near 30 fps shown at 120, with one dip
python3 - "$rt/logs/fg/fg-1234.log" <<'PY'
import math, sys
with open(sys.argv[1], "w") as f:
    for i in range(60):
        real = 30.2 + 2.2 * math.sin(i / 5.0) - (9.0 if 31 <= i <= 33 else 0.0)
        shown = min(real * 4, 119.9)
        f.write(f"  {100 + i * 10:.3f} STAT x4 real={real:.1f}/s generated={shown - real:.1f}/s shown={shown:.1f}/s untouched=0 | "
                f"game frame ms mean={1000 / real:.1f} p50={1000 / real - 1.5:.1f} p95={1000 / real + 6:.1f} p99={1000 / real + 11:.1f} max=105.0 | shown frame ms mean=8.3\n")
PY
printf '#!/usr/bin/env bash\n# stand-in launch script\nexport WINEPREFIX="%s"\nexport wine_path="%s/bin"\nsleep 1\n' "$prefix" "$runner" > "$prefix/sc-launch.sh"
chmod +x "$prefix/sc-launch.sh"
truncate -s 5M "$runner/lib/wine/x86_64-windows/d3d12core.dll"
echo "$prefix" > "$dir/config/starcitizen-lug/winedir.conf"
cat <<ENV
export MYRDDIN_RUNTIME='$rt' MYRDDIN_CONFIG_DIR='$dir/config/myrddin' MYRDDIN_BIN_DIR='$dir/bin'
export XDG_CONFIG_HOME='$dir/config' XDG_DATA_HOME='$dir/data'
unset XDG_CURRENT_DESKTOP WAYLAND_DISPLAY DISPLAY MYRDDIN_PAYLOAD
ENV
