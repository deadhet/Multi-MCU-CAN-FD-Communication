#!/usr/bin/env bash
# End-to-end check of the UART link: runs a scenario in Renode, captures MCU1's USART3 from TCP
# port 3456 while it runs, decodes every frame (COBS, CRC, header, Protobuf), and checks:
#   - each snapshot against MCU1's debug report at the same millisecond (values, synced count,
#     health, online count, own state, links);
#   - the events: replayed between two snapshots they must give the second one's states, and
#     each link, restart, own-state and owner-offline change MCU1 logs must have its event
#     (and the other way round) within 10 ms.
# Prints UART LINK PASS or FAIL.
#   renode/check_uart_link.sh         scenario S1 (normal)
#   renode/check_uart_link.sh s2      S2 (slave halted, reset), or s3 (disconnect)
# Not S4: MCU1 itself restarts there, and its log starts again at t=0.
# Build first (cmake --build build); do not rebuild while this runs.
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-s1}" in
s1) script=s1_normal.resc ;;
s2) script=s2_slave_failure.resc ;;
s3) script=s3_disconnect.resc ;;
*) echo "usage: $0 [s1|s2|s3]" >&2; exit 2 ;;
esac
mkdir -p "$repo/logs"
rm -f "$repo"/logs/mcu[1-4].log
timeout 900 renode --disable-gui --console -e "include @$repo/renode/$script" </dev/null >"$repo/logs/uart_link.console" 2>&1 &
renode_pid=$!
(cd "$repo/dashboard" && uv run --frozen python -m noccarc_dashboard.capture --port 3456 --compare-log "$repo/logs/mcu1.log")
status=$?
wait "$renode_pid"
exit "$status"
