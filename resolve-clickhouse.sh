#!/usr/bin/env bash
# resolve-clickhouse.sh — resolve the clickhouse binary path
#
# Usage: source resolve-clickhouse.sh
#        resolve_clickhouse_binary [worktree-name]
#        export_clickhouse_env "$CLICKHOUSE_ROOT"
#
# Sets: CLICKHOUSE_BINARY, CLICKHOUSE_ROOT

_base="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
_active_worktree_file="$_base/.active-worktree"

resolve_clickhouse_binary() {
    local worktree_name="$1"
    local worktree_path=""

    if [[ -n "$worktree_name" ]]; then
        worktree_path="$(git -C "$_base" worktree list --porcelain \
            | awk -v name="$worktree_name" '/^worktree / { path=$2 } /^branch / { if (path && path ~ "/" name "$") print path }' \
            | head -1)"
        if [[ -z "$worktree_path" ]]; then
            echo "Error: worktree '$worktree_name' not found in git worktree list" >&2
            return 1
        fi
    elif [[ -f "$_active_worktree_file" ]]; then
        worktree_path="$(cat "$_active_worktree_file")"
        if [[ ! -d "$worktree_path" ]]; then
            echo "Warning: active worktree '$worktree_path' no longer exists, falling back to default" >&2
            worktree_path=""
        fi
    fi

    if [[ -n "$worktree_path" ]]; then
        CLICKHOUSE_BINARY="$worktree_path/build/programs/clickhouse"
        CLICKHOUSE_ROOT="$worktree_path"
    else
        CLICKHOUSE_BINARY="$_base/build/programs/clickhouse"
        CLICKHOUSE_ROOT="$_base"
    fi

    if [[ ! -x "$CLICKHOUSE_BINARY" ]]; then
        echo "Error: $CLICKHOUSE_BINARY not found or not executable" >&2
        return 1
    fi
}

save_active_worktree() {
    local worktree_path="$1"
    if [[ -n "$worktree_path" ]]; then
        echo "$worktree_path" > "$_active_worktree_file"
    else
        rm -f "$_active_worktree_file"
    fi
}

export_clickhouse_env() {
    local root="$1"
    local build_dir
    build_dir="$(dirname "$CLICKHOUSE_BINARY")"

    export CLICKHOUSE_TESTS_SERVER_BIN_PATH="$build_dir/clickhouse-server"
    export CLICKHOUSE_TESTS_CLIENT_BIN_PATH="$build_dir/clickhouse-client"
    export CLICKHOUSE_TESTS_ODBC_BRIDGE_BIN_PATH="$build_dir/clickhouse-odbc-bridge"
    export CLICKHOUSE_TESTS_LIBRARY_BRIDGE_BIN_PATH="$build_dir/clickhouse-library-bridge"
    export CLICKHOUSE_TESTS_BASE_CONFIG_DIR="$root/src/programs/server"
    export CLICKHOUSE_STATELESS_QUERY_TESTS_DIR="$root/src/tests/queries/0_stateless"
    export CLICKHOUSE_STATEFUL_QUERY_TESTS_DIR="$root/src/tests/queries/1_stateful"
    export CLICKHOUSE_TESTS_INTEGRATION_PATH="$root/src/tests/integration"
    export CLICKHOUSE_USER_FILES="$root/data/user_files"
    export CLICKHOUSE_DISKS_FILES="$root/data/disks"
    export CLICKHOUSE_SCHEMA_FILES="$root/data/format_schemas"
    export CLICKHOUSE_SRC_DIR="$root/src/src"
    export CLICKHOUSE_PERF_TESTS_DIR="$root/src/tests/performance"
    export CLICKHOUSE_PORT_HTTP=8123
    export CLICKHOUSE_PORT_TCP=9000
}
