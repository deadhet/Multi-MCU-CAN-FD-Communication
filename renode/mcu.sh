#!/usr/bin/env bash
# One MCU's terminal for the live network (start renode/network.sh first):
#   renode/mcu.sh 1|2|3|4
# Shows that MCU's own log of changes as they happen and takes commands for it (type help):
# halt [s], resume, reset, unplug [s], plug, status, time, wait <s>, run <step file>, raw <command>;
# start / pause for the whole network. Not a shell: sequences go in step files (renode/steps/).
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-}" in
1|2|3|4) ;;
*) echo "usage: $0 1|2|3|4" >&2; exit 2 ;;
esac
(cd "$repo/dashboard" && uv sync --frozen --quiet) || exit 1
exec "$repo/dashboard/.venv/bin/python" "$repo/renode/console.py" "$1"
