#!/usr/bin/env bash

# This script launches Star Citizen using Wine.
# It is meant to be used after installation via the LUG Helper.
#
# Usage:
# Run from your terminal or use the .desktop files installed by the Helper.
#
# version: 2.5
# License: GPLv3.0
#
# Changes to the Helper's script made by this project (an example: your own script has your
# prefix and runner in it; carry the TUNING section and the two marked blocks over):
#   - wine_path uses a copy of the same LUG runner whose kernelbase.dll implements
#     CPU sets / EcoQoS, i.e. the game's own P-core/E-core requests.  If that copy
#     is missing, or the Helper switched you to another runner, the runner named
#     in wine_path is used as before.
#   - sc-hybrid runs next to the game (main thread -> P-cores, background service
#     threads -> E-cores) and stops by itself after this script exits.
#   - SC_X11=1 runs the game through XWayland so Discord can stream it.
#   - Each launch appends its settings to <project>/logs/launches.log, and the
#     previous sc-launch.log is kept as <project>/logs/sc-launch.prev.log.
# Everything is switched in the TUNING section below. Keep a copy of the Helper's own script
# before changing it: the Helper writes a new one when it updates.

############################################################################
# ENVIRONMENT VARIABLES
############################################################################
# Add additional environment variables to this section as needed
# Example:
# export NEW_VARIABLE="value"
############################################################################
export WINEPREFIX="$HOME/Games/star-citizen"   # your prefix

launch_log="$WINEPREFIX/sc-launch.log"
unset SDL_VIDEODRIVER

export WINEDLLOVERRIDES="winemenubuilder.exe=d" # Prevent updates from overwriting our .desktop entries
export WINEDEBUG=-all # Cut down on console debug messages
# Nvidia cache options
export __GL_SHADER_DISK_CACHE=1
export __GL_SHADER_DISK_CACHE_SIZE=10737418240
export __GL_SHADER_DISK_CACHE_PATH="$WINEPREFIX"
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
# Mesa (AMD/Intel) shader cache options
export MESA_SHADER_CACHE_DIR="$WINEPREFIX"
export MESA_SHADER_CACHE_MAX_SIZE="10G"
# Performance options
#export DXVK_ASYNC=1
# Optional HUDs
#export DXVK_HUD=fps,compiler
#export MANGOHUD=1

############################################################################
# END ENVIRONMENT VARIABLES
############################################################################

############################################################################
# TUNING (this project)
############################################################################
# Change the values here.  From a terminal you can also override one launch, e.g.
#   WINE_CPUSETS=0 SC_HYBRID=off ./sc-launch.sh     # behaves like the stock script
SC_PROJECT="$HOME/Myrddin/wine"   # the wine folder of this project
# The game's CPU set requests: class (P-core/E-core level) | exact | 0 (ignored, like stock)
export WINE_CPUSETS="${WINE_CPUSETS:-0}"   # (class until 2026-11-14: measured 4-6 % slower than 0 in a heavy scene)
# 1 = threads that ask for EcoQoS run on the E-cores | 0 = ignore EcoQoS
export WINE_ECOQOS="${WINE_ECOQOS:-0}"
# 1 = log the game's CPU set / priority calls to sc-launch.log (read them with
#     tools/sc-schedlog).  For a diagnostic session only: logging costs time.
export WINE_SCHED_LOG="${WINE_SCHED_LOG:-0}"
# sc-hybrid policy: main | main-only | off
SC_HYBRID="${SC_HYBRID:-off}"
# 1 = Wine's X11 driver through XWayland instead of Wine Wayland, so Discord can
#     stream the game (no HDR; XWayland mouse issues are possible)
SC_X11="${SC_X11:-0}"
if [ "$SC_X11" = "1" ]; then
    export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;winewayland.drv=d"
fi
# The display the game takes as its main one: auto | a name (HDMI-A-1; the names are shown by
#     "$SC_PROJECT/tools/sc-monitor show") | off.  Left to itself (off) this runner's Wayland
#     driver takes the display with the most pixels: a 4K TV beside the monitor wins, and the
#     game renders 3840x2160 at the TV's refresh rate onto the monitor.
#     auto = the display COSMIC has as primary, else the one with the highest refresh rate
SC_MONITOR="${SC_MONITOR:-auto}"
if [ "$SC_MONITOR" != "off" ] && [ -z "${WAYLANDDRV_PRIMARY_MONITOR:-}" ] && [ -x "$SC_PROJECT/tools/sc-monitor" ]; then
    sc_monitor="$(SC_MONITOR="$SC_MONITOR" "$SC_PROJECT/tools/sc-monitor" 2>/dev/null)"
    [ -n "$sc_monitor" ] && export WAYLANDDRV_PRIMARY_MONITOR="$sc_monitor"
fi

##################
# Wine binary path
##################
# To use a custom wine runner, set the path to its bin directory
# export wine_path="/path/to/custom/runner/bin"
export wine_path="$WINEPREFIX/runners/lug-wine-tkg-experimental-wayland-git-11.20-2/bin"
# Use the CPU-sets copy of that runner if it exists (build/make-patched-runner.sh)
sc_patched_bin="$SC_PROJECT/runners/$(basename "$(dirname "$wine_path")")-cpusets/bin"
if [ -x "$sc_patched_bin/wine" ]; then
    export wine_path="$sc_patched_bin"
fi

########################
# Command line arguments
########################
# shell - Drop into a Wine maintenance shell
# config - Wine configuration
# controllers - Game controller configuration
# Usage: ./sc-launch.sh shell
case "$1" in
    "shell")
        echo "Entering Wine prefix maintenance shell. Type 'exit' when done."
        export PATH="$wine_path:$PATH"; export PS1="Wine: "
        cd "$WINEPREFIX"; pwd; /usr/bin/env bash --norc; exit 0
        ;;
    "config")
        /usr/bin/env bash --norc -c "\"${wine_path}\"/winecfg"; exit 0
        ;;
    "controllers")
        /usr/bin/env bash --norc -c "\"${wine_path}\"/wine control joy.cpl"; exit 0
        ;;
esac

##########################
# Update check and cleanup
##########################
# Kill existing wine processes before launch
update_check() {
    while "$wine_path"/winedbg --command "info proc" | grep -qi "rsi.*setup"; do
        echo "RSI Setup process detected. Exiting."; exit 0
    done
}
"$wine_path"/wineserver -k

#####################################################
# sc-hybrid and launch logs (this project)
#####################################################
sc_settings="WINE_CPUSETS=$WINE_CPUSETS WINE_ECOQOS=$WINE_ECOQOS WINE_SCHED_LOG=$WINE_SCHED_LOG"
sc_settings="$sc_settings SC_HYBRID=$SC_HYBRID SC_X11=$SC_X11 monitor=${WAYLANDDRV_PRIMARY_MONITOR:-runner} nice-limit=$(ulimit -e) wine_path=$wine_path"
if [ -d "$SC_PROJECT" ]; then
    mkdir -p "$SC_PROJECT/logs"
    if [ "$SC_HYBRID" != "off" ] && [ -x "$SC_PROJECT/tools/sc-hybrid" ]; then
        # One governor per game session for as long as this script runs.  The loop
        # and sc-hybrid end by themselves once this script has exited.
        ( while kill -0 $$ 2>/dev/null; do
              "$SC_PROJECT/tools/sc-hybrid" run --wait --while-pid $$ --policy "$SC_HYBRID" || sleep 5
          done ) >> "$SC_PROJECT/logs/sc-hybrid.log" 2>&1 &
    fi
    # sc-launch.log is overwritten below; keep the previous one
    [ -f "$launch_log" ] && cp -f "$launch_log" "$SC_PROJECT/logs/sc-launch.prev.log"
    echo "$(date '+%F %T') $sc_settings" >> "$SC_PROJECT/logs/launches.log"
fi
echo "### sc-launch $(date '+%F %T') $sc_settings" > "$launch_log"

############################################################################
# Launch the game
############################################################################
"$wine_path"/wine "C:\Program Files\Roberts Space Industries\RSI Launcher\RSI Launcher.exe" >> "$launch_log" 2>&1
