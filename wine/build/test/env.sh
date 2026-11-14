# Isolated test environment: separate prefix + wineserver, no display, no desktop integration.
# Sourced by run_test.sh; the paths follow this file, wherever the project lives.
unset DISPLAY WAYLAND_DISPLAY
_test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export WINEPREFIX="$_test_dir/prefix"
export WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree=d;mshtml=d;winewayland.drv=d;winex11.drv=d"
export WINEDEBUG=-all
# The stock runner (SC_RUNNER, or the 11.20-2 LUG runner in the game's prefix: SC_PREFIX, see
# sc-turbo.conf.example) and the patched copy of it that make-patched-runner.sh made.
_project="$(cd "$_test_dir/../../.." && pwd)"
_env_prefix="${SC_PREFIX:-}"; _env_runner="${SC_RUNNER:-}"
[ -f "$_project/sc-turbo.conf" ] && . "$_project/sc-turbo.conf"
SC_PREFIX="${_env_prefix:-${SC_PREFIX:-$HOME/Games/star-citizen}}"
ORIG="${_env_runner:-${SC_RUNNER:-$SC_PREFIX/runners/lug-wine-tkg-experimental-wayland-git-11.20-2}}"
PATCHED="$(cd "$_test_dir/../.." && pwd)/runners/$(basename "$ORIG")-cpusets"
