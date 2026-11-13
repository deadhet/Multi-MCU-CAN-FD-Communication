#!/usr/bin/env bash
# The live network, driven by hand: all four MCUs in one Renode (one CAN hub, one simulated
# clock, so the nodes stay in step), loaded and paused, plus the dashboard in the browser.
#   renode/network.sh                   this terminal shows Renode's messages and answers
#   renode/network.sh --no-dashboard
# Then one terminal per MCU:  renode/mcu.sh 1   renode/mcu.sh 2   renode/mcu.sh 3   renode/mcu.sh 4
# and type "start" in any of them. Ctrl-C here stops Renode and the dashboard.
#
# Commands reach Renode through a named pipe, logs/monitor.fifo (this user only); what Renode
# prints goes to logs/renode.out, where the MCU terminals read their answers. No network port is
# opened for control (Renode's TCP monitor, -P, would listen on every interface, and that
# monitor can run Python). The UART link for the dashboard stays on Renode's TCP port 3456.
# Build first (cmake --build build).
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
logs="$repo/logs"
mkdir -p "$logs"
rm -f "$logs"/mcu[1-4].log "$logs"/mcu[1-4].log.* "$logs/renode.out" "$logs/monitor.fifo"
mkfifo -m 600 "$logs/monitor.fifo"
exec 3<>"$logs/monitor.fifo"   # held open for the whole run: Renode never reaches the end of its input

dashboard=""
cleanup() {
    [ -n "$dashboard" ] && kill "$dashboard" 2>/dev/null
    rm -f "$logs/monitor.fifo"
}
trap cleanup EXIT

if [ "${1:-}" != "--no-dashboard" ]; then
    # The project's own uv environment, run directly so that stopping this script stops it.
    (cd "$repo/dashboard" && uv sync --frozen --quiet) || exit 1
    (cd "$repo/dashboard" && exec .venv/bin/python -m noccarc_dashboard.server --port 3456 --lost-ms 1500) &
    dashboard=$!
fi

cat <<MSG
Network: MCU1-MCU4 loaded and PAUSED.
Open four terminals:   renode/mcu.sh 1   renode/mcu.sh 2   renode/mcu.sh 3   renode/mcu.sh 4
Type "start" in any of them. Ctrl-C here stops everything.
MSG
# Everything goes to renode.out. This terminal leaves out Renode's start-up warnings (registers
# its models do not implement) and the time readouts the MCU terminals ask for during a timed
# halt or unplug (several a second).
renode --disable-gui --console --pid-file "$logs/renode.pid" -e "include @$repo/renode/system.resc" <&3 2>&1 |
    tee "$logs/renode.out" |
    awk '/\[WARNING\]/ { next }
         /emulation GetTimeSourceInfo/ { skip = 1; next }
         skip { if (/^Quantum:/) skip = 0; next }
         { print; fflush() }'
