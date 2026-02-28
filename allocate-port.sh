#!/bin/bash
# Allocate a unique sccache server port for a worktree.
# Ports are persistent — the same worktree always gets the same port.
# Concurrent callers are serialized via flock.
#
# Usage:
#   ./allocate-port.sh <worktree-name>
#   export SCCACHE_SERVER_PORT=$(./allocate-port.sh my-feature)
#
# To see all allocations:
#   ./allocate-port.sh --list
#
# To release a port (e.g., after removing a worktree):
#   ./allocate-port.sh --release <worktree-name>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT_FILE="$SCRIPT_DIR/cache/sccache-ports"
LOCK_FILE="$SCRIPT_DIR/cache/sccache-ports.lock"
PORT_MIN=4227
PORT_MAX=4326

mkdir -p "$(dirname "$PORT_FILE")"

do_list() {
    if [ -f "$PORT_FILE" ]; then
        cat "$PORT_FILE"
    fi
}

do_release() {
    local name="$1"
    exec 8>"$LOCK_FILE"
    flock 8
    if [ -f "$PORT_FILE" ]; then
        grep -v "^${name}	" "$PORT_FILE" >"$PORT_FILE.tmp" || true
        mv "$PORT_FILE.tmp" "$PORT_FILE"
    fi
    exec 8>&-
}

do_allocate() {
    local name="$1"

    exec 8>"$LOCK_FILE"
    flock 8

    touch "$PORT_FILE"

    # Check if this worktree already has a port
    local existing
    existing=$(awk -F'\t' -v n="$name" '$1 == n { print $2 }' "$PORT_FILE")
    if [ -n "$existing" ]; then
        echo "$existing"
        exec 8>&-
        return 0
    fi

    # Collect all used ports
    local used_ports
    used_ports=$(awk -F'\t' '{ print $2 }' "$PORT_FILE" | sort -n)

    # Find the first free port in range
    local port
    for port in $(seq $PORT_MIN $PORT_MAX); do
        if ! echo "$used_ports" | grep -qx "$port"; then
            echo -e "${name}\t${port}" >>"$PORT_FILE"
            echo "$port"
            exec 8>&-
            return 0
        fi
    done

    echo "ERROR: No free ports in range $PORT_MIN-$PORT_MAX" >&2
    exec 8>&-
    return 1
}

# Parse arguments
if [ $# -eq 0 ]; then
    echo "Usage: $0 <worktree-name> | --list | --release <name>" >&2
    exit 1
fi

case "$1" in
--list)
    do_list
    ;;
--release)
    if [ $# -lt 2 ]; then
        echo "Usage: $0 --release <worktree-name>" >&2
        exit 1
    fi
    do_release "$2"
    ;;
*)
    do_allocate "$1"
    ;;
esac
