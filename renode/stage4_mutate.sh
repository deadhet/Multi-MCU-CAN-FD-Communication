#!/usr/bin/env bash
# Stage 4 mutation tests. Copies the source tree to build/mutant/<name>, breaks node 2's
# frames in one way, builds, runs 4 nodes for 1.1 s, and prints the t=1005 report of every
# node. The real source tree is never changed.
#   renode/stage4_mutate.sh <name>   (crc version source length nobrs dup old gap badid fakeboot values)
set -euo pipefail

name="$1"
repo="$(cd "$(dirname "$0")/.." && pwd)"
dst="$repo/build/mutant/$name"
rm -rf "$dst"
mkdir -p "$dst/logs"
cp -r "$repo/CMakeLists.txt" "$repo/cmake" "$repo/firmware" "$repo/renode" "$dst/"

comm="$dst/firmware/common/comm/comm.c"
fdcan="$dst/firmware/common/drivers/fdcan.c"

# Replaces one exact line (must occur once) with new text.
swap() { # file old new
    FILE="$1" OLD="$2" NEW="$3" perl -0pi -e '
        my ($o, $n) = ($ENV{OLD}, $ENV{NEW});
        my $c = () = /\Q$o\E/g;
        die "anchor found $c times\n" unless $c == 1;
        s/\Q$o\E/$n/;' "$1"
}

seq_line='    uint32_t seq = tx_seq[msg] + 1U;'
crc_line='    put_u16_le(&buf[msg_info[msg].crc_offset], frame_crc(id, buf, msg_info[msg].crc_offset));'
send_line='    if (!fdcan_send(id, buf, msg_info[msg].len)) {'
id_line='    uint32_t id = MAKE_ID(msg_info[msg].type, NODE_ID);'
range_line='        { MAKE_ID(TYPE_HEARTBEAT, 1U), MAKE_ID(TYPE_HEARTBEAT, COMM_NODE_COUNT), QUEUE_HEARTBEAT },'
elem_line='    *ram_word(element + 4U) = (dlc << ELEMENT_DLC_POS) | ELEMENT_BRS | ELEMENT_FDF;'

p2() { # code to run only on node 2's parameter blocks
    printf '#if NODE_ID == 2 /* MUTANT */\n    if (msg == COMM_MSG_PARAM) { %s }\n#endif\n' "$1"
}

h2() { # code to run only on node 2's heartbeats
    printf '#if NODE_ID == 2 /* MUTANT */\n    if (msg == COMM_MSG_HEARTBEAT) { %s }\n#endif\n' "$1"
}

case "$name" in
crc)     swap "$comm" "$crc_line" "$crc_line"$'\n'"$(p2 'buf[msg_info[msg].crc_offset] ^= 0x01U;')" ;;
version) swap "$comm" "$crc_line" "$(p2 'buf[BYTE_VERSION] = 2U;')"$'\n'"$crc_line" ;;
source)  swap "$comm" "$crc_line" "$(p2 'buf[BYTE_SOURCE] = 3U;')"$'\n'"$crc_line" ;;
length)  swap "$comm" "$send_line" "    uint8_t len = msg_info[msg].len;"$'\n'"$(p2 'len = 24U;')"$'\n'"    if (!fdcan_send(id, buf, len)) {" ;;
dup)     swap "$comm" "$seq_line" "$seq_line"$'\n'"$(p2 'seq = 1U;')" ;;
old)     swap "$comm" "$seq_line" "$seq_line"$'\n'"$(p2 'seq = 1000U - tx_stats.sent[msg];')" ;;
gap)     swap "$comm" "$seq_line" "$seq_line"$'\n'"$(p2 'seq = 2U * (tx_stats.sent[msg] + 1U);')" ;;
fakeboot) swap "$comm" "$crc_line" "$crc_line"$'\n'"$(h2 'if ((tx_stats.sent[msg] % 5U) == 4U) { put_u32_le(&buf[BYTE_SEQUENCE], 1U); }')" ;;
# Check 8, sealed with a good CRC: P6 NaN, P7 SOURCE_FAULT, P8 SOURCE_STALE, P9 reserved status 3,
# P10 bool 2. Status bits: slot 1 = 2 (0x08), slot 2 = 1 (0x10), slot 3 = 3 (0xC0).
values)  swap "$comm" "$crc_line" "$(p2 'put_u32_le(&buf[PARAM_VALUES], 0x7FC00000U); buf[PARAM_STATUS] |= 0xD8U; put_u32_le(&buf[PARAM_VALUES + 16U], 2U);')"$'\n'"$crc_line" ;;
badid)   swap "$comm" "$id_line" "$id_line"$'\n'"$(p2 'id = MAKE_ID(TYPE_PARAM, 5U);')"
         swap "$comm" "$range_line" "$range_line"$'\n#if NODE_ID == 1 /* MUTANT: also accept 0x025-0x02F */\n        { MAKE_ID(TYPE_PARAM, 5U), MAKE_ID(TYPE_PARAM, 15U), QUEUE_HEARTBEAT },\n#endif' ;;
nobrs)   swap "$fdcan" "$elem_line" $'#if NODE_ID == 2 /* MUTANT */\n    *ram_word(element + 4U) = (dlc << ELEMENT_DLC_POS) | ELEMENT_FDF;\n#else\n'"$elem_line"$'\n#endif' ;;
*) echo "unknown mutation $name" >&2; exit 2 ;;
esac

grep -q 'MUTANT' "$comm" "$fdcan" || { echo "mutation not applied" >&2; exit 3; }

cat > "$dst/renode/mutant_run.resc" <<'EOF'
include $ORIGIN/system.resc
emulation RunFor "1.1"
quit
EOF

cmake -S "$dst" -B "$dst/build" -DCMAKE_TOOLCHAIN_FILE="$dst/cmake/arm-none-eabi.cmake" >/dev/null
cmake --build "$dst/build" -j4 2>&1 | grep -E 'warning|error' && { echo "build failed" >&2; exit 4; } || true
timeout 200 renode --disable-gui --console -e "include @$dst/renode/mutant_run.resc" </dev/null >"$dst/logs/console.txt" 2>&1

echo "##### mutation: $name"
for n in 1 2 3 4; do
    grep -A5 ' t=1005 tx ' "$dst/logs/mcu$n.log" | grep -E ' rx | err | db synced'
done
