---
name: abck
description: End-to-end ClickHouse development workflow — from feature description to merged PR. Creates a worktree, implements the feature/fix, builds, tests, commits, and creates a PR. Use when the user starts a message with "abck".
argument-hint: <feature-description>
disable-model-invocation: false
---

# End-to-End ClickHouse Development Skill

Orchestrates the full development lifecycle: worktree → implement → build → test → commit → PR.

## Arguments

- `$0` (required): A natural-language description of the feature or fix to implement.

## Workflow Overview

```
1. Plan        — Analyze the request, derive branch name, identify files to change
2. Setup       — Create worktree (parallel with step 1 research)
3. Implement   — Write the code changes and tests
4. Build       — Compile with build.sh
5. Test        — Run relevant tests with Praktika
6. Commit      — Stage, verify (no submodule leaks!), commit
7. PR          — Push and create pull request
```

Each step uses the corresponding skill where available. The key value of this skill is
**orchestration** — knowing which steps can run in parallel and which must be serial.

## Step 1 & 2: Plan + Setup (PARALLEL)

These two steps MUST run in parallel. Worktree creation does not depend on understanding
the code.

### Step 1: Plan

- Parse the feature description to understand what needs to change
- Identify which source files, headers, and test files are involved
- Search the codebase (`grep`, `glob`, `ast_grep_search`) for relevant code
- Derive a short, kebab-case branch name from the feature description
  (e.g., "fix nullable partition pruning" → `fix-nullable-partition-pruning`)
- If the description references a TSearch commit, use the `tsearch-port` skill for evaluation

### Step 2: Setup (create-worktree skill)

```bash
./create-worktree.sh <branch-name>
```

- Creates the worktree on `/data2/worktrees/<branch-name>/` with a symlink from `<repo-root>/<branch-name>/`
- Sets up all 129 submodules via hardlinks (~20 seconds)

## Step 3: Implement

Work inside the worktree directory (`<repo-root>/<branch-name>/`).

- Apply the code changes identified in step 1
- Write regression tests if this is a bug fix
  - For bug fixes: the test MUST reproduce the bug on unfixed code (verify later in step 5)
- Follow ClickHouse style:
  - Allman-style braces in C++
  - Function names as `f` not `f()` in comments/docs
  - "exception" not "crash" for logical errors
  - "ASan" not "ASAN"

## Step 4: Build (dev skill)

```bash
./build.sh <worktree-path>
```

- Uses Docker + ccache for fast builds
- For incremental rebuilds after code changes: `./build.sh <worktree-path> --no-cmake`
- If build fails, fix the errors and rebuild

## Step 5: Test (test skill)

Run the new and related tests:

```bash
export DOCKER_WRAPPER_NO_NET_HOST=1
python -u -m ci.praktika run "Stateless tests (amd_debug, parallel)" --test <test_name> > <logfile> 2>&1
```

- Run from the **worktree root**
- Test the new test first
- Also run any existing related tests to check for regressions
- For bug fix regression tests: verify the test actually fails on unfixed code
  - Use `git checkout upstream/master -- <fixed-file>` to temporarily revert
  - Rebuild with `--no-cmake`, run the test, confirm it fails
  - Restore with `git checkout HEAD -- <fixed-file>`, rebuild

## Step 6: Commit

**CRITICAL: Check for submodule leaks before committing.**

```bash
# Stage the changes
git add <specific-files>

# Verify no accidental submodule changes
git diff --staged --stat
# If any contrib/* entries appear, revert them:
# git checkout upstream/master -- contrib/<name>

# Commit
git commit -m "<message>"
```

Commit message guidelines:
- Imperative mood: "Fix X", "Add Y", "Accept Z"
- Wrap ClickHouse identifiers in backticks
- Include CI report URLs if provided by the user

**After committing, STOP and present results to the user.** Show:
- The commit hash and message
- A summary of what was changed
- Ask the user if they want to proceed to create a PR

Do NOT proceed to Step 7 unless the user explicitly confirms.

## Step 7: PR (pr skill)

**MANDATORY: User must explicitly approve before creating the PR.**

1. Draft the PR title and body following the PR template
2. Show the draft to the user and ask for confirmation
3. Only after the user says "go" (or equivalent approval), push and create the PR

```bash
# Push to fork
git push amos <branch-name> -u

# Create PR
gh pr create \
  --repo ClickHouse/ClickHouse \
  --head amosbird:<branch-name> \
  --base master \
  --title "<title>" \
  --body "$(cat <<'EOF'
## Summary
...

### Changelog category (leave one):
- <category>

### Changelog entry:
<entry>

### Documentation entry for user-facing changes
- [x] Documentation is not required
EOF
)"
```

- Do NOT add `--reviewer` — let maintainers self-assign
- Follow the PR template from `.github/PULL_REQUEST_TEMPLATE.md`

## Parallelization Map

```
Step 1 (Plan) ──────┐
                     ├──→ Step 3 (Implement) → Step 4 (Build) → Step 5 (Test) → Step 6 (Commit) → Step 7 (PR)
Step 2 (Setup) ─────┘
```

- Steps 1 and 2 run in parallel
- Steps 3–7 are sequential (each depends on the previous)
- Within step 5, multiple tests can run sequentially

## Iteration

If any step fails:
- **Build failure**: Fix code, rebuild (`--no-cmake`)
- **Test failure**: Fix code or test, rebuild, retest
- **PR feedback**: Fix code, add new commit (never amend/rebase), push, update PR

Always add new commits — never rebase or amend.

## Critical Warnings

All warnings from the `dev` skill apply here, especially:

- **No submodule leaks**: Always check `git diff --staged --stat` before committing
- **No `git stash`**: Use `git checkout upstream/master -- <file>` for temporary reverts
- **Verify rebuilds**: After reverting code, confirm ninja actually recompiled
- **No `-j` with ninja**: Let it auto-detect
- **Allman braces**: CI will reject K&R style

## Examples

- `abck fix the nullable partition key minmax index merge bug`
- `abck add support for base64 credentials without padding in HTTP Basic Auth`
- `abck optimize the merge of minmax indexes to avoid quadratic complexity`
