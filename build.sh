#!/usr/bin/env bash
#
# build.sh — Build ClickHouse inside Docker with sccache, build seed, etc.
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
#   --no-seed       Don't use/update build seed
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
NO_SEED=0
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
    --no-seed)
        NO_SEED=1
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
    -DCOMPILER_CACHE=sccache
    -DCMAKE_TOOLCHAIN_FILE=/ClickHouse/cmake/linux/toolchain-x86_64.cmake
    -DENABLE_RUST=0
    -DENABLE_THINLTO=0
    -DENABLE_TESTS=1
    -DENABLE_UTILS=0
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
SEED_DIR="$REPO_ROOT/cache/builds/$BUILD_TYPE"
SEED_LOCK="$REPO_ROOT/cache/builds/seed.lock"
SCCACHE_CACHE="$REPO_ROOT/cache/sccache"
CONTAINER_NAME="build-$WORKTREE_NAME"

echo "=== ClickHouse Build ==="
echo "  Worktree:   $WORKTREE_PATH"
echo "  Build type: $BUILD_TYPE"
echo "  Target:     $TARGET"
echo "  Build dir:  $BUILD_DIR"
echo "  Container:  $CONTAINER_NAME"
echo ""

# --- Ensure sccache proxy is running ---
if ! curl -sf http://localhost:8083/ >/dev/null 2>&1; then
    echo "Starting sccache proxy on port 8083..."
    nohup python3 "$REPO_ROOT/sccache-proxy.py" \
        --port 8083 \
        --cache-dir "$SCCACHE_CACHE" \
        --max-size 80 >/dev/null 2>&1 &
    sleep 0.5
fi

# --- Patch build.ninja to disable VerifyGlobs cmake re-run ---
# CMake's VerifyGlobs.cmake_force phony target is always dirty, which causes
# cmake.verify_globs to re-run, which triggers RERUN_CMAKE and regenerates
# build.ninja — invalidating all ninja deps and causing a full rebuild.
# Fix: remove cmake.verify_globs from the RERUN_CMAKE deps.
patch_build_ninja() {
    local build_ninja="$1"
    if [[ ! -f "$build_ninja" ]]; then
        return
    fi
    # Check if already patched
    if ! python3 -c "
with open('$build_ninja', 'r') as f:
    for line in f:
        if 'RERUN_CMAKE' in line and 'cmake.verify_globs' in line:
            exit(1)
" 2>/dev/null; then
        # Break the hardlink before editing (safe for both seeded and non-seeded)
        cp "$build_ninja" "$build_ninja.tmp"
        python3 -c "
with open('$build_ninja.tmp', 'r') as f:
    lines = f.readlines()
for i, line in enumerate(lines):
    if 'RERUN_CMAKE' in line and 'cmake.verify_globs' in line:
        lines[i] = line.replace(' /ClickHouse/build/CMakeFiles/cmake.verify_globs', '')
        break
with open('$build_ninja.tmp', 'w') as f:
    f.writelines(lines)
"
        mv "$build_ninja.tmp" "$build_ninja"
        # Ensure build.ninja is newer than all cmake inputs
        touch "$build_ninja"
        echo "Patched build.ninja (disabled VerifyGlobs cmake re-run)"
    fi
}

# --- Seed build directory ---
SEEDED=0
if [[ "$NO_SEED" -eq 0 && ! -d "$BUILD_DIR" ]]; then
    mkdir -p "$(dirname "$SEED_DIR")"
    mkdir -p "$(dirname "$BUILD_DIR")"
    touch "$SEED_LOCK"
    if flock -s "$SEED_LOCK" bash -c '
        if [ -d "'"$SEED_DIR"'" ]; then
            cp -al "'"$SEED_DIR"'/" "'"$BUILD_DIR"'/"
            echo "SEEDED"
        fi
    ' | grep -q SEEDED; then
        SEEDED=1
        echo "Seeded build directory from $SEED_DIR"
        patch_build_ninja "$BUILD_DIR/build.ninja"
    else
        echo "No seed found — building from scratch"
        mkdir -p "$BUILD_DIR"
    fi
fi

mkdir -p "$BUILD_DIR"

# --- Always patch build.ninja before building ---
# This handles the case where build dir already exists from a previous build
# that ran cmake (which regenerates the VerifyGlobs dep).
patch_build_ninja "$BUILD_DIR/build.ninja"

# --- Allocate sccache port ---
SCCACHE_PORT=$("$REPO_ROOT/allocate-port.sh" "$WORKTREE_NAME")

# --- Stop any existing container with same name ---
docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

# --- Build the command to run inside Docker ---
DOCKER_ARGS=(
    docker run --rm
    --name "$CONTAINER_NAME"
    --user "$(id -u):$(id -g)"
    --network=host
    --volume "$WORKTREE_PATH:/ClickHouse"
    --volume "$REPO_ROOT/.git:$REPO_ROOT/.git"
    --workdir /ClickHouse/build
    -e SCCACHE_ENDPOINT=http://localhost:8083
    -e SCCACHE_BUCKET=cache
    -e SCCACHE_S3_USE_SSL=false
    -e SCCACHE_SERVER_PORT="$SCCACHE_PORT"
    -e AWS_ACCESS_KEY_ID=local
    -e AWS_SECRET_ACCESS_KEY=local
)

if [[ "$SHELL_MODE" -eq 1 ]]; then
    echo "Dropping into shell..."
    "${DOCKER_ARGS[@]}" -it "$IMAGE" bash
    exit $?
fi

# --- Build the inner script ---
INNER_SCRIPT=""

# Start sccache
INNER_SCRIPT+='echo "--- Starting sccache ---"
sccache --start-server 2>&1 || true
'

# cmake decision:
#   - Skip if --no-cmake
#   - Force if --cmake
#   - Otherwise: run cmake only if build.ninja doesn't exist (first build)
#   - After cmake, always patch build.ninja to disable VerifyGlobs
NEED_CMAKE=1
if [[ "$SKIP_CMAKE" -eq 1 ]]; then
    NEED_CMAKE=0
elif [[ "$FORCE_CMAKE" -eq 1 ]]; then
    NEED_CMAKE=1
elif [[ -f "$BUILD_DIR/build.ninja" ]]; then
    # build.ninja exists — skip cmake to avoid VerifyGlobs cascade
    NEED_CMAKE=0
    echo "Skipping cmake (build.ninja exists; use --cmake to force)"
fi

if [[ "$NEED_CMAKE" -eq 1 ]]; then
    CMAKE_CMD="cmake"
    for flag in "${CMAKE_FLAGS[@]}"; do
        CMAKE_CMD+=" $flag"
    done
    CMAKE_CMD+=" /ClickHouse -B /ClickHouse/build"

    INNER_SCRIPT+='echo "--- Running cmake ---"
'"$CMAKE_CMD"'
CMAKE_RC=$?
if [ $CMAKE_RC -ne 0 ]; then
    echo "cmake failed with exit code $CMAKE_RC"
    exit $CMAKE_RC
fi
echo "--- Patching build.ninja (post-cmake) ---"
python3 -c "
import sys
path = \"/ClickHouse/build/build.ninja\"
with open(path, \"r\") as f:
    lines = f.readlines()
patched = False
for i, line in enumerate(lines):
    if \"RERUN_CMAKE\" in line and \"cmake.verify_globs\" in line:
        lines[i] = line.replace(\" /ClickHouse/build/CMakeFiles/cmake.verify_globs\", \"\")
        patched = True
        break
if patched:
    with open(path, \"w\") as f:
        f.writelines(lines)
    print(\"Patched build.ninja (disabled VerifyGlobs cmake re-run)\")
else:
    print(\"build.ninja already patched or no VerifyGlobs dep found\")
"
'
fi

# NOTE: Do NOT touch .o/.a files or run ninja -t restat on seeded builds.
# The hardlinked files from cp -al preserve their original mtimes, which
# match what .ninja_deps recorded. Touching them would make mtimes
# inconsistent and cause ninja to rebuild everything.

# ninja build (unless cmake-only)
if [[ "$CMAKE_ONLY" -eq 0 ]]; then
    INNER_SCRIPT+='echo "--- Building ---"
time ninja '"$TARGET"'
BUILD_RC=$?
echo "--- sccache stats ---"
sccache --show-stats 2>&1 || true
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

# --- Update seed after successful build ---
if [[ "$NO_SEED" -eq 0 && "$CMAKE_ONLY" -eq 0 ]]; then
    echo "Updating build seed..."
    mkdir -p "$(dirname "$SEED_DIR")"
    touch "$SEED_LOCK"
    flock "$SEED_LOCK" bash -c '
        rm -rf "'"$SEED_DIR"'.old"
        mv "'"$SEED_DIR"'" "'"$SEED_DIR"'.old" 2>/dev/null || true
        cp -al "'"$BUILD_DIR"'/" "'"$SEED_DIR"'"
        rm -rf "'"$SEED_DIR"'.old" &
    '
    echo "Build seed updated: $SEED_DIR"
fi

echo ""
echo "=== Done ==="
echo "Binary: $BUILD_DIR/programs/clickhouse"
