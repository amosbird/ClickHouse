---
name: dev
description: Integrated ClickHouse development workflow using worktrees, Docker-based builds via Praktika, and a local sccache proxy. Use when the user wants to develop a new feature or fix in an isolated worktree.
argument-hint: <branch-name> [--build-type amd_binary|amd_debug|...] [--skip-cmake]
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(cp:*), Bash(ln:*), Bash(ls:*), Bash(rm:*), Bash(mkdir:*), Bash(docker:*), Bash(python:*), Bash(python3:*), Bash(curl:*), Bash(find:*), Bash(sed:*), Bash(pgrep:*), Bash(ps:*), Bash(kill:*), Bash(mktemp:*), Bash(du:*), Bash(wc:*), Bash(sleep:*), Bash(nohup:*), Bash(export:*), Bash(pwd:*), AskUserQuestion
---

# ClickHouse Dev Workflow Skill

Integrated workflow: create a worktree, build with Praktika inside Docker, with shared sccache via a local S3 cache proxy. Each worktree is an independent development environment with hardlinked submodule git objects (no extra disk, no network).

## Architecture

```
ClickHouse/                    ← meta branch (amos), repo root
├── src/                       ← main worktree (upstream/master)
├── my-feature/                ← feature worktree (sibling of src/)
├── fix-bug-123/               ← another worktree
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
- Builds run inside Docker using Praktika (`python3 -m praktika run build`)
- sccache shares compilation cache across all worktrees via a local S3 proxy on port **8083**
- Port 8083 chosen because port 9000 conflicts with ClickHouse native protocol

## Arguments

- `$0` (required): Branch name for the new feature/fix
- `--build-type` (optional): Praktika build type. Default: `amd_binary` (RelWithDebInfo). Options:
  - `amd_binary` — RelWithDebInfo (default)
  - `amd_debug` — Debug
  - `amd_asan` — ASan
  - `amd_tsan` — TSan
  - `amd_msan` — MSan
  - `amd_ubsan` — UBSan
- `--skip-cmake` (optional): Skip cmake configuration, go straight to ninja. Use for incremental builds.

## Workflow

### 1. Ensure sccache proxy is running

Check if the proxy is running:
```bash
curl -s http://localhost:8083/ 2>/dev/null | python3 -c "import sys,json; json.load(sys.stdin)" && echo "Proxy is running" || echo "Proxy not running"
```

If not running, start it:
```bash
nohup python3 /tmp/gentoo/home/amos/git/ClickHouse/sccache-proxy.py \
  --port 8083 \
  --cache-dir /tmp/gentoo/home/amos/git/ClickHouse/cache/sccache \
  --max-size 80 \
  > /tmp/gentoo/home/amos/git/ClickHouse/cache/proxy.log 2>&1 &
```

Verify:
```bash
curl -s http://localhost:8083/
```
This returns JSON stats: `{"local_hits": 0, "upstream_hits": 0, ...}`

### 2. Create worktree (if it doesn't exist)

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

### 3. Set up submodules via hardlinks

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

### 4. Apply CI patches (if not already present)

The worktree needs five upstream-friendly patches for the dev workflow to work.
Check if they're already applied before patching.

**Patch 1: `ci/jobs/build_clickhouse.py`** — Use `setdefault` for sccache env vars

Check: `grep -q "setdefault" "$WORKTREE_PATH/ci/jobs/build_clickhouse.py"`

If not applied, change `os.environ["X"] = v` to `os.environ.setdefault("X", v)` for all sccache-related variables, and guard the `SCCACHE_S3_NO_CREDENTIALS` assignment behind a check for `SCCACHE_ENDPOINT`:
```python
# Before (original):
os.environ["SCCACHE_BUCKET"] = Settings.S3_ARTIFACT_PATH
...
if info.is_local_run:
    os.environ["SCCACHE_S3_NO_CREDENTIALS"] = "true"

# After (patched):
os.environ.setdefault("SCCACHE_BUCKET", Settings.S3_ARTIFACT_PATH)
...
if info.is_local_run:
    if not os.environ.get("SCCACHE_ENDPOINT"):
        os.environ["SCCACHE_S3_NO_CREDENTIALS"] = "true"
```

**Patch 2: `ci/praktika/runner.py`** — Forward host env vars into Docker

Add `PRAKTIKA_DOCKER_PASSTHROUGH` support before the Docker `cmd =` line:
```python
# Forward host env vars matching PRAKTIKA_DOCKER_PASSTHROUGH into Docker.
passthrough_envs = ""
prefixes = os.environ.get("PRAKTIKA_DOCKER_PASSTHROUGH", "")
if prefixes:
    prefix_list = [p.strip() for p in prefixes.split(",") if p.strip()]
    for env_name, env_val in os.environ.items():
        if any(env_name.startswith(p) for p in prefix_list):
            passthrough_envs += f" -e {env_name}={env_val}"
```
Then include `{passthrough_envs}` in the `docker run` command string.

**Patch 3: `ci/praktika/runner.py`** — Extra Docker volume mounts

Add `PRAKTIKA_DOCKER_EXTRA_MOUNTS` support in the `extra_mounts` construction block:
```python
# Mount additional host paths into Docker via PRAKTIKA_DOCKER_EXTRA_MOUNTS.
# Comma-separated list of Docker --volume specs, e.g.
# "/host/path:/container/path:ro,/another:/another"
docker_extra_mounts = os.environ.get("PRAKTIKA_DOCKER_EXTRA_MOUNTS", "")
if docker_extra_mounts:
    for mount in docker_extra_mounts.split(","):
        mount = mount.strip()
        if mount:
            extra_mounts += f" --volume {mount}"
```

This is **required for worktrees** — the worktree's `.git` file references the parent repo's
`.git/worktrees/<name>/` directory, which is outside the Docker volume mount. Without this,
`git submodule` commands fail inside Docker with `fatal: not a git repository`.

**Patch 4: `ci/praktika/runner.py`** — Custom container name

Add `PRAKTIKA_CONTAINER_NAME` support for the Docker container name:
```python
container_name = os.environ.get("PRAKTIKA_CONTAINER_NAME", "praktika")
```
Required for parallel builds since two Docker containers can't share the same name.

**Patch 5: `ci/jobs/build_clickhouse.py`** — Smart checkout skip for local runs

The `do_checkout` function is modified to skip the expensive network-based submodule
update when submodules are already populated (e.g., via hardlinked worktree setup).
On local runs, it checks `git submodule status` for uninitialized entries ("-" prefix);
if all submodules are initialized, it skips `contrib/update-submodules.sh` entirely.
```python
def do_checkout():
    # On local runs, skip the expensive network-based submodule update
    # if submodules are already populated (e.g. via hardlinked worktree
    # setup).  Check by looking for uninitialized ("-" prefix) entries
    # in `git submodule status`.
    if info.is_local_run:
        out = Shell.get_output("git submodule status")
        if out:
            uninitialized = [
                line
                for line in out.splitlines()
                if line.strip().startswith("-")
            ]
            if not uninitialized:
                print(
                    "NOTE: All submodules already initialized — skipping network update (local run)"
                )
                Shell.check(f"mkdir -p {build_dir}")
                return True

    res = Shell.check(
        f"mkdir -p {build_dir} && git submodule sync && git submodule init"
    )
    res = res and Shell.check(
        "contrib/update-submodules.sh --max-procs 10",
        retries=3,
    )
    return res
```
This saves ~30 seconds on each build by avoiding unnecessary network fetches.

### 5. Seed build directory (if empty)

Before running Praktika, check if the worktree already has a build directory.
If not, copy one from the seed cache. This eliminates cmake reconfiguration (~44s → ~5s)
and Rust crate recompilation (754 crates → 0) on new worktrees. C++ `.o` files are also
copied but ninja will rebuild them (mtime mismatch — see notes below); sccache handles those.

The seed is safe to copy because Praktika mounts every worktree as `--volume .:/ClickHouse`,
so `CMakeCache.txt` always records `CMAKE_SOURCE_DIR=/ClickHouse` and
`CMAKE_BINARY_DIR=/ClickHouse/ci/tmp/build` — identical paths regardless of host worktree location.

```bash
BUILD_TYPE="amd_binary"  # or whatever --build-type was requested
SEED_DIR="$REPO_ROOT/cache/builds/$BUILD_TYPE"
SEED_LOCK="$REPO_ROOT/cache/builds/seed.lock"
BUILD_DIR="$WORKTREE_PATH/ci/tmp/build"

if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$WORKTREE_PATH/ci/tmp"
    mkdir -p "$(dirname "$SEED_DIR")"
    touch "$SEED_LOCK"
    flock -s "$SEED_LOCK" bash -c '
      if [ -d "'"$SEED_DIR"'" ]; then
        cp -al "'"$SEED_DIR"'/" "'"$BUILD_DIR"'/"
        echo "Seeded build directory from $SEED_DIR"
      else
        echo "No seed found — building from scratch"
      fi
    '
fi
```

**Notes:**
- `flock -s` (shared lock) allows multiple worktrees to read the seed concurrently
- `cp -al` uses hardlinks — fast copy, no extra disk until files diverge
- If no seed exists yet (first ever build), the build proceeds from scratch and creates the seed afterward (step 9)
- The seed directory is per build type: `cache/builds/amd_binary/`, `cache/builds/amd_debug/`, etc.
- **Ninja mtime behavior**: ninja compares recorded mtime in `.ninja_log` against current source
  file mtime. Since `git worktree add` checks out sources with fresh timestamps, all `.o` files
  will be marked dirty and recompiled. This is expected — sccache provides ~100% hit rate for
  unchanged sources. The seed's value comes from cmake state and Rust/cargo artifacts (which use
  content-based fingerprinting, not mtime).

### 6. Build with Praktika

**Set environment variables:**
```bash
REPO_ROOT="/tmp/gentoo/home/amos/git/ClickHouse"
WORKTREE_NAME="$(basename "$WORKTREE_PATH")"

export PYTHONPATH=".:./ci"
export PRAKTIKA_DOCKER_PASSTHROUGH="SCCACHE_,AWS_"
export SCCACHE_ENDPOINT="http://localhost:8083"
export AWS_ACCESS_KEY_ID="local"
export AWS_SECRET_ACCESS_KEY="local"

# Required for worktrees — mount the parent .git directory into Docker
export PRAKTIKA_DOCKER_EXTRA_MOUNTS="$REPO_ROOT/.git:$REPO_ROOT/.git"

# Each worktree gets a unique container name and sccache server port.
# This is REQUIRED for parallel builds — without it, two containers on
# --network=host would share port 4226, causing cross-contamination
# (server in container A compiles container B's requests with wrong files).
export PRAKTIKA_CONTAINER_NAME="praktika-$WORKTREE_NAME"
export SCCACHE_SERVER_PORT=$("$REPO_ROOT/allocate-port.sh" "$WORKTREE_NAME")
```

**Run the build:**
```bash
cd "$WORKTREE_PATH"
python3 -m praktika run build
```

This is an alias for `Build (amd_binary)`. Other aliases:
- `build_debug` → `Build (amd_debug)`
- `fast` → `Fast test`
- `functional` → Functional test

For specific build types:
```bash
python3 -m praktika run "Build (amd_debug)"
```

**To skip cmake** (incremental builds after the first full build):
```bash
python3 -m praktika run build --param cmake
```

The `--param cmake` flag causes the build script to skip the cmake configure step.

### 7. Monitor build progress

```bash
# Watch Docker container logs (use container name from PRAKTIKA_CONTAINER_NAME)
docker logs -f praktika-$(basename $WORKTREE_PATH)

# Check sccache proxy stats
curl -s http://localhost:8083/ | python3 -m json.tool

# Check if binary was produced
ls -la "$WORKTREE_PATH/ci/tmp/build/programs/clickhouse"
```

**Build output location**: The build happens inside Docker at `/ClickHouse/ci/tmp/build/`.
Since Praktika mounts `./` into Docker as `--volume .:/ClickHouse`, the build artifacts
appear at `$WORKTREE_PATH/ci/tmp/build/programs/clickhouse` on the host.

### 8. Report results

Report to user:
- Worktree: `$WORKTREE_PATH`
- Branch: `$BRANCH`
- Build status: success/failed
- Binary: `$WORKTREE_PATH/ci/tmp/build/programs/clickhouse`
- sccache stats (hit rate, errors)

### 9. Update build seed (after successful build)

After a successful build, atomically update the seed directory so future worktrees
benefit from the cached cmake state and compiled Rust crates.

```bash
BUILD_TYPE="amd_binary"  # same as step 5
SEED_DIR="$REPO_ROOT/cache/builds/$BUILD_TYPE"
SEED_LOCK="$REPO_ROOT/cache/builds/seed.lock"
BUILD_DIR="$WORKTREE_PATH/ci/tmp/build"

mkdir -p "$(dirname "$SEED_DIR")"
touch "$SEED_LOCK"
flock "$SEED_LOCK" bash -c '
  rm -rf "'"$SEED_DIR"'.old"
  mv "'"$SEED_DIR"'" "'"$SEED_DIR"'.old" 2>/dev/null
  cp -al "'"$BUILD_DIR"'/" "'"$SEED_DIR"'"
  rm -rf "'"$SEED_DIR"'.old" &
'
echo "Build seed updated: $SEED_DIR"
```

**Notes:**
- `flock` (exclusive lock) blocks concurrent seed reads/writes during the update
- Two-step `mv`: rename old seed → `.old`, then hardlink-copy new build → seed. This is safe
  because `rename(2)` on Linux returns `ENOTEMPTY` for non-empty directories, so a single
  `mv -T` cannot atomically replace — we use flock for mutual exclusion instead.
- `rm -rf .old &` runs in background — cleanup doesn't block the workflow
- Only update after a **successful** build — never seed a broken build directory

## sccache Proxy Details

The proxy (`sccache-proxy.py`) is a Python HTTP server that acts as a local S3-compatible endpoint:

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

## Examples

- `/dev my-feature` — Create worktree, start proxy, build with Praktika
- `/dev fix-bug-123 --skip-cmake` — Incremental build (skip cmake step)
- `/dev my-feature --build-type amd_debug` — Build in Debug mode

## Notes

- Worktrees are placed inside the repo root (e.g., `ClickHouse/my-feature/`), NOT outside
- Submodules use hardlinks (`cp -al`) from `src` worktree — ~21GB shared, no extra disk
- The sccache proxy runs on port **8083** (NOT 9000 — that conflicts with ClickHouse native protocol)
- Docker containers use `--network=host`, so they can reach `localhost:8083` on the host
- sccache inside Docker images is v0.10.0, installed in the fasttest base image
- The five CI patches are designed to be upstream-friendly (general-purpose mechanisms, no hardcoded overrides)
- sccache uses OpenDAL library, makes only `GetObject` and `PutObject` S3 API calls
- First build in a new worktree uses seeded build directory (step 5) for cmake and Rust cache; subsequent builds can skip cmake with `--param cmake`
- Build target is `ninja clickhouse-bundle` (includes clickhouse binary and all tools)

## Parallel Builds

Multiple worktrees can be built in parallel. Each worktree is fully isolated:

- **Unique container name**: `PRAKTIKA_CONTAINER_NAME=praktika-<worktree-name>`
- **Unique sccache port**: `SCCACHE_SERVER_PORT` allocated via `allocate-port.sh` (range 4227–4326)
  - Each container starts its own sccache server on its own port, fully isolated
  - Without this, two containers on `--network=host` would share port 4226, causing the
    server in container A to compile container B's requests with wrong source files
- **Shared cache**: All builds share the same local S3 proxy on port 8083, so compilation
  artifacts from one worktree benefit all others
- Heavy resource contention (2× compile jobs) may cause sporadic failures — consider
  limiting concurrency via ninja's auto-detection or staggering builds
- Cache performance observed: first build ~50% upstream hits; second build ~100% local hits

### Port allocation

```bash
# Allocate a port (idempotent — same worktree always gets the same port)
export SCCACHE_SERVER_PORT=$(./allocate-port.sh my-feature)

# List all allocations
./allocate-port.sh --list

# Release a port after removing a worktree
./allocate-port.sh --release my-feature
```

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
