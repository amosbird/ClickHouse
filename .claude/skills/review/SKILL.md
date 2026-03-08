---
name: review
description: Review a ClickHouse pull request and create a pending (draft) review for the user to approve before submission. Use when the user wants to review a PR.
argument-hint: "<PR-number-or-URL>"
disable-model-invocation: false
allowed-tools: Bash(git:*), Bash(gh:*), Bash(ls:*), Bash(realpath:*), Bash(node:*), Bash(python3:*), Read, Grep, Glob
---

# ClickHouse PR Review Skill

Review a pull request on `ClickHouse/ClickHouse` and create a **PENDING** (draft) review for the user to inspect, edit, and submit on GitHub.

## Arguments

- `$0`: PR number or URL (required)

## Key Rules

1. **NEVER submit a review** — never set `event` to `"COMMENT"`, `"APPROVE"`, or `"REQUEST_CHANGES"`. These immediately publish the review, making it visible to everyone and non-retractable.
2. **Create reviews in PENDING state only** — omit the `event` field when calling the GitHub API. PENDING reviews are only visible to the token owner (the user) and serve as a draft for the user to inspect, edit, and submit on GitHub at their discretion.
3. **The purpose of the PENDING review is for the user to read on GitHub** — the user wants to see inline comments anchored to specific diff lines in the GitHub UI. Always create the PENDING review with inline comments via the API.

## Review Style Guidelines

- **Write like a human reviewer**, not a bot. No emoji headers, no bold category labels (like "🐛 Bug:", "⚡ Performance:"), no structured/formulaic titles. Just natural, conversational code review comments as a colleague would write them.
- Be specific: reference exact file paths, line numbers, variable names
- Be actionable: suggest fixes, not just problems
- Wrap ClickHouse identifiers in inline code blocks
- Say "exception" not "crash" for logical errors
- Say "ASan" not "ASAN"

## Examples

- `/review 96563` — Review PR #96563
- `/review https://github.com/ClickHouse/ClickHouse/pull/96563` — Same, from URL
