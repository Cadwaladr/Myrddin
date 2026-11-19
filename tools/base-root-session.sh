#!/usr/bin/env bash
# One approval for a measured session with huge pages. Run through sudo while the game is closed:
#
#   sudo tools/base-root-session.sh thp-on OUT_DIR [SECONDS=10] [STRETCHES=3]
#
# It switches transparent huge pages on for every program (tools/base-thp-root.sh on: only the kernel's
# settings under /sys/kernel/mm/transparent_hugepage are written), then waits for the game and records
# as "tools/base-trace-root.sh wait" does (read-only). Huge pages stay on until the next boot or until
# "sudo tools/base-thp-root.sh off". Nothing is attached to the game and nothing in it is touched.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
[ "${1:-}" = thp-on ] && [ -n "${2:-}" ] || { sed -n 2,10p "$0"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run this through sudo"; exit 1; }
shift
"$here/tools/base-thp-root.sh" on || exit 1
exec "$here/tools/base-trace-root.sh" wait "$@"
