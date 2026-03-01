#!/usr/bin/env bash
#
# create-worktree.sh — Create a ClickHouse worktree with submodules (fast, no network)
#
# Usage:
#   ./create-worktree.sh <branch-name> [worktree-path] [--ref <base-ref>]
#
# Arguments:
#   branch-name     Branch to check out (creates if doesn't exist)
#   worktree-path   Where to put the worktree (default: ./<branch-name>)
#
# Options:
#   --ref REF       Base ref for new branches (default: upstream/master)
#
# The script:
#   1. Creates a git worktree
#   2. Hardlink-copies submodule git data from a source worktree
#   3. Fixes config paths for the new worktree
#   4. Writes .git pointer files for all 129 submodules
#   5. Checks out all submodule working trees in parallel
#
# Total time: ~20 seconds (vs 55+ seconds with git submodule update)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Parse arguments ---
BRANCH=""
WORKTREE_PATH=""
BASE_REF="upstream/master"

while [[ $# -gt 0 ]]; do
    case "$1" in
    --ref)
        BASE_REF="$2"
        shift 2
        ;;
    -*)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
    *)
        if [[ -z "$BRANCH" ]]; then
            BRANCH="$1"
        elif [[ -z "$WORKTREE_PATH" ]]; then
            WORKTREE_PATH="$1"
        else
            echo "Unexpected argument: $1" >&2
            exit 1
        fi
        shift
        ;;
    esac
done

if [[ -z "$BRANCH" ]]; then
    echo "Usage: $0 <branch-name> [worktree-path] [--ref <base-ref>]" >&2
    exit 1
fi

# Default worktree path: ./<branch-name> (slashes replaced with dashes)
if [[ -z "$WORKTREE_PATH" ]]; then
    SAFE_BRANCH="${BRANCH//\//-}"
    WORKTREE_PATH="$SCRIPT_DIR/$SAFE_BRANCH"
fi

# Resolve to absolute path (parent must exist)
WORKTREE_DIR="$(dirname "$WORKTREE_PATH")"
WORKTREE_BASE="$(basename "$WORKTREE_PATH")"
WORKTREE_DIR="$(cd "$WORKTREE_DIR" && pwd)"
WORKTREE_PATH="$WORKTREE_DIR/$WORKTREE_BASE"

if [[ -d "$WORKTREE_PATH" ]]; then
    echo "Error: $WORKTREE_PATH already exists" >&2
    exit 1
fi

# --- Find source worktree with initialized submodules ---
# We need a worktree that has modules/ set up (i.e., submodules initialized).
# Prefer the one with the most modules (most complete submodule set).
GIT_COMMON_DIR="$(git -C "$SCRIPT_DIR" rev-parse --git-common-dir)"
GIT_COMMON_DIR="$(cd "$SCRIPT_DIR" && cd "$GIT_COMMON_DIR" && pwd)"

SOURCE_MODULES=""
SOURCE_ENTRY=""
BEST_COUNT=0
for entry_dir in "$GIT_COMMON_DIR"/worktrees/*/; do
    entry="$(basename "$entry_dir")"
    if [[ -d "$entry_dir/modules" ]]; then
        # Count modules (dirs with HEAD file = actual git repos)
        count="$(find "$entry_dir/modules" -name HEAD -maxdepth 3 | wc -l)"
        if [[ "$count" -gt "$BEST_COUNT" ]]; then
            BEST_COUNT="$count"
            SOURCE_MODULES="$entry_dir/modules"
            SOURCE_ENTRY="$entry"
        fi
    fi
done

if [[ -z "$SOURCE_MODULES" ]]; then
    echo "Error: No worktree with initialized submodules found." >&2
    echo "Run 'git submodule update --init' in at least one worktree first." >&2
    exit 1
fi

echo "Using submodule data from worktree: $SOURCE_ENTRY ($BEST_COUNT modules)"

# Determine source worktree's working directory path from its gitdir file.
# The gitdir file contains the path to the source worktree's .git file.
SOURCE_GITDIR_FILE="$GIT_COMMON_DIR/worktrees/$SOURCE_ENTRY/gitdir"
if [[ -f "$SOURCE_GITDIR_FILE" ]]; then
    SOURCE_WORKTREE_PATH="$(dirname "$(cat "$SOURCE_GITDIR_FILE")")"
else
    echo "Error: Cannot determine source worktree path (no gitdir file)" >&2
    exit 1
fi

# --- Step 1: Create the worktree ---
echo "=== Creating worktree ==="
echo "  Branch: $BRANCH"
echo "  Path:   $WORKTREE_PATH"

# Check if branch exists locally or on remote
LOCAL_BRANCH="$(git -C "$SCRIPT_DIR" branch --list "$BRANCH" | tr -d '* ')"
REMOTE_BRANCH="$(git -C "$SCRIPT_DIR" branch --list -r "origin/$BRANCH" | tr -d ' ')"

t0=$(date +%s%3N)

if [[ -n "$LOCAL_BRANCH" ]]; then
    git -C "$SCRIPT_DIR" worktree add "$WORKTREE_PATH" "$BRANCH"
elif [[ -n "$REMOTE_BRANCH" ]]; then
    git -C "$SCRIPT_DIR" worktree add "$WORKTREE_PATH" -b "$BRANCH" "origin/$BRANCH"
else
    # New branch — base it on BASE_REF
    echo "  Creating new branch from $BASE_REF"
    git -C "$SCRIPT_DIR" worktree add -b "$BRANCH" "$WORKTREE_PATH" "$BASE_REF"
fi

t1=$(date +%s%3N)
echo "  Worktree created in $((t1 - t0))ms"

# Verify the worktree has .gitmodules (i.e., it's a ClickHouse source commit, not a meta branch)
if [[ ! -f "$WORKTREE_PATH/.gitmodules" ]]; then
    echo "Error: $WORKTREE_PATH/.gitmodules not found." >&2
    echo "The branch '$BRANCH' does not appear to contain ClickHouse source code." >&2
    echo "Use --ref to specify a base ref with source code (e.g., --ref upstream/master)." >&2
    rm -rf "$WORKTREE_PATH"
    git -C "$SCRIPT_DIR" worktree prune
    exit 1
fi

# --- Step 2: Determine the worktree entry name ---
# git names the worktree entry after the basename of the path
WORKTREE_ENTRY="$(basename "$WORKTREE_PATH")"
DEST_MODULES="$GIT_COMMON_DIR/worktrees/$WORKTREE_ENTRY/modules"

if [[ -d "$DEST_MODULES" ]]; then
    echo "Warning: $DEST_MODULES already exists, removing..."
    rm -rf "$DEST_MODULES"
fi

# --- Step 3: Hardlink-copy submodule git data ---
echo "=== Setting up submodules (hardlink copy) ==="
t0=$(date +%s%3N)
cp -al "$SOURCE_MODULES" "$DEST_MODULES"
t1=$(date +%s%3N)
echo "  Hardlink copy in $((t1 - t0))ms"

# --- Step 4: Fix config paths ---
# The hardlinked configs reference the source worktree path (relative or absolute).
# Replace the entire worktree line with an absolute path to the new worktree.
echo "  Fixing config paths..."
echo "    Source: $SOURCE_WORKTREE_PATH"
echo "    Dest:   $WORKTREE_PATH"

# Fix worktree paths in all config files under modules/.
# Only match files named 'config' or 'config.worktree' (avoid scanning git objects).
# Replace any worktree line pointing to source contrib/ with absolute path to dest contrib/.
find "$DEST_MODULES" \( -name config -o -name config.worktree \) -exec \
    sed -i -E "s|worktree = .*/contrib/(.+)|worktree = ${WORKTREE_PATH}/contrib/\1|" {} +

t2=$(date +%s%3N)
echo "  Config paths fixed in $((t2 - t1))ms"

# --- Step 5: Write .git pointer files ---
echo "=== Writing .git pointer files ==="
t0=$(date +%s%3N)

# Get list of submodule paths from .gitmodules
SUBMODULES="$(git -C "$WORKTREE_PATH" config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')"

# The 3 aws sub-submodules have their modules at modules/<name> (top-level),
# while all other submodules have modules at modules/contrib/<name>.
AWS_TOP_LEVEL="aws-c-common aws-c-event-stream aws-checksums"

for sub in $SUBMODULES; do
    name="$(basename "$sub")"
    mkdir -p "$WORKTREE_PATH/$sub"

    # Determine the correct modules path
    if echo " $AWS_TOP_LEVEL " | grep -q " $name "; then
        echo "gitdir: ../../../.git/worktrees/$WORKTREE_ENTRY/modules/$name" \
            >"$WORKTREE_PATH/$sub/.git"
    else
        echo "gitdir: ../../../.git/worktrees/$WORKTREE_ENTRY/modules/contrib/$name" \
            >"$WORKTREE_PATH/$sub/.git"
    fi
done

t1=$(date +%s%3N)
echo "  Wrote $(echo "$SUBMODULES" | wc -l) .git pointer files in $((t1 - t0))ms"

# --- Step 6: Point submodule HEADs to correct commits and checkout ---
# The hardlinked git data may reference commits from the source worktree.
# We must update each submodule HEAD to the commit recorded in the new worktree's tree,
# then checkout the working directory.
# If the expected commit object doesn't exist (source worktree never fetched it),
# collect the submodule for a slower git-submodule-update fallback.
echo "=== Checking out submodule working trees (parallel) ==="
t0=$(date +%s%3N)

NCPU="$(nproc 2>/dev/null || echo 8)"
FALLBACK_LIST=$(mktemp)

echo "$SUBMODULES" | xargs -P "$NCPU" -I {} bash -c '
    # Get the commit the new worktree tree expects for this submodule
    expected=$(git -C "'"$WORKTREE_PATH"'" ls-tree HEAD -- "{}" 2>/dev/null | awk "{print \$3}")
    if [[ -z "$expected" ]]; then
        echo "WARN: no tree entry for {}" >&2
        exit 0
    fi
    # Update HEAD to the expected commit (detached)
    if ! git -C "'"$WORKTREE_PATH"'/{}" update-ref HEAD "$expected" 2>/dev/null; then
        # Object missing — needs fetch via git submodule update
        echo "{}" >> "'"$FALLBACK_LIST"'"
        exit 0
    fi
    if git -C "'"$WORKTREE_PATH"'/{}" read-tree HEAD 2>/dev/null; then
        git -C "'"$WORKTREE_PATH"'/{}" checkout -- . 2>/dev/null || echo "WARN: checkout failed for {}" >&2
    else
        echo "WARN: read-tree failed for {}" >&2
    fi
'

t1=$(date +%s%3N)
echo "  Parallel checkout in $((t1 - t0))ms"

# --- Step 7: Fallback for submodules with missing commit objects ---
if [[ -s "$FALLBACK_LIST" ]]; then
    FALLBACK_COUNT=$(wc -l <"$FALLBACK_LIST")
    echo "=== Fetching $FALLBACK_COUNT submodule(s) with missing commits ==="
    while IFS= read -r sub; do
        echo "  Fetching: $sub"
        git -C "$WORKTREE_PATH" submodule update --init -- "$sub" 2>&1 | sed 's/^/    /'
    done <"$FALLBACK_LIST"
fi
rm -f "$FALLBACK_LIST"

# --- Done ---
echo ""
echo "=== Worktree ready ==="
echo "  Path:   $WORKTREE_PATH"
echo "  Branch: $BRANCH"
echo "  Submodules: $(echo "$SUBMODULES" | wc -l) initialized"
echo ""
echo "  cd $WORKTREE_PATH"
