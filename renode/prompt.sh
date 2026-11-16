#!/usr/bin/env bash
# One terminal: Renode's own prompt with the four MCUs loaded and paused, plus the dashboard in
# the browser. You type Renode's commands yourself (docs/running_in_renode.md, "One terminal").
#   renode/prompt.sh                  Renode and the dashboard
#   renode/prompt.sh --no-dashboard   Renode only
# Type "start" to run. "quit" ends Renode and stops the dashboard. Build first (cmake --build build).
set -u

repo="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$repo/logs"

dashboard=""
cleanup() {
    [ -n "$dashboard" ] && kill "$dashboard" 2>/dev/null
}
trap cleanup EXIT

if [ "${1:-}" != "--no-dashboard" ]; then
    (cd "$repo/dashboard" && uv sync --frozen --quiet) || exit 1
    # In the background, its messages in a file, so this terminal shows only Renode.
    (cd "$repo/dashboard" && exec .venv/bin/python -m noccarc_dashboard.server --port 3456 --lost-ms 1500) \
        >"$repo/logs/dashboard.out" 2>&1 &
    dashboard=$!
    sleep 1
    if ! kill -0 "$dashboard" 2>/dev/null; then
        echo "The dashboard did not start (logs/dashboard.out):" >&2
        tail -n 3 "$repo/logs/dashboard.out" >&2
        dashboard=""
        exit 1
    fi
    echo "Dashboard: http://127.0.0.1:8765 (its messages: logs/dashboard.out)"
fi

# logLevel 3: only Renode's errors, not its start-up warnings about registers it does not model.
renode --console -e "logLevel 3; include @$repo/renode/system.resc"
