---
name: test
description: Run ClickHouse stateless or integration tests. Use when the user wants to run or execute tests.
argument-hint: "[test-name] [--flags]"
disable-model-invocation: false
allowed-tools: Task, Bash(python:*), Bash(python3:*), Bash(mktemp:*), Bash(export:*), Bash(ls:*), Bash(test:*), Bash(realpath:*)
---

# ClickHouse Test Runner Skill

Run stateless tests from `tests/queries/0_stateless/` or integration tests from `tests/integration/` using Praktika.

## Arguments

- `$0` (optional): Test name (e.g., `03312_issue_63093` for stateless or `test_keeper_three_nodes_start` for integration), or empty to prompt for selection
- `$1+` (optional): Additional flags passed through to Praktika

## Test Types

The skill automatically detects the test type:
- **Stateless tests**: Located in `tests/queries/0_stateless/`, named like `NNNNN_description` (e.g., `03312_issue_63093`)
- **Integration tests**: Located in `tests/integration/`, named like `test_*` (e.g., `test_keeper_three_nodes_start`)

Detection logic:
1. If test name starts with `test_` → Integration test
2. If test name matches pattern `\d{5}_.*` → Stateless test
3. If currently viewing file in `tests/integration/test_*/` → Integration test
4. If currently viewing file in `tests/queries/0_stateless/` → Stateless test

## Test Selection

If no test name is provided in arguments, prompt the user with `AskUserQuestion`:

**Question: "Which test would you like to run?"**
- **Option 1: "Currently viewed test"** - Extract test name from currently opened file in IDE
  - Description: "Run the test file currently open in your editor"
  - Only available if a test file is currently open in the IDE
  - For stateless: Extract filename without extension from `tests/queries/0_stateless/03312_issue_63093.sh` → `03312_issue_63093`
  - For integration: Extract directory name from `tests/integration/test_keeper_three_nodes_start/test.py` → `test_keeper_three_nodes_start`

- **Option 2: "Custom test name"** - User provides test name
  - Description: "Specify a test name manually"
  - User can provide the test name via the "Other" field
  - Stateless examples: `03312_issue_63093`, `00029_test_zookeeper`
  - Integration examples: `test_keeper_three_nodes_start`, `test_access_control_on_cluster`

## Build Directory Auto-Detection

Before running any test, the skill must locate a valid build directory containing the `clickhouse` binary.

**Search for `programs/clickhouse` binary in the following locations, in order. Stop at the first match:**

1. `build/programs/clickhouse`
2. `build/RelWithDebInfo/programs/clickhouse`
3. `build/Debug/programs/clickhouse`
4. `build_debug/programs/clickhouse`
5. `build_asan/programs/clickhouse`
6. `build_tsan/programs/clickhouse`
7. `build_msan/programs/clickhouse`
8. `build_ubsan/programs/clickhouse`

The **build directory** is the path up to and including the parent of `programs/` (e.g., if `build/RelWithDebInfo/programs/clickhouse` is found, the build directory is `build/RelWithDebInfo`).

**If no binary is found:**
- Use `AskUserQuestion` to ask:
  - Question: "No ClickHouse binary found in well-known build directories. Where is your build directory?"
  - Option 1: "Build first" - Description: "Run /build to compile ClickHouse first"
  - Option 2: "Specify path" - Description: "Enter the path to an existing build directory containing programs/clickhouse"
- Do NOT proceed without a valid build directory.

**If a binary is found:**
- Report to the user: "Using build directory: `<path>`"

## Test Execution Process

Both stateless and integration tests are run via **Praktika**, which manages Docker containers, the ClickHouse server, minio, azurite, and all other dependencies automatically. **No manual server management is needed.**

### Environment Setup (MANDATORY)

Before running any Praktika command, **always** set:

```bash
export DOCKER_WRAPPER_NO_NET_HOST=1
```

This prevents the user's Docker wrapper (`~/scripts/docker`) from injecting `--net=host` into Docker containers, which causes port conflicts (e.g., minio port 11111) when running concurrent tests.

### For Stateless Tests

1. **Determine test name and type:**
   - If `$ARGUMENTS` is provided, use it as the test name
   - Otherwise, use `AskUserQuestion` to prompt user for test selection
   - Detect test type using patterns described in "Test Types" section
   - For stateless: Test name should NOT include file extension (`.sql`, `.sh`, etc.)

2. **Create log file:**
   ```bash
   mktemp /tmp/test_clickhouse_XXXXXX.log
   ```
   - **IMMEDIATELY report to the user:**
     - "Test logs will be written to: [log file path]"
     - Then display in a copyable code block:
       ```bash
       tail -f [log file path]
       ```

3. **Run the stateless test with Praktika:**
   ```bash
   export DOCKER_WRAPPER_NO_NET_HOST=1 && python -u -m ci.praktika run "Stateless tests (amd_debug, parallel)" --test <test_name> > [log file path] 2>&1
   ```

   **Important:**
   - Run from the **repository root directory** (the worktree root if using worktrees)
   - Use `python -u` for unbuffered output
   - The job name MUST be `"Stateless tests (amd_debug, parallel)"` — using just `"Stateless tests"` matches 30+ jobs and fails
   - Praktika auto-detects the binary from `build/programs/clickhouse` — do NOT pass `--path` unless the binary is in a non-standard location
   - If `--path` is needed, pass the **directory** containing the binary (e.g., `build/programs`), NOT the binary path itself — Praktika asserts `Path(args.path).is_dir()`
   - Redirect both stdout and stderr to the log file
   - Run in the background using `run_in_background: true`
   - **After starting the test**, report: "Test started in the background. Waiting for completion..."

4. **Wait for test completion:**
   - Use TaskOutput with `block=true` to wait for the background task to finish

### For Integration Tests

1. **Determine test name:**
   - If `$ARGUMENTS` is provided, use it as the test name
   - Otherwise, use `AskUserQuestion` to prompt user for test selection
   - Test name should be the directory name (e.g., `test_keeper_three_nodes_start`)

2. **Create log file:**
   ```bash
   mktemp /tmp/test_clickhouse_XXXXXX.log
   ```
   - **IMMEDIATELY report to the user:**
     - "Test logs will be written to: [log file path]"
     - Then display in a copyable code block:
       ```bash
       tail -f [log file path]
       ```

3. **Run the integration test with Praktika:**
   ```bash
   export DOCKER_WRAPPER_NO_NET_HOST=1 && python -u -m ci.praktika run "integration" --test <test_name> [--path <absolute_binary_dir>] > [log file path] 2>&1
   ```

   **Important:**
   - Run from the **repository root directory**
   - Use `python -u` for unbuffered output
   - If `--path` is needed, it MUST be an **absolute path** to the **directory** containing the binary — relative paths break inside Docker
   - Run in the background using `run_in_background: true`
   - **After starting the test**, report: "Test started in the background. Waiting for completion..."

4. **Wait for test completion:**
   - Use TaskOutput with `block=true` to wait for the background task to finish

## Result Analysis

5. **Report results:**

   **ALWAYS use Task tool to analyze results** (both pass and fail):
   - Use Task tool with `subagent_type=general-purpose` to analyze the test output
   - **Pass the log file path** to the Task agent — let it read the file directly
   - Example Task prompt: "Read and analyze the test output from: /tmp/test_clickhouse_abc123.log"
   - The Task agent should read the file and provide:

     **If tests passed:**
     - Confirm all tests passed
     - Report execution time
     - Show summary (e.g., "Failed: 0, Passed: 1, Skipped: 0")
     - Keep response brief

     **If tests failed:**
     - Parse the output to identify which test failed
     - Extract the relevant error messages and differences
     - Identify the root cause if possible
     - Provide a concise summary with:
       - Test name that failed
       - What assertion or comparison failed
       - Expected vs actual output (show the diff)
       - Any error messages or exceptions
       - Brief explanation of the root cause
     - Filter out excessive verbose logs and focus on the actual failure

   - Return ONLY the Task agent's summary to the user
   - Do NOT return the full raw test output

   **After receiving the summary:**
   - If tests passed: Done, no further action needed
   - If tests failed:
     - Present the summary to the user first
     - **MANDATORY:** Use `AskUserQuestion` to prompt: "Do you want deeper analysis of this test failure?"
       - Option 1: "Yes, investigate further" - Description: "Launch a subagent to investigate the root cause across the codebase"
       - Option 2: "No, I'll fix it myself" - Description: "Skip deeper analysis and proceed without investigation"
     - If user chooses "Yes, investigate further":
       - **CRITICAL: DO NOT read files, edit code, or fix the issue yourself**
       - **MANDATORY: Use Task tool to launch a subagent for deep analysis only (NO FIXES)**
       - Use Task tool with `subagent_type=Explore` to search for related code patterns, or find where functions/queries are implemented
       - For complex failures involving multiple components, use Task tool with `subagent_type=general-purpose` to investigate root causes
       - Provide specific prompts to the agent based on the failure type
       - The subagent should only investigate and analyze, NOT edit or fix code
       - **CRITICAL: Return ONLY the agent's summary of findings to the user**
     - If user chooses "No, I'll fix it myself":
       - Skip deeper analysis

## Test File Structure

### Stateless Tests
- **Location**: `tests/queries/0_stateless/`
- **Extensions**: `.sql`, `.sh`, `.py`, `.sql.j2`, `.expect`
- **Reference files**: `.reference` (expected output)
- **Test name format**: `NNNNN_description` (e.g., `03312_issue_63093`)

### Integration Tests
- **Location**: `tests/integration/test_*/`
- **Format**: Python pytest files (`test.py` or `test_*.py`)
- **Directory structure**: Each test is a directory named `test_*`
- **Test name format**: `test_*` (e.g., `test_keeper_three_nodes_start`)
- **Dependencies**: Uses Docker containers, pytest fixtures, and helper modules

## Examples

### Stateless Tests
- `/test` - Prompt to select test (currently viewed or custom name)
- `/test 03312_issue_63093` - Run specific stateless test by name
- `/test 04005_basic_auth_base64_no_padding` - Run specific stateless test

### Integration Tests
- `/test test_keeper_three_nodes_start` - Run specific integration test
- `/test test_access_control_on_cluster` - Run integration test by name

## Notes

### General
- Run from repository root directory (worktree root if using worktrees)
- Test type is automatically detected based on name pattern or file location
- **ALL tests run via Praktika** — no manual server management needed
- Praktika manages Docker containers, ClickHouse server, minio, azurite, kafka, etc.
- **MANDATORY:** `export DOCKER_WRAPPER_NO_NET_HOST=1` before any Praktika command
- **MANDATORY:** ALL test output (success or failure) MUST be analyzed by a Task agent
- **MANDATORY:** For test failures, MUST prompt user if they want deeper analysis
- **CRITICAL:** Test output is redirected to a unique log file created with `mktemp`. The log file path is reported to the user BEFORE starting the test, allowing real-time monitoring with `tail -f`.

### Stateless Tests
- Test names do NOT include extensions (use `03312_issue_63093`, not `03312_issue_63093.sh`)
- Job name must be `"Stateless tests (amd_debug, parallel)"` — not just `"Stateless tests"`
- Praktika auto-detects binary from `build/programs/clickhouse`
- If `--path` is needed, pass a **directory** (not a file path) — Praktika asserts `Path(args.path).is_dir()`

### Integration Tests
- Test names are directory names (use `test_keeper_three_nodes_start`, not `test.py`)
- Integration tests manage their own Docker containers
- Tests may take longer due to container startup and teardown
- If `--path` is needed, use an **absolute path** to the directory containing the binary
- Docker daemon must be running and accessible
- **Debugging failures:** Check `_instances*` directories inside the test directory for per-node logs and configs
- **Permission issues:** Files in `_instances*` are owned by root. Fix with:
  ```bash
  sudo chown -R $(id -u):$(id -g) tests/integration/<test_name>/_instances*/
  ```
