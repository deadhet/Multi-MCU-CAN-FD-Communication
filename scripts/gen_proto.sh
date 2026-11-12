#!/usr/bin/env bash
# Regenerates the UART link's Protobuf code from proto/monitor.proto (+ monitor.options):
#   C, nanopb      -> firmware/common/monitor/monitor.pb.c, monitor.pb.h
#   Python         -> dashboard/noccarc_dashboard/monitor_pb2.py
# Both generators run from the uv project in dashboard/ (uv-managed Python only). The outputs
# are committed, so building the firmware needs only the ARM toolchain.
#   scripts/gen_proto.sh
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo/dashboard"
uv run --frozen nanopb_generator -I "$repo/proto" -D "$repo/firmware/common/monitor" "$repo/proto/monitor.proto"
uv run --frozen python -m grpc_tools.protoc -I "$repo/proto" --python_out="$repo/dashboard/noccarc_dashboard" "$repo/proto/monitor.proto"
echo "generated: firmware/common/monitor/monitor.pb.{c,h}, dashboard/noccarc_dashboard/monitor_pb2.py"
