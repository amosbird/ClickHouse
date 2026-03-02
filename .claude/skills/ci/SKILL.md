---
name: ci
description: Check CI status for a ClickHouse PR, analyze failures, and download logs. Use when the user wants to check CI, investigate test failures, or analyze CI reports.
argument-hint: <pr-number-or-url> [--failed] [--links]
disable-model-invocation: false
allowed-tools: Bash(gh:*), Bash(node:*), Bash(curl:*), Bash(tar:*), Bash(ls:*), Bash(python3:*), Bash(grep:*), Bash(cat:*), Bash(mktemp:*), Read, Grep, Glob, WebFetch
---

# ClickHouse CI Skill

Check CI status for a PR, analyze failures, download and inspect logs.

## Arguments

- `$0` (required): PR number or URL (e.g., `98405` or `https://github.com/ClickHouse/ClickHouse/pull/98405`)
- `--failed` (optional): Show only failed checks
- `--links` (optional): Show artifact links

## Workflow

### 1. Get CI failure summary (preferred — one command)

```bash
node .claude/tools/fetch_ci_report.js "https://github.com/ClickHouse/ClickHouse/pull/<number>" --failed
```

This fetches ALL Praktika reports for the PR and shows which tests failed. No need to
find report URLs manually — the tool resolves them from the PR number.

### 2. Deep-dive into a specific failure

```bash
# Show specific test details with artifact links
node .claude/tools/fetch_ci_report.js "https://github.com/ClickHouse/ClickHouse/pull/<number>" \
  --report <N> --test <test-name> --all --links

# Download logs for local analysis
node .claude/tools/fetch_ci_report.js "https://github.com/ClickHouse/ClickHouse/pull/<number>" \
  --report <N> --download-logs
```

### 3. Get the actual test diff from job.log

The tool shows which tests failed but does NOT show the output diff. To see
the actual diff (reference vs stdout), download the `job.log` artifact:

```bash
# The --links flag shows artifact URLs including job.log
curl -s "<job.log URL>" | grep -A15 '<test_name>'
```

This shows the unified diff between expected reference and actual output, plus
the `.debuglog` trace showing which commands ran.

### 4. For integration test failures

After downloading logs:
```bash
tar -xzf /tmp/ci_logs.tar.gz ci/tmp/pytest_parallel.jsonl
grep "<test_name>" ci/tmp/pytest_parallel.jsonl | python3 -c "
import sys, json
for l in sys.stdin:
    d = json.loads(l)
    if 'failed' in l:
        print(d.get('longrepr', ''))
"
```

### 5. Alternative: `gh pr checks` (less useful)

```bash
gh pr checks <number> --repo ClickHouse/ClickHouse
```

This only shows pass/fail per job, not per test. Prefer the `fetch_ci_report.js`
approach above.

### 6. Report to user

Provide a concise summary:
- **Overall status**: how many checks passed / failed / pending
- **Failed checks**: name, type (build / stateless / integration / style), and error summary
- **Actionable next steps**: what to fix, whether it's a flaky test, or needs investigation

If the failure is clearly a flaky test (unrelated to the PR changes), say so.

## Examples

```bash
# Quick: which tests failed?
node .claude/tools/fetch_ci_report.js "https://github.com/ClickHouse/ClickHouse/pull/98405" --failed

# Detailed: specific report with artifact links
node .claude/tools/fetch_ci_report.js "https://github.com/ClickHouse/ClickHouse/pull/98405" \
  --report 2 --test 03258 --all --links

# See the actual diff for a stateless test failure
curl -s "<job.log URL from --links>" | grep -A15 '03258_nonexistent_db'
```

- `/ci 98405` — Check CI status for PR #98405
- `/ci https://github.com/ClickHouse/ClickHouse/pull/98405` — Same, from URL
- `/ci 98405 --failed` — Show only failed checks
