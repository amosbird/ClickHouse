---
name: dev
description: Integrated ClickHouse development workflow using worktrees, Docker-based builds with build.sh, and a local sccache proxy. Use when the user wants to develop a new feature or fix in an isolated worktree.
argument-hint: <branch-name> [--type release|debug|asan|tsan|msan|ubsan] [--no-cmake]
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(cp:*), Bash(ln:*), Bash(ls:*), Bash(rm:*), Bash(mkdir:*), Bash(docker:*), Bash(python:*), Bash(python3:*), Bash(curl:*), Bash(find:*), Bash(sed:*), Bash(pgrep:*), Bash(ps:*), Bash(kill:*), Bash(mktemp:*), Bash(du:*), Bash(wc:*), Bash(sleep:*), Bash(nohup:*), Bash(export:*), Bash(pwd:*), AskUserQuestion
---

# ClickHouse Dev Workflow Skill

Integrated workflow: create a worktree, build with `build.sh` inside Docker, with shared sccache via a local S3 cache proxy. Each worktree is an independent development environment with hardlinked submodule git objects (no extra disk, no network).

## Architecture

```
ClickHouse/                    ← meta branch (amos), repo root
├── src/                       ← main worktree (upstream/master)
├── my-feature/                ← feature worktree (sibling of src/)
├── fix-bug-123/               ← another worktree
├── build.sh                   ← build script (docker + cmake + ninja + sccache)
├── sccache-proxy.py           ← local S3 cache proxy
├── cache/sccache/             ← sccache cache (gitignored)
├── cache/builds/<type>/       ← build directory seeds (gitignored)
├── .claude/skills/dev/SKILL.md ← this file
└── .git/
    └── worktrees/
        ├── src/modules/       ← submodule git objects (~21GB)
        ├── my-feature/modules/ ← hardlinked from src (no extra disk)
        └── fix-bug-123/modules/
```

**Key design decisions:**
- Worktrees are **inside** the meta branch root (e.g., `ClickHouse/my-feature/`), NOT outside as siblings
- Submodule git objects are shared via `cp -al` (hardlinks) — independent directories, shared disk blocks
- Builds run inside Docker via `./build.sh` (runs cmake + ninja + sccache automatically)
- Build artifacts land in `<worktree>/build/` (NOT `ci/tmp/build/`)
- sccache shares compilation cache across all worktrees via a local S3 proxy on port **8083**
- Port 8083 chosen because port 9000 conflicts with ClickHouse native protocol

## Arguments

- `$0` (required): Branch name for the new feature/fix
- `--type` (optional): Build type. Default: `release`. Options:
  - `release` — RelWithDebInfo (default)
  - `debug` — Debug
  - `asan` — ASan
  - `tsan` — TSan
  - `msan` — MSan
  - `ubsan` — UBSan
- `--no-cmake` (optional): Skip cmake, run ninja only. Use for incremental builds.

## build.sh Usage

```
./build.sh [WORKTREE_PATH] [OPTIONS]

Options:
  --type TYPE     Build type: release, debug, asan, tsan, msan, ubsan (default: release)
  --target TGT    Ninja target (default: clickhouse)
  --cmake         Force cmake even if build.ninja exists
  --no-cmake      Skip cmake, run ninja only
  --no-seed       Don't use/update build seed
  --cmake-only    Run cmake only, don't build
  --shell         Drop into a shell inside the container
```

## How build.sh Works

1. Ensures the sccache proxy is running on port 8083
2. If build dir doesn't exist and a seed is available: `cp -al` seed → build dir (~590ms for 20GB)
3. Patches `build.ninja` to remove `VerifyGlobs` from `RERUN_CMAKE` deps (prevents cmake re-run cascade)
4. cmake decision: runs cmake **only** if `build.ninja` doesn't exist (first build from scratch). Skipped on seeded or existing builds. `--cmake` forces it; `--no-cmake` skips it.
5. After cmake: patches `build.ninja` inside the container (post-cmake patching)
6. Runs ninja inside the Docker container (`clickhouse/binary-builder:0261cd99929ade4b3e59_amd`)
7. Updates the seed after a successful build (atomic via `flock`)

## Workflow

### 1. Create worktree (if it doesn't exist)

**Determine repo root:**
```bash
REPO_ROOT="/tmp/gentoo/home/amos/git/ClickHouse"
```

**Create worktree inside the repo root** (as a sibling of `src/`):
```bash
BRANCH="<branch-name>"
WORKTREE_PATH="$REPO_ROOT/$BRANCH"
git -C "$REPO_ROOT/src" worktree add -b "$BRANCH" "$WORKTREE_PATH"
```

If the branch already exists:
```bash
git -C "$REPO_ROOT/src" worktree add "$WORKTREE_PATH" "$BRANCH"
```

### 2. Set up submodules via hardlinks

```bash
GIT_DIR=$(git -C "$REPO_ROOT/src" rev-parse --git-common-dir)
WORKTREE_ENTRY=$(basename "$WORKTREE_PATH")

# Hardlink-copy the modules directory from src worktree
cp -al "$GIT_DIR/worktrees/src/modules" "$GIT_DIR/worktrees/$WORKTREE_ENTRY/"

# Fix worktree paths in submodule configs
find "$GIT_DIR/worktrees/$WORKTREE_ENTRY/modules" -name config -exec \
    sed -i "s|worktree = .*/contrib/|worktree = $WORKTREE_PATH/contrib/|" {} +

find "$GIT_DIR/worktrees/$WORKTREE_ENTRY/modules" -name config.worktree -exec \
    sed -i "s|worktree = .*/contrib/|worktree = $WORKTREE_PATH/contrib/|" {} +

# Register and populate submodules (purely local, no network)
git -C "$WORKTREE_PATH" submodule init
git -C "$WORKTREE_PATH" submodule update
```

If `submodule update` leaves empty working trees, run:
```bash
git -C "$WORKTREE_PATH" submodule foreach \
    '(git read-tree HEAD && git checkout -- .) 2>/dev/null || echo "SKIP: $name"'
```

### 3. Build

```bash
cd "$REPO_ROOT"
./build.sh "$WORKTREE_PATH"
```

For a specific build type:
```bash
./build.sh "$WORKTREE_PATH" --type debug
```

For incremental builds (skip cmake, ninja only):
```bash
./build.sh "$WORKTREE_PATH" --no-cmake
```

To drop into a shell inside the container for debugging:
```bash
./build.sh "$WORKTREE_PATH" --shell
```

### 4. Monitor build progress

```bash
# Watch Docker container logs (container name is build-<worktree-name>)
docker logs -f build-$(basename "$WORKTREE_PATH")

# Check sccache proxy stats
curl -s http://localhost:8083/ | python3 -m json.tool

# Check if binary was produced
ls -la "$WORKTREE_PATH/build/programs/clickhouse"
```

**Build output location**: `$WORKTREE_PATH/build/programs/clickhouse`

### 5. Report results

Report to user:
- Worktree: `$WORKTREE_PATH`
- Branch: `$BRANCH`
- Build status: success/failed
- Binary: `$WORKTREE_PATH/build/programs/clickhouse`
- sccache stats (hit rate, errors)

## Performance

| Scenario | Time |
|---|---|
| Seeded no-op build (no changes) | ~1 second (ninja: no work to do) |
| Incremental build (1 file changed) | ~11 seconds (1 compile + 2 link steps) |
| From-scratch build | ~25 minutes |
| sccache hit rate (warm cache) | ~100% |

## Docker Image

- Image: `clickhouse/binary-builder:0261cd99929ade4b3e59_amd`
- Contains: clang-21, cmake 4.1.2, ninja 1.12.1, sccache 0.10.0
- Container mount: worktree → `/ClickHouse`
- Container name: `build-<worktree-name>` (unique per worktree, safe for parallel builds)

## cmake Flags

- `-DENABLE_RUST=0` (disabled for speed)
- `-DENABLE_THINLTO=0` (disabled for speed)
- `-DENABLE_TESTS=1` (tests enabled)
- `-DCOMPILER_CACHE=sccache`
- Toolchain: `cmake/linux/toolchain-x86_64.cmake`

## Build Directory and Seed

- **Build dir**: `<worktree>/build/` (created by `build.sh`, or seeded from `cache/builds/<type>/`)
- **Seed**: `cache/builds/<type>/` — hardlink copy of a prior successful build dir
- **Seeding**: `build.sh` performs `cp -al` seed → build dir at startup (~590ms for 20GB)
- **Seed update**: after a successful build, `build.sh` atomically replaces the seed via `flock`

The seed is safe across worktrees because `build.sh` mounts every worktree as `--volume .:/ClickHouse`, so `CMakeCache.txt` always records `CMAKE_SOURCE_DIR=/ClickHouse` and `CMAKE_BINARY_DIR=/ClickHouse/build` — identical paths regardless of host worktree location.

**Critical warnings — do NOT:**
- Touch `.o`/`.a` files or run `ninja -t restat` on seeded builds — causes mtime inconsistency → full rebuild
- Run cmake unnecessarily — `VerifyGlobs` causes `build.ninja` regeneration → full rebuild
- Use Praktika (obsolete approach)
- Use `-j` with ninja (let it decide automatically)

## sccache Proxy Details

The proxy (`sccache-proxy.py`) is a Python HTTP server that acts as a local S3-compatible endpoint. `build.sh` starts it automatically if it isn't running; port 8083 is always used.

- **On GET**: Check local cache → on miss, fetch from `s3.us-east-1.amazonaws.com`, cache locally, return
- **On PUT**: Store to local cache only (captures local compilation artifacts)
- **Inflight deduplication**: If multiple threads request the same missing key, only one fetches from upstream
- **LRU eviction**: When cache exceeds max size (default 80GB), evicts least-recently-accessed entries
- **Stats**: `curl http://localhost:8083/` returns JSON stats

**Proxy stats fields:**
- `local_hits` — served from local cache (your own builds or previously fetched upstream objects)
- `upstream_hits` — fetched from upstream S3 (ClickHouse CI cache)
- `upstream_dedup` — deduplicated concurrent fetches (waited on another thread's result)
- `misses` — not found anywhere (new compilation unit, no cache)
- `puts` — stored locally (your compilation output)
- `errors` — any failures

## Parallel Builds

Multiple worktrees can be built in parallel. Each worktree is fully isolated:

- **Unique container name**: `build-<worktree-name>` (assigned automatically by `build.sh`)
- **Unique sccache port**: allocated via `allocate-port.sh` (range 4227–4326)
  - Each container starts its own sccache server on its own port, fully isolated
  - Without this, two containers on `--network=host` would share port 4226, causing cross-contamination
- **Shared cache**: All builds share the same local S3 proxy on port 8083, so compilation artifacts from one worktree benefit all others

### Port allocation

```bash
# Allocate a port (idempotent — same worktree always gets the same port)
./allocate-port.sh my-feature

# List all allocations
./allocate-port.sh --list

# Release a port after removing a worktree
./allocate-port.sh --release my-feature
```

## Examples

- `/dev my-feature` — Create worktree, build with `build.sh` (release)
- `/dev fix-bug-123 --no-cmake` — Incremental build (skip cmake)
- `/dev my-feature --type debug` — Build in Debug mode
- `/dev my-feature --type asan` — Build with ASan

## Notes

- Worktrees are placed inside the repo root (e.g., `ClickHouse/my-feature/`), NOT outside
- Submodules use hardlinks (`cp -al`) from `src` worktree — ~21GB shared, no extra disk
- The sccache proxy runs on port **8083** (NOT 9000 — that conflicts with ClickHouse native protocol)
- Docker containers use `--network=host`, so they can reach `localhost:8083` on the host
- sccache inside the Docker image is v0.10.0
- `build.sh` handles the entire build lifecycle: sccache proxy, seeding, cmake, ninja, seed update

## Submodule + Worktree Sharing: Design Notes

Git's official documentation (BUGS section) still states worktree+submodule support
is "incomplete" as of git 2.53. No `--recurse-submodules` flag exists for `git worktree add`.

**Why hardlinks (`cp -al`) over alternatives:**
- `git submodule update --reference`: Still requires network fetches; `--reference` only provides
  alternates for object lookups. Our `cp -al` is purely local, zero network.
- Symlinks: More fragile than hardlinks, git doesn't support symlinked `.git` directories well.
- Re-downloading: 129 submodules, ~7.5GB — too slow for worktree creation.
- Custom tooling (repo/gclient): Overkill for our use case.

**Known risks of hardlinks (mitigated in our setup):**
- `git gc` / repack could corrupt shared objects if run concurrently — we never run `git gc`
  in worktrees (builds are in Docker, and all worktrees track the same upstream master).
- Submodule version conflicts if worktrees check out different submodule commits simultaneously —
  mitigated because all worktrees track the same upstream master branch.
- Cannot move worktrees with `git worktree move` — not needed in our workflow.
