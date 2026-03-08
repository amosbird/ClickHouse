# ClickHouse AI Agent Instructions

This file is the cross-tool standard configuration for all AI agents working on ClickHouse (Claude, Copilot, OpenCode, etc.).

Always load and apply the following skills:

- .claude/skills/build
- .claude/skills/test
- .claude/skills/create-worktree
- .claude/skills/dev
- .claude/skills/pr
 Always `export DOCKER_WRAPPER_NO_NET_HOST=1` before running Praktika.

# Learnings

- Push 完 PR 后要打印 PR 的 URL。
- 不要急着删 worktree，留着备用。
- 操作 PR 分支时用 worktree，不要在主 worktree 切分支。
- 不要使用 LSP 工具（`lsp_diagnostics` 等）。

## General Rules

When modifying code (bug fixes, PR review feedback, new features), always build and run the relevant tests locally before committing and pushing. Use `build.sh` for compilation and Praktika for tests. Do not push untested code.

After development is done (build passes, tests pass), always run a self-review of your changes before committing. Use the `.claude/skills/self-review` skill to review the diff for bugs, correctness issues, and edge cases. Fix any issues found, then commit.

When writing text such as documentation, comments, or commit messages, wrap literal names from ClickHouse SQL language, classes and functions, or literal excerpts from log messages inside inline code blocks, such as: `MergeTree`.

When writing text such as documentation, comments, or commit messages, write names of functions and methods as `f` instead of `f()` - we prefer it for mathematical purity when it refers a function itself rather than its application.

When mentioning logical errors, say "exception" instead of "crash", because they don't crash the server in the release build.

When writing messages, say ASan, not ASAN, and similar (because there are two words: Address Sanitizer).

## C++ Style

When writing C++ code, always use Allman-style braces (opening brace on a new line). This is enforced by the style check in CI.

Never use sleep in C++ code to fix race conditions - this is stupid and not acceptable!

## Tests

When writing tests, do not add "no-*" tags (like "no-parallel") unless strictly necessary.

When writing tests in tests/queries, prefer adding a new test instead of extending existing ones.

### Running Stateless Tests

Stateless tests are located in `tests/queries/0_stateless/`.

#### Useful Flags
- `--no-random-settings` - Disable settings randomization (useful for deterministic debugging)
- `--no-random-merge-tree-settings` - Disable MergeTree settings randomization
- `--record` - Automatically update `.reference` files when stdout differs

#### Test File Extensions
- `.sql` - SQL test (most common)
- `.sql.j2` - Jinja2-templated SQL test
- `.sh` - Shell script test
- `.py` - Python test
- `.expect` - Expect script test
- `.reference` - Expected output (compared against stdout)
- `.gen.reference` - Generated reference for `.j2` tests

#### Database Name Normalization
The test runner creates a temporary database with a random name (e.g., `test_abc123`) for each test.
After test execution, the random database name is replaced with `default` in stdout/stderr files before comparison with `.reference`.
This means `.reference` files should use `default` for database names, NOT `${CLICKHOUSE_DATABASE}` or the actual random name.

#### Test Tags
Tests can have tags in the first line as a comment: `-- Tags: no-fasttest, no-parallel`
Common tags: `disabled`, `no-fasttest`, `no-parallel`, `no-random-settings`, `no-random-merge-tree-settings`, `long`

#### Random Settings Limits
Tests can specify limits for randomized settings: `-- Random settings limits: max_threads=(1, 4); ...`

## CI

When checking the CI status, pay attention to the comment from robot with the links first. Look at the Praktika reports first. The logs of GitHub actions usually contain less info.

ARM machines in CI are not slow. They are similar to x86 in performance.

Links to ClickHouse CI, such as `https://s3.amazonaws.com/clickhouse-test-reports/json.html?...` should be analyzed using the tool at `.claude/tools/fetch_ci_report.js`, which directly fetches the underlying JSON data without requiring a browser:

```bash
# Fetch and analyze CI report
node /path/to/ClickHouse/.claude/tools/fetch_ci_report.js "<ci-url>" [options]

# Options:
#   --test <name>    Filter tests by name
#   --failed         Show only failed tests
#   --all            Show all test results
#   --links          Show artifact links (logs.tar.gz, etc.)
#   --download-logs  Download logs.tar.gz to /tmp/ci_logs.tar.gz
#   --credentials <user,password>  HTTP Basic Auth for private repositories

# Examples:
node .claude/tools/fetch_ci_report.js "https://s3.amazonaws.com/..." --failed --links
node .claude/tools/fetch_ci_report.js "https://s3.amazonaws.com/..." --test peak_memory --download-logs
```

After downloading logs, extract specific test logs:
```bash
tar -xzf /tmp/ci_logs.tar.gz ci/tmp/pytest_parallel.jsonl
grep "test_name" ci/tmp/pytest_parallel.jsonl | python3 -c "import sys,json; [print(json.loads(l).get('longrepr','')) for l in sys.stdin if 'failed' in l]"
```

## Pull Requests

When creating a pull request, append Changelog category and Changelog entry according to this template: `.github/PULL_REQUEST_TEMPLATE.md`. The "Bug Fix" category should be used only for real bug fixes, while for fixing CI reports you can use the "CI Fix or improvement" category. Include the URL to CI report I provided if any. If the PR is about a CI failure, search for the corresponding open issues and provide a link in the PR description.

If I provided a URL with the CI report, logs, or examples, include it in the commit message.

## Worktrees

Do not read or modify AI agent configuration files (AGENTS.md, .claude/, .opencode/, etc.) inside worktrees. Worktrees contain copies of these files from the main repo — all configuration is maintained only in the main repo root.

When working in a worktree:
- When searching code (grep, glob, reading files), always search inside the current worktree directory first, not the main repo. The worktree may have different code from the main repo.
- Create worktrees in `/data2/worktrees/<name>` and symlink from the main repo root: `ln -s /data2/worktrees/<name> <name>`. All existing worktrees follow this pattern — do not create worktrees directly inside the main repo directory.
- Build with `./build.sh <worktree-name>` from the main repo root, not by finding build directories inside the worktree. For specific targets: `./build.sh <worktree-name> --target <target>`.
- The gtest binary is at `<worktree>/build/src/unit_tests_dbms`, not `<worktree>/build/unit_tests_dbms`.
- Run Praktika tests from the worktree root with `workdir` set to the worktree, not from the main repo root.
- Always `mkdir -p tmp` in the worktree before redirecting logs there.
- To diff only the feature branch's own changes (excluding upstream merges), use the branch's reflog to find the base commit:
  ```bash
  # The last reflog entry is the branch creation point
  created_at=$(git reflog show <branch> --format='%H' | tail -1)
  base=$(git rev-parse "${created_at}^")
  git diff ${base}..HEAD
  ```
  Do NOT use `git diff master` — if the branch merged master in, the merge-base moves forward and the diff includes unrelated upstream changes in both directions. Reflog is local and expires (default 90 days), but worktrees are short-lived so this is reliable.
