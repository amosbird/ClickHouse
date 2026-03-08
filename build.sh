#!/usr/bin/env bash
#
# build.sh — Build ClickHouse inside Docker with ccache.
#
# Usage:
#   ./build.sh [WORKTREE_PATH] [OPTIONS]
#
# Arguments:
#   WORKTREE_PATH   Path to worktree (default: current directory)
#
# Options:
#   --type TYPE     Build type: release, debug, asan, tsan, msan, ubsan (default: release)
#   --target TGT    Ninja target (default: clickhouse)
#   --cmake         Force cmake even if build.ninja exists
#   --no-cmake      Skip cmake, run ninja only
#   --cmake-only    Run cmake only, don't build
#   --shell         Drop into a shell inside the container instead of building
#
# Examples:
#   ./build.sh par-build-2
#   ./build.sh par-build-2 --type debug
#   ./build.sh par-build-2 --no-cmake --target clickhouse
#   ./build.sh par-build-2 --shell
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
IMAGE="clickhouse/binary-builder:0261cd99929ade4b3e59_amd"

# --- Parse arguments ---
WORKTREE_PATH=""
BUILD_TYPE="release"
TARGET="clickhouse"
SKIP_CMAKE=0
FORCE_CMAKE=0
CMAKE_ONLY=0
SHELL_MODE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
    --type)
        BUILD_TYPE="$2"
        shift 2
        ;;
    --target)
        TARGET="$2"
        shift 2
        ;;
    --no-cmake)
        SKIP_CMAKE=1
        shift
        ;;
    --cmake)
        FORCE_CMAKE=1
        shift
        ;;
    --cmake-only)
        CMAKE_ONLY=1
        shift
        ;;
    --shell)
        SHELL_MODE=1
        shift
        ;;
    -*)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
    *)
        if [[ -z "$WORKTREE_PATH" ]]; then
            WORKTREE_PATH="$1"
        else
            echo "Unexpected argument: $1" >&2
            exit 1
        fi
        shift
        ;;
    esac
done

# Default to current directory
if [[ -z "$WORKTREE_PATH" ]]; then
    WORKTREE_PATH="$(pwd)"
fi

# Resolve to absolute path
WORKTREE_PATH="$(cd "$WORKTREE_PATH" && pwd)"
WORKTREE_NAME="$(basename "$WORKTREE_PATH")"

# --- Build type → cmake flags ---
COMMON_FLAGS=(
    -DCMAKE_C_COMPILER=clang-21
    -DCMAKE_CXX_COMPILER=clang++-21
    -DCOMPILER_CACHE=ccache
    -DCMAKE_TOOLCHAIN_FILE=/ClickHouse/cmake/linux/toolchain-x86_64.cmake
    -DENABLE_RUST=0
    -DENABLE_THINLTO=0
    -DENABLE_TESTS=1
    -DENABLE_UTILS=0
    -DUSE_MONGODB=0
    -DENABLE_LIBPQXX=0
    -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON
)

case "$BUILD_TYPE" in
release)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
        "${COMMON_FLAGS[@]}"
    )
    ;;
debug)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=Debug
        "${COMMON_FLAGS[@]}"
    )
    ;;
asan)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=None
        -DSANITIZE=address
        "${COMMON_FLAGS[@]}"
    )
    ;;
tsan)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=None
        -DSANITIZE=thread
        "${COMMON_FLAGS[@]}"
    )
    ;;
msan)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=None
        -DSANITIZE=memory
        "${COMMON_FLAGS[@]}"
    )
    ;;
ubsan)
    CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=None
        -DSANITIZE=undefined
        "${COMMON_FLAGS[@]}"
    )
    ;;
*)
    echo "Unknown build type: $BUILD_TYPE" >&2
    echo "Valid: release, debug, asan, tsan, msan, ubsan" >&2
    exit 1
    ;;
esac

BUILD_DIR="$WORKTREE_PATH/build"
CCACHE_DIR="$REPO_ROOT/cache/ccache"
CCACHE_BIN="$REPO_ROOT/cache/ccache-bin/ccache"
CONTAINER_NAME="build-$WORKTREE_NAME"

echo "=== ClickHouse Build ==="
echo "  Worktree:   $WORKTREE_PATH"
echo "  Build type: $BUILD_TYPE"
echo "  Target:     $TARGET"
echo "  Build dir:  $BUILD_DIR"
echo "  ccache:     $CCACHE_DIR"
echo "  Container:  $CONTAINER_NAME"
echo ""

# --- Ensure ccache binary exists ---
if [[ ! -x "$CCACHE_BIN" ]]; then
    echo "Downloading ccache 4.12.3..."
    mkdir -p "$(dirname "$CCACHE_BIN")"
    curl -sL https://github.com/ccache/ccache/releases/download/v4.12.3/ccache-4.12.3-linux-x86_64.tar.xz |
        tar -xJ --strip-components=1 -C "$(dirname "$CCACHE_BIN")"
fi

mkdir -p "$BUILD_DIR" "$CCACHE_DIR"

# --- Stop any existing container with same name ---
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

# --- Worktree git support ---
# If the source is a git worktree (.git is a file pointing to the main repo),
# mount the main repo's .git directory so git commands work inside Docker.
EXTRA_DOCKER_ARGS=()
if [[ -f "$WORKTREE_PATH/.git" ]]; then
    # .git file contains: gitdir: /path/to/main/.git/worktrees/<name>
    MAIN_GIT_DIR="$(sed 's/^gitdir: //' "$WORKTREE_PATH/.git")"
    # Resolve to absolute path
    MAIN_GIT_DIR="$(cd "$WORKTREE_PATH" && cd "$(dirname "$MAIN_GIT_DIR")" && pwd)/$(basename "$MAIN_GIT_DIR")"
    # The common git dir is two levels up from .git/worktrees/<name>
    GIT_COMMON="$(cd "$MAIN_GIT_DIR/../.." && pwd)"
    echo "  Worktree detected, mounting git dir: $GIT_COMMON"
    EXTRA_DOCKER_ARGS=(
        --volume "$GIT_COMMON:$GIT_COMMON:ro"
    )
fi

# --- Build the Docker command ---
DOCKER_ARGS=(
    docker run --rm
    --name "$CONTAINER_NAME"
    --user "$(id -u):$(id -g)"
    --network=host
    --volume "$WORKTREE_PATH:/ClickHouse"
    --volume "$CCACHE_BIN:/usr/local/bin/ccache:ro"
    --volume "$CCACHE_DIR:/ccache"
    "${EXTRA_DOCKER_ARGS[@]}"
    --workdir /ClickHouse/build
    -e CCACHE_DIR=/ccache
    -e CCACHE_MAXSIZE=50G
    -e CCACHE_DEPEND=1
    -e CCACHE_SLOPPINESS=file_macro,time_macros,include_file_mtime
    -e CCACHE_NOHASHDIR=1
    -e CCACHE_BASEDIR=/ClickHouse
)

if [[ "$SHELL_MODE" -eq 1 ]]; then
    echo "Dropping into shell..."
    "${DOCKER_ARGS[@]}" -it "$IMAGE" bash
    exit $?
fi

# --- Build the inner script ---
INNER_SCRIPT='ccache -z >/dev/null 2>&1
'

# cmake decision:
#   - Skip if --no-cmake
#   - Force if --cmake
#   - Otherwise: run cmake only if build.ninja doesn't exist (first build)
NEED_CMAKE=1
if [[ "$SKIP_CMAKE" -eq 1 ]]; then
    NEED_CMAKE=0
elif [[ "$FORCE_CMAKE" -eq 1 ]]; then
    NEED_CMAKE=1
elif [[ -f "$BUILD_DIR/build.ninja" ]]; then
    NEED_CMAKE=0
    echo "Skipping cmake (build.ninja exists; use --cmake to force)"
fi

if [[ "$NEED_CMAKE" -eq 1 ]]; then
    CMAKE_CMD="cmake"
    for flag in "${CMAKE_FLAGS[@]}"; do
        CMAKE_CMD+=" $flag"
    done
    CMAKE_CMD+=" /ClickHouse -B /ClickHouse/build"

    INNER_SCRIPT+='echo "--- cmake ---"
'"$CMAKE_CMD"'
CMAKE_RC=$?
if [ $CMAKE_RC -ne 0 ]; then
    echo "cmake failed with exit code $CMAKE_RC"
    exit $CMAKE_RC
fi
'
fi

# ninja build (unless cmake-only)
if [[ "$CMAKE_ONLY" -eq 0 ]]; then
    INNER_SCRIPT+='echo "--- ninja '"$TARGET"' ---"
time ninja '"$TARGET"'
BUILD_RC=$?
echo "--- ccache stats ---"
ccache -s
if [ $BUILD_RC -ne 0 ]; then
    echo "Build failed with exit code $BUILD_RC"
    exit $BUILD_RC
fi
echo "--- Build complete ---"
ls -lh /ClickHouse/build/programs/clickhouse 2>/dev/null || true
'
fi

# Run it
echo "Starting build at $(date)"
"${DOCKER_ARGS[@]}" "$IMAGE" bash -c "$INNER_SCRIPT"
BUILD_EXIT=$?

if [[ $BUILD_EXIT -ne 0 ]]; then
    echo "Build failed (exit code $BUILD_EXIT)"
    exit $BUILD_EXIT
fi

echo ""
echo "=== Done ==="
echo "Binary: $BUILD_DIR/programs/clickhouse"
