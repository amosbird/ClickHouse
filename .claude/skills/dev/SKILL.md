---
name: dev
description: Integrated ClickHouse development workflow using worktrees, Docker-based builds with build.sh, and ccache. Use when the user wants to develop a new feature or fix in an isolated worktree.
argument-hint: <branch-name> [--type release|debug|asan|tsan|msan|ubsan] [--no-cmake]
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(cp:*), Bash(ln:*), Bash(ls:*), Bash(rm:*), Bash(mkdir:*), Bash(docker:*), Bash(python:*), Bash(python3:*), Bash(curl:*), Bash(find:*), Bash(sed:*), Bash(pgrep:*), Bash(ps:*), Bash(kill:*), Bash(mktemp:*), Bash(du:*), Bash(wc:*), Bash(sleep:*), Bash(nohup:*), Bash(export:*), Bash(pwd:*), AskUserQuestion
---

# ClickHouse Dev Workflow Skill

Integrated workflow: create a worktree, build with `build.sh` inside Docker, with shared ccache on local disk. Each worktree is an independent development environment with hardlinked submodule git objects (no extra disk, no network).

## Architecture

```
ClickHouse/                    ← meta branch (amos), repo root
├── src/                       ← main worktree (upstream/master)
├── my-feature/                ← feature worktree (sibling of src/)
├── fix-bug-123/               ← another worktree
├── build.sh                   ← build script (docker + cmake + ninja + ccache)
├── create-worktree.sh         ← fast worktree creation (~20s)
├── cache/ccache-bin/ccache    ← ccache 4.12.3 static binary (committed)
├── cache/ccache/              ← ccache storage (gitignored, shared across all builds)
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
- Builds run inside Docker via `./build.sh` (runs cmake + ninja with ccache automatically)
- Build artifacts land in `<worktree>/build/`
- **ccache** shares compilation cache across all worktrees via a shared local disk cache
- No seed mechanism, no sccache, no S3 proxy — just ccache on local disk

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
  --cmake-only    Run cmake only, don't build
  --shell         Drop into a shell inside the container
```

## How build.sh Works

1. Ensures the ccache binary exists at `cache/ccache-bin/ccache` (downloads if missing)
2. Mounts the worktree, ccache binary, and ccache storage into Docker
3. cmake decision: runs cmake **only** if `build.ninja` doesn't exist (first build). `--cmake` forces it; `--no-cmake` skips it.
4. Runs `ninja` inside the Docker container (`clickhouse/binary-builder:0261cd99929ade4b3e59_amd`)
5. All compilations go through ccache — cache hits are near-instant

### ccache Configuration

- Binary: `cache/ccache-bin/ccache` (v4.12.3, static x86_64, committed to repo)
- Storage: `cache/ccache/` (gitignored, 50 GB max, shared across all containers)
- Mounted into container as `/usr/local/bin/ccache:ro` and `/ccache`
- Environment: `CCACHE_DEPEND=1`, `CCACHE_SLOPPINESS=file_macro,time_macros,include_file_mtime`, `CCACHE_NOHASHDIR=1`, `CCACHE_BASEDIR=/ClickHouse`

## Performance

| Scenario | Time |
|---|---|
| Cold build (no cache) | ~19 minutes |
| Warm rebuild (rm -rf build, cmake + ninja) | ~1.5 minutes |
| Incremental rebuild (build.ninja exists) | ~31 seconds |
| Link-only (delete binary) | ~10 seconds |
| ccache hit rate (warm cache) | 100% |

## Workflow

### 1. Create worktree (if it doesn't exist)

```bash
REPO_ROOT="/tmp/gentoo/home/amos/git/ClickHouse"
./create-worktree.sh <branch-name>
```

This creates the worktree and sets up all 129 submodules via hardlinks in ~20 seconds.

### 2. Build

```bash
./build.sh <worktree-path>
```

For a specific build type:
```bash
./build.sh <worktree-path> --type debug
```

For incremental builds (skip cmake, ninja only):
```bash
./build.sh <worktree-path> --no-cmake
```

To drop into a shell inside the container for debugging:
```bash
./build.sh <worktree-path> --shell
```

### 3. Monitor build progress

```bash
# Watch Docker container logs (container name is build-<worktree-name>)
docker logs -f build-$(basename "<worktree-path>")

# Check ccache stats
CCACHE_DIR=cache/ccache cache/ccache-bin/ccache -s

# Check if binary was produced
ls -la <worktree-path>/build/programs/clickhouse
```

**Build output location**: `<worktree-path>/build/programs/clickhouse`

### 4. Report results

Report to user:
- Worktree path
- Branch name
- Build status: success/failed
- Binary: `<worktree-path>/build/programs/clickhouse`
- ccache stats (hit rate)

## Docker Image

- Image: `clickhouse/binary-builder:0261cd99929ade4b3e59_amd`
- Contains: clang-21, cmake 4.1.2, ninja 1.12.1
- Does NOT contain ccache (we mount it in from `cache/ccache-bin/ccache`)
- Container mount: worktree → `/ClickHouse`
- Container name: `build-<worktree-name>` (unique per worktree, safe for parallel builds)

## cmake Flags

- `-DENABLE_RUST=0` (disabled for speed)
- `-DENABLE_THINLTO=0` (disabled for speed)
- `-DENABLE_TESTS=1` (tests enabled)
- `-DCOMPILER_CACHE=ccache`
- Toolchain: `cmake/linux/toolchain-x86_64.cmake`

## Parallel Builds

Multiple worktrees can be built in parallel. Each worktree is fully isolated:

- **Unique container name**: `build-<worktree-name>` (assigned automatically by `build.sh`)
- **Shared ccache**: All builds share the same ccache directory (`cache/ccache/`). ccache 4.12.3 supports concurrent multi-process access.

## Critical Warnings

- Do NOT use `-j` with ninja (let it decide automatically)
- Do NOT use sccache (obsolete — we use ccache now)
- Do NOT use seed/mtime-touching approaches (unsafe across different commits)
- Do NOT use Praktika for local builds

## Submodule + Worktree Sharing: Design Notes

Git's official documentation (BUGS section) still states worktree+submodule support
is "incomplete" as of git 2.53. No `--recurse-submodules` flag exists for `git worktree add`.

**Why hardlinks (`cp -al`) over alternatives:**
- `git submodule update --reference`: Still requires network fetches; `--reference` only provides
  alternates for object lookups. Our `cp -al` is purely local, zero network.
- Symlinks: More fragile than hardlinks, git doesn't support symlinked `.git` directories well.
- Re-downloading: 129 submodules, ~7.5GB — too slow for worktree creation.

## Examples

- `/dev my-feature` — Create worktree, build with `build.sh` (release)
- `/dev fix-bug-123 --no-cmake` — Incremental build (skip cmake)
- `/dev my-feature --type debug` — Build in Debug mode
- `/dev my-feature --type asan` — Build with ASan
