---
name: create-worktree
description: Create a ClickHouse git worktree with submodules hardlinked from the main repo. Use when the user wants to create a new worktree for ClickHouse development.
argument-hint: <branch-name>
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(cp:*), Bash(ln:*), Bash(ls:*), Bash(rm:*), Bash(mkdir:*), Bash(find:*), Bash(pwd:*), Bash(sed:*), Bash(xargs:*), Bash(echo:*), Bash(bash:*), Bash(for:*), Bash(awk:*), AskUserQuestion
---

# Create ClickHouse Worktree Skill

Create a new git worktree for ClickHouse development with submodules hardlinked from the main repo (independent copies, no network, no extra disk for git objects).

## Arguments

- `$0` (required): Branch name. If the branch already exists, the worktree will check it out. If it doesn't exist, a new branch will be created from the current HEAD of the main repo.

## Process

### 1. Determine the source repo

- Detect the source repo (`MAIN_REPO`) by running `git rev-parse --show-toplevel` from the current working directory.
- Verify it is a git repository. If not, report an error and stop.

### 2. Validate inputs

- Ensure `$0` (branch name) is provided. If not, use `AskUserQuestion` to ask the user for a branch name.
- Compute `SAFE_BRANCH` by replacing all `/` characters in the branch name with `-`. For example, branch `release/25.12` → `SAFE_BRANCH=release-25.12`. This avoids creating nested directories from slashes in branch names.
- Use `AskUserQuestion` to ask the user for the **worktree destination path**. Suggest `<MAIN_REPO>/../<MAIN_REPO_NAME>-<SAFE_BRANCH>` as the default — this places the worktree as a sibling of the main repo directory, named after both the repo and the branch (e.g. `../ClickHouse-my-feature` or `../ClickHouse-release-25.12`). The user may enter a different path if preferred.

Let `WORKTREE_PATH` be the chosen path (resolved to an absolute path).

### 3. Determine worktree path and branch state

- Worktree path: `<WORKTREE_PATH>` (from step 2)
- Check if the worktree directory already exists. If it does, report to the user and stop.
- Check if the branch already exists:
  ```bash
  git -C <MAIN_REPO> branch --list <branch-name>
  git -C <MAIN_REPO> branch --list -r "origin/<branch-name>"
  ```

### 4. Create the worktree

**If branch exists locally:**
```bash
git -C <MAIN_REPO> worktree add <WORKTREE_PATH> <branch-name>
```

**If branch exists on remote only:**
```bash
git -C <MAIN_REPO> worktree add <WORKTREE_PATH> -b <branch-name> origin/<branch-name>
```

**If branch does not exist (create new):**
```bash
git -C <MAIN_REPO> worktree add -b <branch-name> <WORKTREE_PATH>
```

### 5. Set up submodules via hardlinks

This is the key optimization — instead of cloning each submodule from the network, hardlink the git modules directory from the main repo. This gives each worktree an independent copy of the submodule git data (safe to modify independently) without using extra disk space for the object files, and without any network access.

Determine `GIT_DIR` — the `.git` directory of the main repo. For a regular repo this is `<MAIN_REPO>/.git`. For a worktree it may differ; use `git -C <MAIN_REPO> rev-parse --git-common-dir` to get the correct path.

Determine `WORKTREE_ENTRY` — the name git uses for this worktree's entry in `$GIT_DIR/worktrees/`. This is `$(basename <WORKTREE_PATH>)`.

```bash
GIT_DIR=$(git -C <MAIN_REPO> rev-parse --git-common-dir)
WORKTREE_ENTRY=$(basename <WORKTREE_PATH>)

# Hardlink-copy the modules directory from the main repo
cp -al $GIT_DIR/modules \
       $GIT_DIR/worktrees/$WORKTREE_ENTRY/modules

# Fix the worktree pointer inside each submodule's config.
# The hardlinked configs still reference the main repo's worktree path,
# so update them to point to the new worktree's contrib directories.
find $GIT_DIR/worktrees/$WORKTREE_ENTRY/modules -name config -exec \
    sed -i "s|worktree = .*/contrib/|worktree = <WORKTREE_PATH>/contrib/|" {} +

# Some submodules (e.g. contrib/boost) use the worktreeConfig extension,
# storing the actual core.worktree in config.worktree instead of config.
# The hardlinked config.worktree files contain relative paths like
# "../../../../contrib/boost" that resolve correctly from the main repo's
# modules dir but incorrectly from the worktree's modules dir.
# Fix them to use absolute paths pointing to the new worktree.
find $GIT_DIR/worktrees/$WORKTREE_ENTRY/modules -name config.worktree -exec \
    sed -i "s|worktree = .*/contrib/|worktree = <WORKTREE_PATH>/contrib/|" {} +
```

### 6. Write `.git` pointer files and populate submodule working trees

**Do NOT use `git submodule update`** — it runs sequentially across all 129 submodules and takes ~55 seconds. Instead, write `.git` pointer files directly and checkout in parallel (~15 seconds total).

#### 6a. Write `.git` pointer files

Each submodule's `contrib/<name>/` directory needs a `.git` file (not directory) that points to the worktree's modules directory. The pattern is:

```
gitdir: ../../../.git/worktrees/<WORKTREE_ENTRY>/modules/contrib/<name>
```

All 129 submodules are under `contrib/` at uniform depth (3 levels from worktree root to `.git`), so the relative prefix `../../../` is constant.

Get the list of submodules from `.gitmodules`:
```bash
SUBMODULES=$(git -C <WORKTREE_PATH> config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
```

Write the `.git` pointer file for each submodule:
```bash
for sub in $SUBMODULES; do
    name=$(basename "$sub")
    mkdir -p "<WORKTREE_PATH>/$sub"
    echo "gitdir: ../../../.git/worktrees/$WORKTREE_ENTRY/modules/contrib/$name" \
        > "<WORKTREE_PATH>/$sub/.git"
done
```

This takes ~0.2 seconds for all 129 submodules.

#### 6b. Parallel checkout of submodule working trees

Populate all submodule working trees in parallel using `xargs`:

```bash
echo "$SUBMODULES" | xargs -P 96 -I {} bash -c '
    git -C "<WORKTREE_PATH>/{}" read-tree HEAD 2>/dev/null && \
    git -C "<WORKTREE_PATH>/{}" checkout -- . 2>/dev/null || \
    echo "SKIP: {}"
'
```

This runs up to 96 submodule checkouts in parallel, completing in ~15 seconds (I/O bound). The largest submodules (llvm-project ~0.35s, aws ~0.28s, boost ~0.2s) dominate the wall time.

**Why parallel?** The `git submodule foreach` command runs sequentially, and `git submodule update` in this git version does not support `--jobs`. Direct `xargs -P` parallelism is 3-4x faster.

### 7. Report results

Report to the user:
- Source repo: `<MAIN_REPO>`
- Worktree path: `<WORKTREE_PATH>`
- Branch: `<branch-name>` (newly created or existing)
- Submodules: hardlinked from main repo (independent copies, no network cloning)
- Suggest: `cd <WORKTREE_PATH>`

## Examples

- `/create-worktree my-feature` — Create a new worktree with a new branch `my-feature`
- `/create-worktree fix/issue-12345` — Create a new worktree, branch name can contain slashes (slashes are replaced with dashes in the default directory name)

## Notes

- Submodules use hardlinks (`cp -al`) — git object files are hardlinked (no extra disk space) but each worktree has its own independent directory structure and config. Modifying submodules in one worktree does not affect others.
- The main repo must have submodules already cloned (`git submodule update --init` must have been run in the main repo at least once).
- To remove a worktree later:
  ```bash
  rm -rf <WORKTREE_PATH>
  git -C <MAIN_REPO> worktree prune
  ```
- Build directories are NOT shared — you'll need to set up CMake/build separately in the new worktree.
