#!/usr/bin/env bash
# Builds and runs the PC tests with the host compiler. Output goes to build/host/.
#   firmware/tests/host/run_host_tests.sh
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
out="$repo/build/host"
mkdir -p "$out"

gcc -std=c11 -Wall -Wextra -Wshadow -Werror -O2 \
    -I"$repo/firmware/common" \
    -DFRAME_QUEUE_TEST_HOOK=test_hook \
    "$repo/firmware/tests/host/test_frame_queue.c" "$repo/firmware/common/drivers/frame_queue.c" \
    -o "$out/test_frame_queue"
"$out/test_frame_queue"

gcc -std=c11 -Wall -Wextra -Wshadow -Werror -O2 \
    -I"$repo/firmware/common" \
    -DNODE_ID=2 \
    "$repo/firmware/tests/host/test_params.c" "$repo/firmware/common/params/params.c" \
    "$repo/firmware/common/params/param_source.c" "$repo/firmware/common/system_table.c" \
    -lm -o "$out/test_params"
"$out/test_params"

gcc -std=c11 -Wall -Wextra -Wshadow -Werror -O2 \
    -I"$repo/firmware/common" \
    -DNODE_ID=1 \
    "$repo/firmware/tests/host/test_diag.c" "$repo/firmware/common/diag/diag.c" \
    -o "$out/test_diag"
"$out/test_diag"
