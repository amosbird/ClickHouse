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
├── cache/sccache/             ← local cache (gitignored)
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

The worktree needs two upstream-friendly patches for the sccache proxy to work.
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

### 5. Build with Praktika

**Set environment variables:**
```bash
export PYTHONPATH=".:./ci"
export PRAKTIKA_DOCKER_PASSTHROUGH="SCCACHE_,AWS_"
export SCCACHE_ENDPOINT="http://localhost:8083"
export AWS_ACCESS_KEY_ID="local"
export AWS_SECRET_ACCESS_KEY="local"
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

### 6. Monitor build progress

```bash
# Watch Docker container logs
docker logs -f praktika

# Check sccache proxy stats
curl -s http://localhost:8083/ | python3 -m json.tool

# Check if binary was produced
ls -la "$WORKTREE_PATH/ci/tmp/build/programs/clickhouse"
```

**Build output location**: The build happens inside Docker at `/ClickHouse/ci/tmp/build/`.
Since Praktika mounts `./` into Docker as `--volume .:/ClickHouse`, the build artifacts
appear at `$WORKTREE_PATH/ci/tmp/build/programs/clickhouse` on the host.

### 7. Report results

Report to user:
- Worktree: `$WORKTREE_PATH`
- Branch: `$BRANCH`
- Build status: success/failed
- Binary: `$WORKTREE_PATH/ci/tmp/build/programs/clickhouse`
- sccache stats (hit rate, errors)

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
- The two CI patches are designed to be upstream-friendly (general-purpose mechanisms, no hardcoded overrides)
- sccache uses OpenDAL library, makes only `GetObject` and `PutObject` S3 API calls
- First build in a new worktree needs cmake; subsequent builds can skip it with `--param cmake`
- Build target is `ninja clickhouse-bundle` (includes clickhouse binary and all tools)
