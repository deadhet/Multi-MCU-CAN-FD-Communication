#!/usr/bin/env bash
# End-to-end check of the UART link: runs scenario S1 in Renode, captures MCU1's USART3 from
# TCP port 3456 while it runs, decodes every frame (COBS, CRC, header, Protobuf), and compares
# each snapshot with MCU1's debug report at the same millisecond. Prints UART LINK PASS or FAIL.
#   renode/check_uart_link.sh
# Build first (cmake --build build); do not rebuild while this runs.
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$repo/logs"
rm -f "$repo"/logs/mcu[1-4].log
timeout 600 renode --disable-gui --console -e "include @$repo/renode/s1_normal.resc" </dev/null >"$repo/logs/uart_link.console" 2>&1 &
renode_pid=$!
(cd "$repo/dashboard" && uv run --frozen python -m noccarc_dashboard.capture --port 3456 --compare-log "$repo/logs/mcu1.log")
status=$?
wait "$renode_pid"
exit "$status"
