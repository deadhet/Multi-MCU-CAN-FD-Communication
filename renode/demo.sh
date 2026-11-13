#!/usr/bin/env bash
# A scripted run with the dashboard: opens the dashboard in the browser, then runs a scenario.
#   renode/demo.sh                 demo.resc: MCU3 cable pulled, MCU2 halted and reset, MCU1
#                                  halted and reset (about 2.5 minutes)
#   renode/demo.sh s2              one of the Design Document scenarios: s1 s2 s3 s4
#   renode/demo.sh s3 --no-browser for recording with your own browser window
# The dashboard stays up after Renode ends, showing the link DISCONNECTED and the last data
# greyed; Ctrl-C stops it. To drive the network by hand instead: renode/network.sh.
# Build first (cmake --build build).
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-demo}" in
demo) script=demo.resc ;;
s1) script=s1_normal.resc ;;
s2) script=s2_slave_failure.resc ;;
s3) script=s3_disconnect.resc ;;
s4) script=s4_master_failure.resc ;;
--*) script=demo.resc; set -- demo "$@" ;;
*) echo "usage: $0 [demo|s1|s2|s3|s4] [--no-browser]" >&2; exit 2 ;;
esac
shift
mkdir -p "$repo/logs"
rm -f "$repo"/logs/mcu[1-4].log "$repo"/logs/mcu[1-4].log.*

# The project's own uv environment, run directly (not through "uv run"), so that stopping this
# script stops the dashboard too: one process, no wrapper in between.
(cd "$repo/dashboard" && uv sync --frozen --quiet) || exit 1
(cd "$repo/dashboard" && exec .venv/bin/python -m noccarc_dashboard.server --port 3456 --lost-ms 1500 "$@") &
dashboard=$!
trap 'kill "$dashboard" 2>/dev/null' EXIT

# Start Renode only once the dashboard answers (it reads the link from Renode's first byte on).
for _ in $(seq 50); do
    curl -s -o /dev/null http://127.0.0.1:8765/initial.js && break
    kill -0 "$dashboard" 2>/dev/null || exit 1   # it failed to start; its message says why
    sleep 0.2
done

echo "Running renode/$script ..."
renode --disable-gui --console -e "include @$repo/renode/$script" </dev/null >"$repo/logs/demo.console" 2>&1
echo "Renode finished (logs/). The dashboard keeps running: Ctrl-C to stop it."
wait "$dashboard"
