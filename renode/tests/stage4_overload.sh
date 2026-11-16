#!/usr/bin/env bash
# Rx overload test. Copies a source tree to build/overload/<scenario>, makes MCU3 print which
# frames from node 1 it gets after an overload, builds, and runs 4 nodes in Renode.
#   stall:  MCU3's main loop is busy from 1.5 s to 2.0 s. The FDCAN interrupt still runs, so the
#           RAM queues overflow.
#   halt:   MCU3's CPU is halted from 1.5 s to 2.0 s. Nothing runs, so the 16-deep hardware FIFO
#           overflows.
#   babble: stall, and node 2 also floods valid parameter blocks, 20 per ms from 1.6 s to 1.9 s.
#           Node 2's queue on MCU3 must absorb it; the other queues must look as in stall.
# MCU3 then prints, for any pass of the main loop that drains more than 8 frames:
#   MCU3 DRAIN frames=<n> n1: <seq>/<lost before>/<age ms> ...
# and every later frame from node 1 that follows a gap:
#   MCU3 GAP n1 seq=<seq> lost=<n> age=<ms>
# Age is "now - arrival time", and arrival time is stamped in the FDCAN interrupt. In halt the
# interrupt cannot run either, so age there is only the time since the CPU resumed.
# The real source tree is never changed.
#   renode/tests/stage4_overload.sh <stall|halt|babble> [source tree, default: this repo]
set -euo pipefail

scenario="$1"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
src="${2:-$repo}"
dst="$repo/build/overload/$scenario"
rm -rf "$dst"
mkdir -p "$dst/logs"
cp -r "$src/CMakeLists.txt" "$src/cmake" "$src/firmware" "$src/renode" "$dst/"

app="$dst/firmware/nodes/slave/app_slave.c"

# Replaces one exact text (must occur once) with new text.
swap() { # file old new
    FILE="$1" OLD="$2" NEW="$3" perl -0pi -e '
        my ($o, $n) = ($ENV{OLD}, $ENV{NEW});
        my $c = () = /\Q$o\E/g;
        die "anchor found $c times\n" unless $c == 1;
        s/\Q$o\E/$n/;' "$1"
}

run_line='        sched_run_due();'
drain_lines=$'        while (comm_receive(&frame)) {\n            if (frame.type == COMM_MSG_PARAM) {\n                params_apply_block(frame.source, &frame.body.param, frame.sequence, frame.rx_time_ms);\n                diag_on_param_block(frame.source, frame.rx_time_ms);\n            } else {\n                diag_on_heartbeat(frame.source, &frame.body.heartbeat, frame.restart, frame.rx_time_ms);\n            }\n        }'

stall_code='
#if NODE_ID == 3 /* OVERLOAD TEST */
        static bool stalled;
        if (!stalled && (systick_now_ms() >= 1500U)) {
            stalled = true;
            while (systick_now_ms() < 2000U) {
            }
        }
#endif'

print_code='
#if NODE_ID == 3 /* OVERLOAD TEST */
        uint32_t drained = 0U;
        uint32_t n1_count = 0U;
        uint32_t n1_seq[16];
        uint32_t n1_lost[16];
        uint32_t n1_age[16];
        while (comm_receive(&frame)) {
            drained++;
            if ((frame.source == 1U) && (frame.type == COMM_MSG_PARAM)) {
                uint32_t age = systick_now_ms() - frame.rx_time_ms;
                if (n1_count < 16U) {
                    n1_seq[n1_count] = frame.sequence;
                    n1_lost[n1_count] = frame.lost_before;
                    n1_age[n1_count] = age;
                    n1_count++;
                }
            }
        }
        if (drained > 8U) {
            uart_debug_write("MCU3 DRAIN frames=");
            uart_debug_write_u32(drained);
            uart_debug_write(" n1:");
            for (uint32_t i = 0U; i < n1_count; i++) {
                uart_debug_write(" ");
                uart_debug_write_u32(n1_seq[i]);
                uart_debug_write("/");
                uart_debug_write_u32(n1_lost[i]);
                uart_debug_write("/");
                uart_debug_write_u32(n1_age[i]);
            }
            uart_debug_write("\n");
        } else {
            for (uint32_t i = 0U; i < n1_count; i++) {
                if (n1_lost[i] > 0U) {
                    uart_debug_write("MCU3 GAP n1 seq=");
                    uart_debug_write_u32(n1_seq[i]);
                    uart_debug_write(" lost=");
                    uart_debug_write_u32(n1_lost[i]);
                    uart_debug_write(" age=");
                    uart_debug_write_u32(n1_age[i]);
                    uart_debug_write("\n");
                }
            }
        }
#else
        while (comm_receive(&frame)) {
            if (frame.type == COMM_MSG_PARAM) {
                params_apply_block(frame.source, &frame.body.param, frame.sequence, frame.rx_time_ms);
                diag_on_param_block(frame.source, frame.rx_time_ms);
            } else {
                diag_on_heartbeat(frame.source, &frame.body.heartbeat, frame.restart, frame.rx_time_ms);
            }
        }
#endif'

babble_code='
#if NODE_ID == 2 /* OVERLOAD TEST: babbling node */
        {
            static const comm_param_block_t zero_block;
            static uint32_t babble_ms;
            static uint32_t babble_count;
            uint32_t now_ms = systick_now_ms();
            if ((now_ms >= 1600U) && (now_ms < 1900U)) {
                if (now_ms != babble_ms) {
                    babble_ms = now_ms;
                    babble_count = 0U;
                }
                if ((babble_count < 20U) && comm_send_param_block(&zero_block)) {
                    babble_count++;
                }
            }
        }
#endif'

case "$scenario" in
stall) swap "$app" "$run_line" "$run_line$stall_code"
       run=$'emulation RunFor "3.1"' ;;
babble) swap "$app" "$run_line" "$run_line$stall_code$babble_code"
       run=$'emulation RunFor "3.1"' ;;
halt)  run=$'emulation RunFor "1.5"\nmach set "mcu3"\nsysbus.cpu IsHalted true\nemulation RunFor "0.5"\nmach set "mcu3"\nsysbus.cpu IsHalted false\nemulation RunFor "1.6"' ;;
*) echo "unknown scenario $scenario" >&2; exit 2 ;;
esac
swap "$app" "$drain_lines" "$print_code"

printf 'include $ORIGIN/system.resc\n%s\nquit\n' "$run" > "$dst/renode/overload_run.resc"

cmake -S "$dst" -B "$dst/build" -DCMAKE_TOOLCHAIN_FILE="$dst/cmake/arm-none-eabi.cmake" >/dev/null
cmake --build "$dst/build" -j4 2>&1 | grep -E 'warning|error' && { echo "build failed" >&2; exit 4; } || true
timeout 300 renode --disable-gui --console -e "include @$dst/renode/overload_run.resc" </dev/null >"$dst/logs/console.txt" 2>&1

echo "##### overload: $scenario (source: $src)"
grep -E 'DRAIN|GAP' "$dst/logs/mcu3.log" || echo "(no DRAIN or GAP lines)"
grep -A3 ' t=3005 ' "$dst/logs/mcu3.log" | grep -E ' err | drv ' || true
