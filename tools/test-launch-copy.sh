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
{ echo "runner=$1"; echo "args=\$*"; echo "core=\$(ulimit -c)"
  for v in __GL_SHADER_DISK_CACHE __GL_SHADER_DISK_CACHE_SIZE __GL_SHADER_DISK_CACHE_SKIP_CLEANUP __GL_SHADER_DISK_CACHE_PATH \\
           WINEDLLOVERRIDES MY_FLAG SPACED SC_FG SC_FG_MULT SC_FG_ADAPT SC_FG_PANEL SC_FG_PANEL_KEY SC_FG_GAPFILL SC_FG_FILL MANGOHUD SC_FG_EXPERIMENTAL WINE_SC_THP LIST PATHS PROTON_ENABLE_NGX_UPDATER \\
           DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE \\
           DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION \\
           DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS VK_LOADER_LAYERS_ENABLE VK_VRAM_REPORT_LIMIT_HEAP_SIZE VK_VRAM_REPORT_LIMIT_DEVICE_ID \\
           VK_ADD_LAYER_PATH NVPRESENT_ENABLE_SMOOTH_MOTION DISABLE_LAYER_NV_PRESENT_1 DISABLE_LSFGVK DISABLE_LSFG VKD3D_FRAME_RATE DXVK_FRAME_RATE MANGOHUD_CONFIG; do
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
    # (the loading screens' follower: a stand-in that writes down how it was started)
    printf '#!/usr/bin/env bash\necho "$*" > "%s/loading-follow"\n' "$work" > "$t/tools/sc-loading"; chmod +x "$t/tools/sc-loading"
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
    turbo ./sc-turbo run
    ! grep -q "more than one NVIDIA card" "$work/turbo.txt"; check $? "one card: no word about several"
    turbo env CARD="12282, 0x278310DE, NVIDIA GeForce RTX 4070 SUPER
4096, 0x1C8110DE, NVIDIA GeForce GTX 1050" ./sc-turbo run
    called "VK_VRAM_REPORT_LIMIT_HEAP_SIZE=9216" && called "VK_VRAM_REPORT_LIMIT_DEVICE_ID=0x10de:0x2783" \
        && grep -q "more than one NVIDIA card here. The limit is for the first one nvidia-smi names, NVIDIA GeForce RTX 4070 SUPER" "$work/turbo.txt"; check $? "two different cards: the limit is for the first, and that is said"
    turbo env CARD="12282, 0x278310DE, NVIDIA GeForce RTX 4070 SUPER
12282, 0x278310DE, NVIDIA GeForce RTX 4070 SUPER" ./sc-turbo run
    called "VK_VRAM_REPORT_LIMIT_HEAP_SIZE=9216" && ! grep -q "more than one NVIDIA card" "$work/turbo.txt"; check $? "two cards of one kind: the limit fits both, nothing more is said"
    turbo env CARD="16376, 0x270410DE, NVIDIA GeForce RTX 4080
4096, 0x1C8110DE, NVIDIA GeForce GTX 1050" ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && ! grep -q "more than one NVIDIA card" "$work/turbo.txt"; check $? "no limit placed: no word about several cards either"
    printf '#!/usr/bin/env bash\nexit 9\n' > "$work/fakebin/nvidia-smi"
    turbo ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && called "runner=stock" && grep -q "NOTE: the graphics memory limit is not set: nvidia-smi named no card" "$work/turbo.txt"; check $? "nvidia-smi gives no card: nothing placed, that is said, the game starts"
    turbo env SC_VRAM_LIMIT=off ./sc-turbo run
    ! grep -q "NOTE: the graphics" "$work/turbo.txt"; check $? "SC_VRAM_LIMIT=off ends the note"
    # a machine without the driver's tool: the system's programs, all but that one
    rm "$work/fakebin/nvidia-smi"; mkdir -p "$work/nosmi"
    find /usr/bin -maxdepth 1 ! -name nvidia-smi ! -name bin -exec ln -s -t "$work/nosmi" {} + 2>/dev/null
    turbo env PATH="$work/fakebin:$work/nosmi" ./sc-turbo run
    ! grep -q '^VK_' "$work/prefix/called" && called "runner=stock" && grep -q "nvidia-smi, which names the card and its memory, was" "$work/turbo.txt"; check $? "no nvidia-smi at all: nothing placed, that is said, the game starts"
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
    echo "8. The multiplier's words, and the layer's settings from sc-turbo.conf"
    turbo ./sc-turbo fg 4
    called "SC_FG_MULT=4" && ! grep -q '^SC_FG_ADAPT=' "$work/prefix/called" && ! grep -q "held at" "$work/turbo.txt"; check $? "\"fg 4\": up to x4, following the game's rate"
    turbo ./sc-turbo fg 3 fixed panel
    called "SC_FG_MULT=3" && called "SC_FG_ADAPT=0" && called "SC_FG_PANEL=1" && grep -q "held at x3" "$work/turbo.txt"; check $? "\"fg 3 fixed panel\": x3, held, and that is said"
    turbo ./sc-turbo fg fixed
    called "SC_FG_MULT=2" && called "SC_FG_ADAPT=0"; check $? "the number is optional: \"fg fixed\" is x2, held"
    turbo ./sc-turbo fg panel
    called "SC_FG_MULT=2" && called "SC_FG_PANEL=1"; check $? "a word in the number's place is taken as a word"
    turbo ./sc-turbo fg 3 nonsense
    [ ! -e "$work/prefix/called" ] && grep -q '"nonsense" is not known' "$work/turbo.txt"; check $? "a word that is not known: nothing is started"
    turbo ./sc-turbo fg 6
    [ ! -e "$work/prefix/called" ] && grep -q "x6 is experimental: SC_FG_EXPERIMENTAL=1" "$work/turbo.txt"; check $? "\"fg 6\" without the experimental setting: nothing is started, and why is said"
    turbo ./sc-turbo fg 9
    [ ! -e "$work/prefix/called" ] && grep -q "the multiplier is 2, 3 or 4 (5 to 8 with the experimental setting)" "$work/turbo.txt"; check $? "\"fg 9\": nothing is started"
    echo 'export SC_FG_EXPERIMENTAL=1' >> "$t/sc-turbo.conf"
    turbo ./sc-turbo fg 6
    called "SC_FG_MULT=6" && called "SC_FG_EXPERIMENTAL=1" && grep -q "Experimental (x5 and above)" "$work/turbo.txt"; check $? "with SC_FG_EXPERIMENTAL=1: x6, the setting reaches the layer, and a note says what it is"
    turbo ./sc-turbo fg 4
    called "SC_FG_EXPERIMENTAL=1" && ! grep -q "Experimental (x5" "$work/turbo.txt"; check $? "x4 with the setting: the layer still makes room for more (the limit can be raised in the game), no note"
    sed -i '/SC_FG_EXPERIMENTAL/d' "$t/sc-turbo.conf"
    turbo ./sc-turbo fg 4
    ! grep -q '^SC_FG_EXPERIMENTAL=' "$work/prefix/called"; check $? "without the setting nothing of it reaches the launch"
    [ ! -e "$t/logs/fg/HUDOFF" ]; check $? "the HUD is kept apart unless the settings say otherwise"
    printf 'SC_FG_KEEP_HUD=0\nexport SC_FG_GAPFILL=0\n' >> "$t/sc-turbo.conf"
    turbo ./sc-turbo fg 4
    [ -e "$t/logs/fg/HUDOFF" ] && grep -q "The HUD is not kept apart" "$work/turbo.txt" && called "SC_FG_GAPFILL=0"; check $? "SC_FG_KEEP_HUD=0 and an exported setting of the layer's reach the launch"
    sed -i '/SC_FG_KEEP_HUD/d; /SC_FG_GAPFILL/d' "$t/sc-turbo.conf"
    turbo ./sc-turbo fg 4
    [ ! -e "$t/logs/fg/HUDOFF" ]; check $? "and the next launch without it keeps the HUD apart again"
    called "SC_FG_FILL=0" && ! grep -q "fills the display" "$work/turbo.txt"; check $? "a launch leaves the game's frames unlimited (SC_FG_FILL=0) and says nothing about it"
    echo 'SC_FG_FILL=1' >> "$t/sc-turbo.conf"
    turbo ./sc-turbo fg 4
    called "SC_FG_FILL=1" && grep -q "fills the display evenly (SC_FG_FILL=1, the window's \"Limit the game's own frame rate\")" "$work/turbo.txt"; check $? "SC_FG_FILL=1 in sc-turbo.conf (\"Limit the game's own frame rate\"): the game's frames are limited to fill the display evenly, and that is said"
    # the prefetchers and huge pages through Myrddin's root helper: a stand-in that says whether it is set up
    printf '#!/usr/bin/env bash\ncase "$1" in allowed) exit "${TUNE_OK:-1}" ;; follow) shift; echo "$*" > "%s/tune-follow" ;; esac\n' "$work" > "$t/tools/sc-tune"
    chmod +x "$t/tools/sc-tune"; printf 'SC_PREFETCH=off\nSC_THP=on\n' >> "$t/sc-turbo.conf"
    turbo env TUNE_OK=0 ./sc-turbo fg 4; sleep 0.3
    [ "$(cat "$work/tune-follow" 2>/dev/null)" = "prefetch thp" ] && grep -q "While the game runs: the CPU's prefetchers off and huge pages on" "$work/turbo.txt"; check $? "SC_PREFETCH=off and SC_THP=on: one follower for both, and that is said"
    rm -f "$work/tune-follow"
    turbo ./sc-turbo run; sleep 0.3
    [ ! -e "$work/tune-follow" ] && grep -q "NOTE: the CPU's prefetchers off and huge pages on: not done, Myrddin's root helper is not set up" "$work/turbo.txt" && called "runner=stock"; check $? "without the helper (\"run\" too): nothing is switched, that is said, the game starts"
    sed -i '/SC_PREFETCH/d; /SC_THP/d' "$t/sc-turbo.conf"
    turbo env TUNE_OK=0 ./sc-turbo fg 4; sleep 0.3
    [ ! -e "$work/tune-follow" ] && ! grep -q "prefetchers" "$work/turbo.txt"; check $? "neither set: no follower, nothing said"
    turbo ./sc-turbo fg 3 fixed
    called "SC_FG_FILL=1" && ! grep -q "fills the display" "$work/turbo.txt"; check $? "with a multiplier that is held nothing is said about it, SC_FG_FILL=1 or not"
    sed -i '/SC_FG_FILL/d' "$t/sc-turbo.conf"
    # MangoHud with frame generation Off too (the window's overlay choice): "run hud", or SC_MANGOHUD=1
    mkdir -p "$work/home/.local/share/vulkan/implicit_layer.d"; : > "$work/home/.local/share/vulkan/implicit_layer.d/MangoHud.x86_64.json"
    turbo ./sc-turbo run hud
    called "MANGOHUD=1" && grep -q "MangoHud on: the frame rate it shows is the game's own" "$work/turbo.txt"; check $? "\"run hud\": the game as it is, with MangoHud"
    echo 'SC_MANGOHUD=1' >> "$t/sc-turbo.conf"
    turbo ./sc-turbo run
    called "MANGOHUD=1"; check $? "SC_MANGOHUD=1 in sc-turbo.conf: \"run\" has MangoHud as well"
    turbo ./sc-turbo fg 4
    called "MANGOHUD=1" && grep -q "generated frames included" "$work/turbo.txt"; check $? "and \"fg\", below the layer, as before"
    sed -i '/SC_MANGOHUD/d' "$t/sc-turbo.conf"
    turbo ./sc-turbo run
    ! grep -q '^MANGOHUD=' "$work/prefix/called"; check $? "without either: no MangoHud"
    rm -f "$work/home/.local/share/vulkan/implicit_layer.d/MangoHud.x86_64.json"
    # the panel with generation off (the window's Off with Myrddin's panel)
    turbo ./sc-turbo fg 4 panel off
    called "SC_FG=1" && called "SC_FG_PANEL=1" && [ -e "$t/logs/fg/OFF" ] && grep -q "Frame generation off at the start" "$work/turbo.txt"; check $? "\"fg 4 panel off\": the layer and its panel, generation off from the start (the OFF file), and that is said"
    turbo ./sc-turbo fg 4
    [ ! -e "$t/logs/fg/OFF" ]; check $? "the next launch without \"off\" generates again"
    # the loading screens' follower (the stand-in made with the scratch copy)
    rm -f "$work/loading-follow"; : > "$t/logs/fg/LOADING"
    turbo ./sc-turbo fg 4; sleep 0.3
    [ "$(cat "$work/loading-follow" 2>/dev/null)" = "follow $work/prefix $t/logs/fg" ] && [ ! -e "$t/logs/fg/LOADING" ]
    check $? "\"fg\" starts the loading screens' follower for the prefix and the layer's folder, and clears an old LOADING"
    grep -q "It starts at x3 and takes x3 at most while the game shows a loading screen" "$work/turbo.txt"; check $? "and says what a loading screen does to Auto up to x4"
    rm -f "$work/loading-follow"
    turbo ./sc-turbo fg 3; sleep 0.3
    [ -e "$work/loading-follow" ] && ! grep -q "loading screen" "$work/turbo.txt"; check $? "up to x3 the follower runs as well, and nothing is said"
    turbo ./sc-turbo fg 4 fixed
    ! grep -q "loading screen" "$work/turbo.txt"; check $? "a held x4: nothing said either"
    rm -f "$work/loading-follow"
    turbo ./sc-turbo run; sleep 0.3
    [ ! -e "$work/loading-follow" ]; check $? "\"run\" (no layer): no follower"
    echo "9. In a running game: the multiplier and parameters (control files; no game is needed to write them)"
    ( cd "$t" && ./sc-turbo fg-mult 3 fixed > "$work/turbo.txt" 2>&1 )
    [ "$(cat "$t/logs/fg/MULT")" = 3 ] && grep -qx 'adapt=0' "$t/logs/fg/PARAMS" && grep -q "held there" "$work/turbo.txt"; check $? "\"fg-mult 3 fixed\": the multiplier and the word that holds it"
    ( cd "$t" && ./sc-turbo fg-mult 4 auto > "$work/turbo.txt" 2>&1 )
    [ "$(cat "$t/logs/fg/MULT")" = 4 ] && [ "$(tr '\n' ' ' < "$t/logs/fg/PARAMS")" = "adapt=0 adapt=1 " ]; check $? "\"fg-mult 4 auto\": what the layer has not read yet is kept, the later word counts"
    rm -f "$t/logs/fg/PARAMS" "$t/logs/fg/MULT"
    ( cd "$t" && ./sc-turbo fg-mult 2 > "$work/turbo.txt" 2>&1 )
    [ "$(cat "$t/logs/fg/MULT")" = 2 ] && [ ! -e "$t/logs/fg/PARAMS" ]; check $? "\"fg-mult 2\" alone leaves held or following as it is"
    ( cd "$t" && ./sc-turbo fg-mult 7 > "$work/turbo.txt" 2>&1 ); [ $? != 0 ] && [ "$(cat "$t/logs/fg/MULT")" = 2 ]; check $? "a multiplier outside 2 to 4 is refused"
    grep -q "fg-mult: x7 is experimental" "$work/turbo.txt"; check $? "and 5 to 8 are named as the experimental setting's"
    ( cd "$t" && SC_FG_EXPERIMENTAL=1 ./sc-turbo fg-mult 7 > "$work/turbo.txt" 2>&1 ); [ "$(cat "$t/logs/fg/MULT")" = 7 ]; check $? "with the setting, \"fg-mult 7\" is passed on"
    ( cd "$t" && SC_FG_EXPERIMENTAL=1 ./sc-turbo fg-mult 9 > "$work/turbo.txt" 2>&1 ); [ $? != 0 ] && [ "$(cat "$t/logs/fg/MULT")" = 7 ]; check $? "and 9 is refused even then"
    ( cd "$t" && ./sc-turbo fg-mult 2 > "$work/turbo.txt" 2>&1 )
    ( cd "$t" && ./sc-turbo fg-mult 3 sometimes > "$work/turbo.txt" 2>&1 ); [ $? != 0 ] && [ "$(cat "$t/logs/fg/MULT")" = 2 ]; check $? "and so is a word that is not fixed or auto"

    echo "10. The panel at launch and its key; what a launch starts leaves no core dump"
    rm -f "$t/logs/fg/PANEL"
    turbo ./sc-turbo fg 4 panel
    [ "$(cat "$t/logs/fg/PANEL")" = "1 tl Shift+7" ] && called "SC_FG_PANEL_KEY=Shift+7" && grep -q "The performance panel is shown; Shift+7" "$work/turbo.txt"
    check $? "a launch with the panel shows it from the start (step 1), with Shift+7 as its key, and says so"
    called "core=0"; check $? "what the launch starts is not allowed a core dump"
    echo "3 br" > "$t/logs/fg/PANEL"; turbo ./sc-turbo fg 4 panel
    [ "$(cat "$t/logs/fg/PANEL")" = "3 br Shift+7" ]; check $? "the step and the corner it was left at are kept"
    echo "0 tr Ctrl+F9" > "$t/logs/fg/PANEL"; turbo ./sc-turbo fg 4 panel
    [ "$(cat "$t/logs/fg/PANEL")" = "1 tr Shift+7" ]; check $? "a panel that was stepped off is back at the next launch; the key is the settings' one, not the last session's"
    echo 'SC_FG_PANEL_KEY=Ctrl+F9' >> "$t/sc-turbo.conf"; turbo ./sc-turbo fg 4 panel
    [ "$(cat "$t/logs/fg/PANEL")" = "1 tr Ctrl+F9" ] && called "SC_FG_PANEL_KEY=Ctrl+F9"; check $? "SC_FG_PANEL_KEY in sc-turbo.conf names the key"
    sed -i 's/^SC_FG_PANEL_KEY=.*/SC_FG_PANEL_KEY=Super+7/' "$t/sc-turbo.conf"; turbo ./sc-turbo fg 4 panel
    called "SC_FG_PANEL_KEY=Shift+7" && grep -q "SC_FG_PANEL_KEY=Super+7 is no key the panel knows" "$work/turbo.txt"; check $? "a key the panel does not know (a Super key) is said and not used"
    sed -i '/SC_FG_PANEL_KEY/d' "$t/sc-turbo.conf"
    echo "2 bl" > "$t/logs/fg/PANEL"; turbo ./sc-turbo fg 4
    [ "$(cat "$t/logs/fg/PANEL")" = "2 bl" ] && ! grep -q '^SC_FG_PANEL' "$work/prefix/called"; check $? "a launch without the panel leaves all of that alone"
    ( cd "$t" && ./sc-turbo fg-panel key=Alt+Home > "$work/turbo.txt" 2>&1 )
    [ "$(cat "$t/logs/fg/PANEL")" = "2 bl Alt+Home" ] && grep -q "panel key: Alt+Home" "$work/turbo.txt"; check $? "\"fg-panel key=Alt+Home\": the key for the running game, the step as it was"
    ( cd "$t" && ./sc-turbo fg-panel > /dev/null 2>&1 && ./sc-turbo fg-panel tr > /dev/null 2>&1 )
    [ "$(cat "$t/logs/fg/PANEL")" = "3 tr Alt+Home" ]; check $? "a step and a corner from outside keep the key"
    ( cd "$t" && ./sc-turbo fg-panel key=Super+P > "$work/turbo.txt" 2>&1 ); [ $? != 0 ] && [ "$(cat "$t/logs/fg/PANEL")" = "3 tr Alt+Home" ]
    check $? "a key the panel does not know is refused"

    echo "10b. the game's own upscaler setting, said at a launch when it is not DLSS"
    profile="$work/prefix/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/user/client/0/Profiles/default"
    mkdir -p "$profile" "$work/prefix/drive_c/windows/system32"
    printf '<Attributes Version="1">\n <Attr name="UpscalingTechnique" value="0"/>\n</Attributes>\n' > "$profile/attributes.xml"
    turbo ./sc-turbo fg 4
    grep -q "the game's upscaler is not set to DLSS" "$work/turbo.txt"; check $? "another upscaler in the game's settings: a note"
    sed -i 's/value="0"/value="2"/' "$profile/attributes.xml"
    turbo ./sc-turbo fg 4
    ! grep -q "upscaler is not set" "$work/turbo.txt"; check $? "DLSS there: nothing is said"
    sed -i '/UpscalingTechnique/d' "$profile/attributes.xml"
    turbo ./sc-turbo fg 4
    ! grep -q "upscaler is not set" "$work/turbo.txt"; check $? "no upscaler named in the settings: nothing is said (not known)"
    rm -rf "$work/prefix/drive_c"

    echo "11. stop and kill: Wine's processes of this prefix, and nothing else"
    # stand-ins: programs named like Wine's, started with the prefix in their environment
    mkdir -p "$work/winebin"; cp "$(command -v sleep)" "$work/winebin/wine64-preloader"; cp "$(command -v sleep)" "$work/winebin/wineserver"
    cp "$(command -v bash)" "$work/winebin/wine"; mkfifo "$work/hold"
    ( cd "$t" && env -i PATH="$PATH" SC_PREFIX="$work/prefix" SC_LAUNCH="$work/sc-launch.sh" ./sc-turbo stop > "$work/turbo.txt" 2>&1 )
    grep -q "nothing of the prefix is running" "$work/turbo.txt"; check $? "with nothing running, stop says so"
    WINEPREFIX="$work/prefix" "$work/winebin/wine64-preloader" 300 & p1=$!
    WINEPREFIX="$work/prefix/" "$work/winebin/wine64-preloader" 300 & p2=$!          # (the same folder, written differently)
    WINEPREFIX="$work/prefix" "$work/winebin/wineserver" 300 & p3=$!
    WINEPREFIX="$work/prefix" "$work/winebin/wine" -c 'trap "" TERM; exec 3<>"$1"; read -u 3' _ "$work/hold" & p4=$!   # (does not end when asked)
    WINEPREFIX="$work/other-prefix" "$work/winebin/wine64-preloader" 300 & d1=$!    # another prefix
    WINEPREFIX="$work/prefix" sleep 300 & d2=$!                                      # this prefix, but no program of Wine's
    sleep 0.5
    ( cd "$t" && env -i PATH="$PATH" SC_PREFIX="$work/prefix" SC_LAUNCH="$work/sc-launch.sh" ./sc-turbo stop > "$work/turbo.txt" 2>&1 ); rc=$?
    alive() { kill -0 "$1" 2>/dev/null; }
    [ $rc = 1 ] && ! alive $p1 && ! alive $p2 && ! alive $p3 && alive $p4 && grep -q "1 of 4 processes of the prefix are still there" "$work/turbo.txt"
    check $? "stop ends what ends when asked, Wine's server too, and says what is still there"
    alive $d1 && alive $d2; check $? "another prefix's processes and programs that are not Wine's are left alone"
    ( cd "$t" && env -i PATH="$PATH" SC_PREFIX="$work/prefix" SC_LAUNCH="$work/sc-launch.sh" ./sc-turbo kill > "$work/turbo.txt" 2>&1 ); rc=$?
    [ $rc = 0 ] && ! alive $p4 && grep -q "ended: 1 processes of the prefix" "$work/turbo.txt"; check $? "kill ends the one that stayed"
    alive $d1 && alive $d2; check $? "and still nothing else"
    kill $d1 $d2 2>/dev/null; wait 2>/dev/null
    echo "12. Other frame generators and frame limits: kept off at a launch with frame generation, and named"
    script "$work/sc-launch.sh" 'export NVPRESENT_ENABLE_SMOOTH_MOTION=1
export VKD3D_FRAME_RATE="60"
# export DXVK_FRAME_RATE=30   (a comment: not counted)'
    before="$(sha256sum < "$work/sc-launch.sh")"
    turbo ./sc-turbo fg 2
    called "DISABLE_LAYER_NV_PRESENT_1=1" && called "VKD3D_FRAME_RATE=0" && called "SC_FG=1"; check $? "Smooth Motion is kept off and vkd3d-proton's limit set to none, the script's own lines notwithstanding"
    grep -q "NOTE: NVIDIA Smooth Motion (the driver's frame generation) is switched on in the launch script" "$work/turbo.txt" \
        && grep -q "NOTE: VKD3D_FRAME_RATE=60 (a frame-rate limit) is set in the launch script" "$work/turbo.txt"; check $? "and both are named before the launch"
    ! grep -q "DXVK_FRAME_RATE" "$work/turbo.txt" && ! grep -q "^DXVK_FRAME_RATE" "$work/prefix/called"; check $? "a limit in a comment is not counted"
    [ "$(sha256sum < "$work/sc-launch.sh")" = "$before" ]; check $? "the launch script itself is left as it was"
    turbo ./sc-turbo run
    ! grep -q "^DISABLE_LAYER_NV_PRESENT_1" "$work/prefix/called" && called "VKD3D_FRAME_RATE=60" && ! grep -q "Smooth Motion" "$work/turbo.txt"; check $? "without frame generation (\"run\", the window's Off) both stay as the script sets them"
    turbo ./sc-turbo others
    grep -qx "generator	NVIDIA Smooth Motion (the driver's frame generation)	the launch script" "$work/turbo.txt" \
        && grep -qx "limiter	VKD3D_FRAME_RATE=60 (a frame-rate limit)	the launch script" "$work/turbo.txt"; check $? "\"others\" lists them for the window"
    turbo env SC_LAUNCH_COPY=0 ./sc-turbo fg 2
    called "DISABLE_LAYER_NV_PRESENT_1=1" && called "VKD3D_FRAME_RATE=60" && grep -q "without a copy of the launch script (SC_LAUNCH_COPY=0) it stays" "$work/turbo.txt"; check $? "without a copy: Smooth Motion still off, the script's limit stays, and that is said"
    script "$work/sc-launch.sh" ""
    mkdir -p "$work/home/.config/lsfg-vk"; printf 'version = 2\n[[profile]]\nname = "sc"\nactive_in = ["StarCitizen.exe"]\nmultiplier = 2\n' > "$work/home/.config/lsfg-vk/conf.toml"
    turbo ./sc-turbo others
    grep -qx "generator	lsfg-vk (Lossless Scaling's frame generation)	~/.config/lsfg-vk/conf.toml (a profile for Star Citizen)" "$work/turbo.txt"; check $? "lsfg-vk set up for the game in its own settings is found"
    turbo ./sc-turbo fg 2
    called "DISABLE_LSFGVK=1" && called "DISABLE_LSFG=1" && grep -q "NOTE: lsfg-vk (Lossless Scaling's frame generation) is switched on in ~/.config/lsfg-vk/conf.toml" "$work/turbo.txt"; check $? "and kept off at a launch with frame generation (both its versions), and named"
    printf 'version = 2\n[[profile]]\nname = "other"\nactive_in = ["ocarina.exe"]\n' > "$work/home/.config/lsfg-vk/conf.toml"
    turbo ./sc-turbo others
    [ ! -s "$work/turbo.txt" ]; check $? "lsfg-vk set up for other games only, and a script that asks for nothing: nothing listed"
    turbo env NVPRESENT_ENABLE_SMOOTH_MOTION=1 ./sc-turbo others
    grep -qx "generator	NVIDIA Smooth Motion (the driver's frame generation)	the environment" "$work/turbo.txt"; check $? "Smooth Motion from the environment is found as well"
    turbo env NVPRESENT_ENABLE_SMOOTH_MOTION=0 ./sc-turbo others
    [ ! -s "$work/turbo.txt" ]; check $? "NVPRESENT_ENABLE_SMOOTH_MOTION=0 does not switch it on: not listed"
    turbo env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation:VK_LAYER_NV_present ./sc-turbo others
    grep -qx "forced	VK_INSTANCE_LAYERS	the environment" "$work/turbo.txt"; check $? "a layer list that forces one of them on is named"
    turbo env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./sc-turbo others
    [ ! -s "$work/turbo.txt" ]; check $? "a layer list without them is not"
    turbo ./sc-turbo fg 2
    called "MANGOHUD_CONFIG=read_cfg,fps_limit=0"; check $? "MangoHud's own limit is none at a launch with frame generation, its settings file still read"
    turbo env MANGOHUD_CONFIG=fps_only ./sc-turbo fg 2
    called "MANGOHUD_CONFIG=fps_only,fps_limit=0"; check $? "a MANGOHUD_CONFIG of the user's own gets the limit added at its end"
    turbo ./sc-turbo run
    ! grep -q "^MANGOHUD_CONFIG" "$work/prefix/called"; check $? "\"run\" leaves MangoHud's settings alone"
    script "$work/sc-launch.sh" 'export NVPRESENT_ENABLE_SMOOTH_MOTION=1'
    turbo ./sc-turbo fg 2
    called "NVPRESENT_ENABLE_SMOOTH_MOTION=0" && called "DISABLE_LAYER_NV_PRESENT_1=1"; check $? "Smooth Motion asked for in the script: the copy also sets its own switch to 0"
fi

echo
if [ "$fails" = 0 ]; then echo "all checks passed"; else echo "$fails check(s) FAILED (work folder: $work)"; fi
exit "$((fails > 0))"
