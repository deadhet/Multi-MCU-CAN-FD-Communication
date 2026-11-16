#!/usr/bin/env bash
# Counter-overflow test. The system uses 32-bit counters that wrap only after a very long uptime:
# the millisecond clock after 49.7 days, the CAN parameter-block sequence after 497 days, the CAN
# heartbeat sequence after 13.6 years, the UART frame sequence later still. A real run can never
# reach those, so this test starts each counter just below its maximum, so every wrap happens in
# the first few seconds, and checks that nothing misbehaves across the wrap.
#
# It edits three real source files IN PLACE, builds, runs four nodes in Renode, then restores the
# files (an EXIT trap restores them even on error or Ctrl-C). The committed source is unchanged;
# `git diff` is empty when it finishes. It needs the port 3456 free (no other Renode running).
#
# Wraps forced (counter -> value at boot -> wraps about):
#   ms clock (systick.c)      0xFFFFF000  ~4.1 s   scheduler due times, heartbeat uptime, ages
#   CAN param seq (comm.c)    0xFFFFFFCE  ~0.5 s   receiver check 7 (sequence difference)
#   CAN heartbeat seq         0xFFFFFFCE  ~5 s     check 7 and the restart rule
#   UART frame seq (monitor)  0xFFFFFFE2  ~3 s     the host's sequence-gap and uptime unwrap
#
# Pass: once each node is OPERATIONAL it stays so with synced=20 across the 2^32 clock crossing
# (no OFFLINE, LATE, RECOVERING or RESTART after it syncs), and the host sees 0 lost, 0 missing
# UART frames with the uptime correctly unwrapped past the wrap. (The injection starts the clock
# near max at boot, so every node briefly reports its peers OFFLINE for the first few ms -- an
# artifact of a non-zero boot clock, which never happens on a real node that boots at ms=0.)
#   renode/tests/counter_overflow.sh
# Run time on the development PC: about 1.5 min (build ~30 s, 12 s simulated at ~0.3x, capture).
# Results are in test_report.md, so this need not run during a presentation.
set -u

repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
systick="firmware/common/drivers/systick.c"
comm="firmware/common/comm/comm.c"
monitor="firmware/common/monitor/monitor.c"
bak="$(mktemp -d)"

restore() {
    cp "$bak/systick.c" "$systick"
    cp "$bak/comm.c"    "$comm"
    cp "$bak/monitor.c" "$monitor"
    rm -rf "$bak"
}
trap restore EXIT INT TERM

cp "$systick" "$bak/systick.c"
cp "$comm"    "$bak/comm.c"
cp "$monitor" "$bak/monitor.c"

# Replace one exact line (must occur exactly once) with new text; abort if it does not.
inject() { # file old new
    FILE="$1" OLD="$2" NEW="$3" perl -0pi -e '
        my ($o, $n) = ($ENV{OLD}, $ENV{NEW});
        my $c = () = /\Q$o\E/g;
        die "anchor \"$o\" found $c times in $ENV{FILE}\n" unless $c == 1;
        s/\Q$o\E/$n/;' "$1" || { echo "inject failed" >&2; exit 3; }
}

inject "$systick" \
  'static volatile uint32_t ms_counter;' \
  'static volatile uint32_t ms_counter = 0xFFFFF000U; /* COUNTER-OVERFLOW TEST: wraps ~4.1 s */'
inject "$comm" \
  'static uint32_t tx_seq[COMM_MSG_COUNT];' \
  'static uint32_t tx_seq[COMM_MSG_COUNT] = { 0xFFFFFFCEU, 0xFFFFFFCEU }; /* COUNTER-OVERFLOW TEST */'
inject "$monitor" \
  'static uint32_t uart_seq; /* Last sequence used; the first frame is 1 */' \
  'static uint32_t uart_seq = 0xFFFFFFE2U; /* COUNTER-OVERFLOW TEST: wraps ~3 s */'

echo "## building with the counters near their maximum"
if ! cmake --build build > "$repo/logs/overflow_build.txt" 2>&1; then
    echo "build failed (logs/overflow_build.txt):" >&2; tail -5 "$repo/logs/overflow_build.txt" >&2; exit 4
fi

# Run 12 s simulated, long enough for every wrap, and capture MCU1's UART link while it runs.
cat > "$repo/renode/overflow_run.resc" <<EOF
include \$ORIGIN/system.resc
emulation RunFor "12.0"
quit
EOF
echo "## running four nodes for 12 s simulated (about 40 s real)"
timeout 300 renode --disable-gui --console -e "include @$repo/renode/overflow_run.resc" </dev/null \
    > "$repo/logs/overflow_console.txt" 2>&1 &
rpid=$!
cap=$(cd "$repo/dashboard" && uv run --frozen python -m noccarc_dashboard.capture \
        --port 3456 --lost-ms 1500 --connect-timeout 60 --compare-log "$repo/logs/mcu1.log" 2>&1)
wait "$rpid"
rm -f "$repo/renode/overflow_run.resc"

echo
echo "===== firmware logs (every node) ====="
# The injection starts the ms clock near its maximum AT BOOT. A real node boots at ms=0, so for
# the first few ms each peer's "time since last heartbeat" (its timer still 0) reads as ~49.7
# days and the node briefly reports the peers OFFLINE, until the first heartbeats arrive. That
# boot instant is an artifact of forcing the clock high at boot, not the wrap under test. What
# the wrap test checks is: once the node reaches OPERATIONAL, it stays there with synced=20
# across the 2^32 crossing -- no OFFLINE, LATE, RECOVERING or RESTART from that point on.
fail=0
for n in 1 2 3 4; do
    log="$repo/logs/mcu$n.log"
    op=$(grep -nE 'diag self OPERATIONAL' "$log" | head -1 | cut -d: -f1)
    last=$(grep -E ' db synced=' "$log" | tail -1)
    if [ -z "$op" ]; then
        echo "MCU$n: FAIL - never reached OPERATIONAL"; fail=1; continue
    fi
    post=$(tail -n +"$((op + 1))" "$log" \
        | grep -E 'diag n[0-9]+ (OFFLINE|LATE|RECOVERING)|diag self RESYNCING|RESTART' || true)
    synced_ok=$(echo "$last" | grep -c 'synced=20 valid=20 invalid=0 stale=0 never=0 offline=0' || true)
    if [ -n "$post" ] || [ "$synced_ok" != "1" ]; then
        echo "MCU$n: FAIL - disruption at/after the wrap, or not fully synced at the end:"
        [ -n "$post" ] && echo "$post" | sed 's/^/    /'
        echo "    last report: ${last#MCU$n }"
        fail=1
    else
        echo "MCU$n: OK - OPERATIONAL through the clock wrap, no state change after sync; last report: synced=20 offline=0"
    fi
done

echo
echo "===== host view of MCU1's UART link (across the frame-sequence and uptime wraps) ====="
echo "$cap" | grep -E 'frames [0-9]|MCU1 restarts|uptime at the last snapshot|EVENT REPLAY|replayed between'
if echo "$cap" | grep -q 'lost on the way 0)' && echo "$cap" | grep -q 'MCU1 restarts seen: 0'; then
    echo "host link: OK - 0 lost, 0 missing, 0 false restarts"
else
    echo "host link: CHECK - see the full capture above"; fail=1
fi

echo
[ "$fail" -eq 0 ] && echo "## COUNTER-OVERFLOW TEST PASS" || echo "## COUNTER-OVERFLOW TEST FAIL"

# Restore the source now and rebuild, so build/ holds the normal firmware again (the run above
# built it with the counters near max). The trap still restores on any earlier error path.
restore
trap - EXIT INT TERM
echo "## rebuilding build/ with the normal firmware"
cmake --build build > "$repo/logs/overflow_rebuild.txt" 2>&1 || echo "rebuild failed (logs/overflow_rebuild.txt)" >&2
exit "$fail"
