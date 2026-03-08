---
name: pr
description: Create a pull request on ClickHouse/ClickHouse from a worktree branch. Use when the user wants to submit a PR.
argument-hint: "[branch-name]"
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(gh:*), Bash(ls:*), Bash(realpath:*)
---

# ClickHouse Pull Request Skill

Create a pull request on `ClickHouse/ClickHouse` from a feature branch, following ClickHouse's PR conventions.

## Arguments

- `$0` (optional): Branch name. If omitted, use the current branch of the current worktree.

## Prerequisites

- `gh` CLI authenticated as `amosbird` (fork: `amosbird/ClickHouse`)
- Branch has at least one commit ahead of `upstream/master`
- Code is built and tested (use `/build` and `/test` skills first)

## Workflow

### 1. Gather context

Run these in parallel from the worktree directory:

```bash
# Commits on this branch vs upstream/master
git log --oneline upstream/master..HEAD

# Full diff for understanding changes
git diff upstream/master...HEAD

# Diff stats for summary
git diff upstream/master...HEAD --stat

# Check if branch is pushed
git status -sb
```

### 2. Draft the PR description

**MANDATORY: Show the draft to the user and wait for explicit approval BEFORE creating the PR. Do NOT create the PR without user confirmation.**

Format:

```
**Title:** `<imperative mood, concise description>`

**Body:**

## Summary

<2-5 sentences explaining what and why. Reference specific files/functions changed.>

**Changes:**
- <bullet points listing concrete changes>

### Changelog category (leave one):
- <exactly one category from the template>

### Changelog entry (a user-readable short description of the changes that goes into CHANGELOG.md):

<one sentence, user-facing description>

### Documentation entry for user-facing changes

- [x] Documentation is not required (bug fix, no new user-facing feature)
```

Wait for explicit user approval before proceeding. Do NOT create the PR until the user confirms.

### 3. Push and create PR

**Only after the user explicitly says "go", "ok", "确认", or equivalent approval:**

```bash
# Push branch to fork
git push amos <branch-name> -u

# Create PR
gh pr create \
  --repo ClickHouse/ClickHouse \
  --head amosbird:<branch-name> \
  --base master \
  --title "<title>" \
  --body "$(cat <<'EOF'
<body>
EOF
)"
```

Report the PR URL to the user.

## PR Description Guidelines

### Title
- Use imperative mood: "Fix X", "Add Y", "Accept Z"
- Be specific: "Accept base64 credentials without padding in HTTP Basic Auth" not "Fix auth"

### Changelog categories
Choose exactly one (from `.github/PULL_REQUEST_TEMPLATE.md`):

| Category | When to use |
|---|---|
| New Feature | Wholly new functionality |
| Experimental Feature | New feature behind a flag |
| Improvement | Enhancement to existing feature |
| Performance Improvement | Measurable performance gain |
| Backward Incompatible Change | Breaks existing behavior |
| Build/Testing/Packaging Improvement | Build system, tests, packages |
| Documentation | Docs only (no changelog entry needed) |
| Critical Bug Fix | Crash, data loss, RBAC, or `LOGICAL_ERROR` |
| Bug Fix | User-visible misbehavior in an official stable release |
| CI Fix or Improvement | CI changes (no changelog entry needed) |
| Not for changelog | Internal changes (no changelog entry needed) |

### Key rules
- "Bug Fix" is for **real bugs** in official stable releases only
- For CI report fixes, use "CI Fix or Improvement"
- If the user provided a CI report URL, include it in the PR body
- If the PR relates to a CI failure, search for corresponding open issues and link them
- Wrap ClickHouse SQL names, classes, functions, and log excerpts in inline code blocks
- Write function names as `f` not `f()` (mathematical purity)
- Say "exception" not "crash" for logical errors (they don't crash in release builds)
- Say "ASan" not "ASAN"

## Remotes

| Name | URL | Purpose |
|---|---|---|
| `amos` / `origin` | `git@github.com:amosbird/ClickHouse` | Fork (push here) |
| `upstream` | `https://github.com/ClickHouse/ClickHouse` | Upstream (PR target) |

Always push to `amos` remote and create PR with `--head amosbird:<branch>`.

## Examples

- `/pr` — Create PR from current worktree branch
- `/pr basic-auth-base64` — Create PR for the `basic-auth-base64` branch
