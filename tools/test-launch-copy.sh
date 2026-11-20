#!/usr/bin/env bash
# Tests tools/sc-launch-copy and the way sc-turbo uses it, without Wine and without the game: launch
# scripts of the LUG's layout in a scratch folder, whose "wine" is a stand-in that writes down how it
# was called and what it found in its environment.
#   tools/test-launch-copy.sh [WORK_DIR]        (default: a new folder under ${TMPDIR:-/tmp})
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
tool="$here/tools/sc-launch-copy"
work="${1:-$(mktemp -d "${TMPDIR:-/tmp}/launch-copy-test.XXXXXX")}"
rm -rf "$work"; mkdir -p "$work"
fails=0
ok() { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails + 1)); }
check() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }

# a stand-in runner: wine writes its arguments and some of its environment to $WINEPREFIX/called-NAME
runner() {   # runner NAME
    mkdir -p "$work/runners/$1/bin"
    cat > "$work/runners/$1/bin/wine" <<EOF
#!/usr/bin/env bash
{ echo "runner=$1"; echo "args=\$*"
  for v in __GL_SHADER_DISK_CACHE __GL_SHADER_DISK_CACHE_SIZE __GL_SHADER_DISK_CACHE_SKIP_CLEANUP __GL_SHADER_DISK_CACHE_PATH \\
           WINEDLLOVERRIDES MY_FLAG SPACED SC_FG WINE_SC_THP LIST PATHS PROTON_ENABLE_NGX_UPDATER \\
           DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE \\
           DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION \\
           DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS VK_LOADER_LAYERS_ENABLE VK_VRAM_REPORT_LIMIT_HEAP_SIZE VK_VRAM_REPORT_LIMIT_DEVICE_ID \\
           VK_ADD_LAYER_PATH; do
      [ -n "\${!v+set}" ] && echo "\$v=\${!v}"
  done; } > "\$WINEPREFIX/called"
exit 0
EOF
    printf '#!/usr/bin/env bash\nexit 0\n' > "$work/runners/$1/bin/wineserver"
    chmod +x "$work/runners/$1/bin/wine" "$work/runners/$1/bin/wineserver"
}
runner stock; runner other

# a launch script as the LUG Helper writes it; $1: extra lines for its environment part, $2: the launch line
script() {   # script FILE "ENV LINES" [LAUNCH LINES]
    local launch="${3:-\"\$wine_path\"/wine \"C:\\Program Files\\Roberts Space Industries\\RSI Launcher\\RSI Launcher.exe\" > \"\$launch_log\" 2>&1}"
    cat > "$1" <<EOF
#!/usr/bin/env bash
# This script launches Star Citizen using Wine.
export WINEPREFIX="$work/prefix"
launch_log="\$WINEPREFIX/sc-launch.log"
export WINEDLLOVERRIDES="winemenubuilder.exe=d" # Prevent updates from overwriting our .desktop entries
$2
# export wine_path="/path/to/custom/runner/bin"
export wine_path="$work/runners/stock/bin"
case "\$1" in
    "shell") echo shell; exit 0 ;;
esac
"\$wine_path"/wineserver -k
$launch
EOF
    chmod +x "$1"
}
mkdir -p "$work/prefix"
called() { grep -qxF "$1" "$work/prefix/called"; }
run_copy() { rm -f "$work/prefix/called"; env -u __GL_SHADER_DISK_CACHE -u __GL_SHADER_DISK_CACHE_SIZE -u __GL_SHADER_DISK_CACHE_SKIP_CLEANUP -u MY_FLAG "$@"; }

echo "1. A copy is made, the original stays as it was"
script "$work/sc-launch.sh" 'export __GL_SHADER_DISK_CACHE_SIZE=10737418240'
before="$(sha256sum < "$work/sc-launch.sh")"; stamp="$(stat -c %Y "$work/sc-launch.sh")"
"$tool" "$work/sc-launch.sh" "$work/out/sc-launch.sh" --default __GL_SHADER_DISK_CACHE=1 --default __GL_SHADER_DISK_CACHE_SIZE=1 \
    --default __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 --set MY_FLAG=on --set "SPACED=two words and a ' quote" > "$work/out.txt" 2>&1
check $? "the tool says yes"
[ "$(sha256sum < "$work/sc-launch.sh")" = "$before" ] && [ "$(stat -c %Y "$work/sc-launch.sh")" = "$stamp" ]; check $? "the original has the same content and time"
[ -x "$work/out/sc-launch.sh" ] && bash -n "$work/out/sc-launch.sh"; check $? "the copy can be run and parses"
[ "$(diff "$work/sc-launch.sh" "$work/out/sc-launch.sh" | grep -c '^<')" = 0 ]; check $? "no line of the original is missing or altered in the copy"
grep -q "before the line that starts the game" "$work/out.txt"; check $? "it says where the settings went"

echo "2. Running the copy"
run_copy "$work/out/sc-launch.sh"; check $? "the copy runs to its end"
called "runner=stock"; check $? "the script's own runner starts the launcher"
grep -q '^args=C:.*RSI Launcher.exe$' "$work/prefix/called"; check $? "with the launcher as its argument"
called "__GL_SHADER_DISK_CACHE=1"; check $? "a default is set where the script has none"
called "__GL_SHADER_DISK_CACHE_SIZE=10737418240"; check $? "the script's own value wins over a default"
called "MY_FLAG=on"; check $? "a forced setting is set"
called "SPACED=two words and a ' quote"; check $? "a value with spaces and a quote arrives whole"
called "WINEDLLOVERRIDES=winemenubuilder.exe=d"; check $? "what the script sets is untouched"
run_copy env __GL_SHADER_DISK_CACHE=0 "$work/out/sc-launch.sh"
called "__GL_SHADER_DISK_CACHE=0"; check $? "a value from the environment wins over a default"
run_copy "$work/out/sc-launch.sh" shell > "$work/shell.txt"
[ "$(cat "$work/shell.txt")" = shell ] && [ ! -e "$work/prefix/called" ]; check $? "the script's own arguments still work (\"shell\")"

echo "3. Another runner"
"$tool" "$work/sc-launch.sh" "$work/out/sc-launch.sh" --runner "$work/runners/other/bin" > "$work/out.txt" 2>&1
run_copy "$work/out/sc-launch.sh"
called "runner=other"; check $? "the runner named is the one that starts"
grep -c '^export wine_path=' "$work/out/sc-launch.sh" | grep -qx 1 && grep -q '^# export wine_path="/path/to/custom' "$work/out/sc-launch.sh"; check $? "only the script's own runner line was replaced (the commented example stays)"
[ "$(sha256sum < "$work/sc-launch.sh")" = "$before" ]; check $? "the original still has the same content"

echo "4. A launch command over several lines, and a script without one"
script "$work/multi.sh" 'export MY_FLAG=script' '"$wine_path"/wine \
    "C:\Program Files\Roberts Space Industries\RSI Launcher\RSI Launcher.exe" \
    > "$launch_log" 2>&1'
"$tool" "$work/multi.sh" "$work/out/multi.sh" --set MY_FLAG=placed > "$work/out.txt" 2>&1; check $? "a continued command: copy made"
run_copy "$work/out/multi.sh"
called "MY_FLAG=placed" && called "runner=stock"; check $? "the settings are in force when the command runs"
script "$work/custom.sh" 'export MY_FLAG=script' '"$wine_path"/wine game.exe'
"$tool" "$work/custom.sh" "$work/out/custom.sh" --default MY_FLAG=default --default __GL_SHADER_DISK_CACHE=1 > "$work/out.txt" 2>&1; check $? "no launcher line: copy made"
grep -q "at the top" "$work/out.txt"; check $? "it says the settings are at the top"
run_copy "$work/out/custom.sh"
called "MY_FLAG=script" && called "__GL_SHADER_DISK_CACHE=1"; check $? "the script's own settings win there, the rest is set"

echo "4b. Lists that are added to"
"$tool" "$work/sc-launch.sh" "$work/out/list.sh" --append LIST=two --append LIST=three --append-path PATHS=/b > "$work/out.txt" 2>&1; check $? "copy made"
run_copy env -u LIST -u PATHS "$work/out/list.sh"
called "LIST=two,three" && called "PATHS=/b"; check $? "an empty list gets the items, without a stray separator"
run_copy env LIST=one,two PATHS=/a:/b "$work/out/list.sh"
called "LIST=one,two,three" && called "PATHS=/a:/b"; check $? "an item that is in the list already is not added again"

echo "5. Files no copy is made of"
printf '#!/usr/bin/env python3\nprint("x")\n' > "$work/py.sh"
"$tool" "$work/py.sh" "$work/out/py.sh" > "$work/out.txt" 2>&1; [ $? = 2 ] && [ ! -e "$work/out/py.sh" ]; check $? "not a shell script: refused, nothing written"
printf '#!/bin/bash\ncd "$(dirname "$0")"\n./wine "RSI Launcher.exe"\n' > "$work/self.sh"; chmod +x "$work/self.sh"
"$tool" "$work/self.sh" "$work/out/self.sh" > "$work/out.txt" 2>&1; [ $? = 2 ] && [ ! -e "$work/out/self.sh" ]; check $? "a script that finds its files from its own place: refused"
printf '#!/bin/bash\nif then fi (\n"$w"/wine "RSI Launcher.exe"\n' > "$work/broken.sh"
"$tool" "$work/broken.sh" "$work/out/broken.sh" > "$work/out.txt" 2>&1; [ $? = 2 ] && [ ! -e "$work/out/broken.sh" ] && [ ! -e "$work/out/broken.sh.new" ]; check $? "a script that does not parse: refused, nothing left behind"
"$tool" "$work/sc-launch.sh" "$work/sc-launch.sh" > "$work/out.txt" 2>&1; [ $? = 1 ] && [ "$(sha256sum < "$work/sc-launch.sh")" = "$before" ]; check $? "the original is never the place the copy is written to"
"$tool" "$work/sc-launch.sh" "$work/out/x.sh" --set 'BAD NAME=1' > "$work/out.txt" 2>&1; [ $? = 1 ]; check $? "a setting with a bad name is a mistake, not a launch"

if [ -x "$here/sc-turbo" ] && grep -q 'sc-launch-copy' "$here/sc-turbo"; then
    echo "6. Through sc-turbo (a copy of it in the scratch folder, so nothing of the project is written)"
    t="$work/turbo"; mkdir -p "$t/tools"
    cp "$here/sc-turbo" "$t/sc-turbo"; cp "$tool" "$t/tools/"
    printf '#!/usr/bin/env bash\nexit 1\n' > "$t/tools/sc-sched"; chmod +x "$t/tools/sc-sched"   # (no scheduler in a test)
    # a stand-in for the driver's tool: a 12 GB card (or what CARD says: "MiB, 0xDDDDVVVV")
    mkdir -p "$work/fakebin"; printf '#!/usr/bin/env bash\necho "${CARD:-12282, 0x278310DE}"\n' > "$work/fakebin/nvidia-smi"; chmod +x "$work/fakebin/nvidia-smi"
    turbo() { rm -f "$work/prefix/called"; ( cd "$t" && env -i PATH="$work/fakebin:$PATH" HOME="$work/home" XDG_DATA_HOME="$work/home/data" SC_PREFIX="$work/prefix" SC_LAUNCH="$work/sc-launch.sh" SC_MONITOR=off "$@" ) > "$work/turbo.txt" 2>&1; }
    syslayer=0; grep -qs VK_LAYER_MESA_vram_report_limit /usr/share/vulkan/*_layer.d/*.json /usr/local/share/vulkan/explicit_layer.d/*.json /etc/vulkan/explicit_layer.d/*.json && syslayer=1
    turbo ./sc-turbo run
    called "runner=stock" && called "__GL_SHADER_DISK_CACHE=1" && called "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1"; check $? "\"run\" starts the copy, with the NVIDIA shader cache settings"
    called "__GL_SHADER_DISK_CACHE_SIZE=10737418240"; check $? "the size the script set itself is kept"
    [ -x "$t/logs/launch/sc-launch.sh" ] && [ "$(sha256sum < "$work/sc-launch.sh")" = "$before" ]; check $? "the copy is in logs/launch, the original untouched"
    turbo env SC_RUNNER="$work/runners/other" ./sc-turbo run
    called "runner=other"; check $? "SC_RUNNER names another runner for one launch"
    turbo env SC_NVIDIA_FLAGS=0 ./sc-turbo run
    called "runner=stock" && ! grep -q -E '^(__GL_SHADER_DISK_CACHE=|PROTON_|DXVK_|VK_)' "$work/prefix/called"; check $? "SC_NVIDIA_FLAGS=0 leaves every NVIDIA setting out"
    turbo env SC_LAUNCH_COPY=0 ./sc-turbo run
    called "runner=stock" && ! grep -q '^__GL_SHADER_DISK_CACHE=' "$work/prefix/called"; check $? "SC_LAUNCH_COPY=0 starts the original itself"
    turbo env SC_LAUNCH="$work/self.sh" ./sc-turbo run
    grep -q "started as it is" "$work/turbo.txt"; check $? "a script no copy can be made of is started as it is, and that is said"
    turbo env SC_LAUNCH_SET="MY_FLAG=conf WINE_SC_THP=1" ./sc-turbo run
    called "MY_FLAG=conf" && called "WINE_SC_THP=1"; check $? "SC_LAUNCH_SET places settings of the user's own"
    echo "7. The LUG wiki's NVIDIA settings"
    turbo ./sc-turbo run
    called "PROTON_ENABLE_NGX_UPDATER=1" && called "DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE=on" && called "DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE=on" \
        && called "DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE=on" && called "DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_latest" \
        && called "DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_latest"; check $? "the six DLSS 4 settings are placed as the wiki has them"
    ! grep -q '^DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=' "$work/prefix/called"; check $? "the indicator setting is not placed unasked"
    turbo env SC_DLSS_OVERRIDE=0 ./sc-turbo run
    ! grep -q -E '^(PROTON_|DXVK_NVAPI_DRS)' "$work/prefix/called" && called "__GL_SHADER_DISK_CACHE=1"; check $? "SC_DLSS_OVERRIDE=0 leaves them out and keeps the shader cache settings"
    turbo env DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_k ./sc-turbo run
    called "DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_k"; check $? "a preset the user names is kept"
    turbo env SC_DLSS_INDICATOR=1 ./sc-turbo run
    called "DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024,DLSSGIndicator=2"; check $? "SC_DLSS_INDICATOR=1 switches DLSS's indicator on"
    turbo ./sc-turbo run
    called "DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1,DLSSGIndicator=1"; check $? "the launch after it switches the indicator off"
    turbo ./sc-turbo run
    ! grep -q '^DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=' "$work/prefix/called"; check $? "and the one after that places nothing for it"
    if [ "$syslayer" = 0 ]; then
        turbo ./sc-turbo run
        grep -q "NOTE: the graphics memory limit is not set" "$work/turbo.txt" && ! grep -q '^VK_' "$work/prefix/called"; check $? "without Mesa's layer nothing is placed for the memory limit, and that is said"
        mkdir -p "$t/build/vram-limit"; echo '{"layer":{"name":"VK_LAYER_MESA_vram_report_limit"}}' > "$t/build/vram-limit/VkLayer_MESA_vram_report_limit.json"
        turbo ./sc-turbo run
        called "VK_ADD_LAYER_PATH=$t/build/vram-limit"; check $? "the layer in build/vram-limit is named to the Vulkan loader"
    fi
    turbo ./sc-turbo run
    called "VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_vram_report_limit" && called "VK_VRAM_REPORT_LIMIT_HEAP_SIZE=9216" && called "VK_VRAM_REPORT_LIMIT_DEVICE_ID=0x10de:0x2783"; check $? "a 12 GB card: the game is told 9216 MiB, the card named as vendor:device"
    turbo env VK_LOADER_LAYERS_ENABLE=VK_LAYER_other ./sc-turbo run
    called "VK_LOADER_LAYERS_ENABLE=VK_LAYER_other,VK_LAYER_MESA_vram_report_limit"; check $? "layers the user enables stay enabled"
    turbo env CARD="8188, 0x248810DE" ./sc-turbo run
    called "VK_VRAM_REPORT_LIMIT_HEAP_SIZE=6144" && called "VK_VRAM_REPORT_LIMIT_DEVICE_ID=0x10de:0x2488"; check $? "an 8 GB card: 6144 MiB"
    turbo env CARD="16376, 0x270410DE" ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && called "runner=stock"; check $? "a 16 GB card is left alone"
    turbo env CARD="16376, 0x270410DE" SC_VRAM_LIMIT=12288 ./sc-turbo run
    called "VK_VRAM_REPORT_LIMIT_HEAP_SIZE=12288"; check $? "SC_VRAM_LIMIT=MiB sets a figure of the user's own"
    turbo env SC_VRAM_LIMIT=off ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && called "PROTON_ENABLE_NGX_UPDATER=1"; check $? "SC_VRAM_LIMIT=off leaves the memory limit out"
    turbo env CARD="" ./sc-turbo run
    called "runner=stock"; check $? "(default card again)"
    printf '#!/usr/bin/env bash\nexit 9\n' > "$work/fakebin/nvidia-smi"
    turbo ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && called "runner=stock" && ! grep -q "NOTE: the graphics" "$work/turbo.txt"; check $? "no NVIDIA card to ask: nothing placed, nothing said, the game starts"
    printf '#!/usr/bin/env bash\necho "${CARD:-12282, 0x278310DE}"\n' > "$work/fakebin/nvidia-smi"
    # with frame generation (an empty file stands in for the layer; the stand-in Wine loads nothing)
    mkdir -p "$t/build/fg" "$work/prefix/drive_c/windows/system32"; : > "$t/build/fg/libsc_fg.so"
    turbo ./sc-turbo fg 2
    called "SC_FG=1" && called "__GL_SHADER_DISK_CACHE=1"; check $? "\"fg 2\" starts the copy with the layer's and the NVIDIA settings"
    grep -q "NOTE: the prefix has no nvapi64.dll" "$work/turbo.txt" && grep -q "nvngx.dll and _nvngx.dll" "$work/turbo.txt"; check $? "a prefix without NVAPI and NGX is named before the launch"
    : > "$work/prefix/drive_c/windows/system32/nvapi64.dll"; : > "$work/prefix/drive_c/windows/system32/nvngx.dll"
    ln -s /nonexistent/_nvngx.dll "$work/prefix/drive_c/windows/system32/_nvngx.dll"
    turbo ./sc-turbo fg 2
    grep -q "NOTE: the prefix has no nvngx.dll" "$work/turbo.txt"; check $? "a link to a driver file that is gone counts as missing"
    rm "$work/prefix/drive_c/windows/system32/_nvngx.dll"; : > "$work/prefix/drive_c/windows/system32/_nvngx.dll"
    turbo ./sc-turbo fg 2
    ! grep -q "NOTE: the prefix" "$work/turbo.txt" && called "SC_FG=1"; check $? "a prefix that has them gets no note"
fi

echo
if [ "$fails" = 0 ]; then echo "all checks passed"; else echo "$fails check(s) FAILED (work folder: $work)"; fi
exit "$((fails > 0))"
