---
name: create-worktree
description: Create a ClickHouse git worktree with submodules hardlinked from the main repo. Use when the user wants to create a new worktree for ClickHouse development.
argument-hint: <branch-name>
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(./create-worktree.sh:*), Bash(cp:*), Bash(ln:*), Bash(ls:*), Bash(rm:*), Bash(mkdir:*), Bash(find:*), Bash(pwd:*), Bash(sed:*), Bash(xargs:*), Bash(echo:*), Bash(bash:*), Bash(for:*), Bash(awk:*), AskUserQuestion
---

# Create ClickHouse Worktree Skill

Create a new git worktree for ClickHouse development with submodules hardlinked from the main repo (independent copies, no network, no extra disk for git objects).

## Arguments

- `$0` (required): Branch name. If the branch already exists, the worktree will check it out. If it doesn't exist, a new branch will be created from `upstream/master`.

## Important

Create worktrees directly inside the main repo directory. No symlinks needed.

## Usage

Run `create-worktree.sh` from the repo root:

```bash
./create-worktree.sh <branch-name> <branch-name> [--ref <base-ref>]
```

### Arguments

- `branch-name` — Branch to check out (creates if doesn't exist)
- `worktree-path` — Where to put the worktree (directly in the repo root)
- `--ref REF` — Base ref for new branches (default: `upstream/master`)

### Examples

```bash
./create-worktree.sh my-feature my-feature                        # new branch from upstream/master
./create-worktree.sh fix/issue-123 fix-issue-123                  # creates worktree in repo root
./create-worktree.sh backport backport --ref release/25.6         # base on release branch
```

## What the script does

1. **Creates git worktree** (~3s for 35,958 files)
2. **Hardlink-copies submodule git data** from the best-matching existing worktree (~200ms–1.2s)
   - Source selection priority: exact target commit > exact `.gitmodules` layout > largest module coverage
3. **Fixes config paths** — replaces source worktree paths with new worktree paths in all submodule configs (~50–200ms)
4. **Writes `.git` pointer files** for all 129 submodules (~700ms)
   - Handles the 3 aws sub-submodules (`aws-c-common`, `aws-c-event-stream`, `aws-checksums`) that use `modules/<name>` instead of `modules/contrib/<name>`
5. **Parallel checkout** of all submodule working trees using `xargs -P` (~14s)
6. **Fallback + repair for missing/inconsistent nested submodules**
   - Runs recursive `submodule sync` + `submodule update --init --recursive` when needed, so nested trees (for example under `contrib/aws`) are consistent

**Total: ~20 seconds** (vs 55+ seconds with `git submodule update`)

## Prerequisites

- At least one existing worktree must have submodules initialized (`git submodule update --init` run at least once)
- The script auto-selects the worktree with the most complete module set

## Cleanup

To remove a worktree:

```bash
rm -rf <worktree-path>
git worktree prune
git branch -D <branch-name>  # if you also want to delete the branch
```

## Notes

- Submodules use hardlinks (`cp -al`) — git object files share inodes but each worktree has independent config/index. Modifying submodules in one worktree does not affect others.
- Build directories are NOT shared — use `build.sh` separately in each worktree.
- The script validates that `.gitmodules` exists in the checked-out tree. If it doesn't (e.g., meta branch), it reports an error with instructions to use `--ref`.
