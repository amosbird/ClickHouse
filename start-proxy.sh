#!/bin/bash
# Start the sccache proxy if it's not already running.
# This script is idempotent — safe to call multiple times.
#
# Usage:
#   ./start-proxy.sh           # start on default port 8083
#   ./start-proxy.sh --port 9999  # start on custom port
#   ./start-proxy.sh --stop    # stop the running proxy
#   ./start-proxy.sh --status  # show proxy stats

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROXY_SCRIPT="$SCRIPT_DIR/sccache-proxy.py"
CACHE_DIR="$SCRIPT_DIR/cache/sccache"
LOG_FILE="$SCRIPT_DIR/cache/proxy.log"
PID_FILE="$SCRIPT_DIR/cache/proxy.pid"
PORT="${SCCACHE_PROXY_PORT:-8083}"
MAX_SIZE="${SCCACHE_PROXY_MAX_SIZE:-80}"

# Parse arguments
ACTION="start"
for arg in "$@"; do
    case "$arg" in
    --stop) ACTION="stop" ;;
    --status) ACTION="status" ;;
    --port)
        shift
        PORT="$1"
        ;;
    --port=*) PORT="${arg#--port=}" ;;
    *) ;;
    esac
done

is_running() {
    # First check PID file
    if [ -f "$PID_FILE" ]; then
        pid=$(cat "$PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        rm -f "$PID_FILE"
    fi
    # Fallback: probe the port directly (handles manually started instances)
    if curl -s --max-time 2 "http://localhost:$PORT/" >/dev/null 2>&1; then
        # Proxy is running but we don't have a PID file — try to find the PID
        pid=$(pgrep -f "python3 .*sccache-proxy\.py" 2>/dev/null | tail -1 || true)
        if [ -n "$pid" ]; then
            echo "$pid" >"$PID_FILE"
        fi
        return 0
    fi
    return 1
}

case "$ACTION" in
stop)
    if is_running; then
        if [ -f "$PID_FILE" ]; then
            pid=$(cat "$PID_FILE")
        else
            pid=$(pgrep -f "python3 .*sccache-proxy\.py" 2>/dev/null | tail -1 || true)
        fi
        if [ -n "$pid" ]; then
            echo "Stopping sccache proxy (PID $pid)..."
            kill $pid 2>/dev/null || true
            rm -f "$PID_FILE"
            echo "Stopped."
        else
            echo "Proxy seems to be running but could not find PID."
        fi
    else
        echo "Proxy is not running."
    fi
    ;;

status)
    if is_running; then
        if [ -f "$PID_FILE" ]; then
            pid=$(cat "$PID_FILE")
        else
            pid="unknown"
        fi
        echo "Proxy is running (PID $pid)"
        echo ""
        curl -s "http://localhost:$PORT/" 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "Could not fetch stats"
    else
        echo "Proxy is not running."
    fi
    ;;

start)
    if is_running; then
        pid=$(cat "$PID_FILE")
        echo "Proxy already running (PID $pid)"
        curl -s "http://localhost:$PORT/" 2>/dev/null | python3 -m json.tool 2>/dev/null || true
        exit 0
    fi

    mkdir -p "$(dirname "$LOG_FILE")"

    echo "Starting sccache proxy on port $PORT..."
    echo "  Cache dir: $CACHE_DIR"
    echo "  Max size:  ${MAX_SIZE}GB"
    echo "  Log file:  $LOG_FILE"

    nohup python3 "$PROXY_SCRIPT" \
        --port "$PORT" \
        --cache-dir "$CACHE_DIR" \
        --max-size "$MAX_SIZE" \
        >>"$LOG_FILE" 2>&1 &
    echo $! >"$PID_FILE"

    # Wait briefly and verify it started
    sleep 0.5
    if is_running; then
        echo "Proxy started (PID $(cat "$PID_FILE"))"
    else
        echo "ERROR: Proxy failed to start. Check $LOG_FILE"
        exit 1
    fi
    ;;
esac
