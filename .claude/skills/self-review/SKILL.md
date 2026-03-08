---
name: self-review
description: Review your own uncommitted/unpushed changes for bugs before committing. Use after implementing a feature or fix, before git commit/push.
argument-hint: "[branch-or-range]"
---

# Self-Review Skill

Review your own changes for bugs, correctness issues, and edge cases before committing. Catches issues that CI review bots would flag — but locally, during development.

## Arguments

- `$0` (optional): A git revision range (e.g., `HEAD~3..HEAD`) or branch name to diff against. Default: review uncommitted changes. If working tree is clean, diffs unpushed commits against merge-base with `master`.

## Process

### 1. Gather the diff

```bash
git diff --stat
git diff --cached --stat
```

- Uncommitted/staged changes → review `git diff` + `git diff --cached`
- Clean working tree with unpushed commits → `git diff $(git merge-base HEAD master)..HEAD`
- `$0` provided → `git diff $0`

Read the full diff. For large diffs, read file-by-file.

### 2. Understand context

For each changed file, read enough surrounding context to understand what the function/class does, what callers expect, and what invariants exist. Use `Read` on full files, not just diff hunks.

### 3. Apply review rules

Focus on **bugs and correctness**, not style nits. Do not suggest refactors unrelated to the diff. Limit to high-confidence findings; avoid speculative warnings.

---

#### Rule: LowCardinality type wrapping

If any changed C++ file calls `isNullable()`, `getTypeId()`, or constructs `WhichDataType` on a type that could be `LowCardinality`:
- `isNullable()` returns `false` for `LowCardinality(Nullable(...))` — use `isNullableOrLowCardinalityNullable` instead
- `getTypeId()` on `LowCardinality(X)` returns `TypeIndex::LowCardinality`, not `X`'s type — apply `removeLowCardinality` before type checks
- `WhichDataType(lhs_type)` won't see through `LowCardinality` — use `WhichDataType(removeLowCardinality(lhs_type))`

What NOT to flag: code in `DataTypeLowCardinality.cpp` itself, or contexts where the type is guaranteed to be unwrapped already (e.g., inside `useDefaultImplementationForLowCardinalityColumns` = true functions).

#### Rule: Nullable semantics in function rewrites

If any changed code converts between function forms (e.g., `IN` → `equals`, `NOT IN` → `notEquals`, or similar):
- `IN`/`NOT IN` have `useDefaultImplementationForNulls = false` and return `UInt8` even for Nullable inputs
- `equals`/`notEquals` propagate NULLs and return `Nullable(UInt8)` for Nullable inputs
- The result **type** differs even when the result **value** is the same, which can break downstream operations
- Verify NULL propagation behavior is preserved for all input types

#### Rule: Enum edge cases in comparisons

If any changed code transforms `IN`/`NOT IN` expressions involving Enum types:
- `IN` silently returns 0 for unknown enum values (no error)
- `equals` returns constant false for unknown enum values (safe)
- `notEquals` throws `UNKNOWN_ELEMENT_OF_ENUM` (code 691) for unknown values — **this is a correctness bug**
- Skip or guard any transformation that could turn `NOT IN` into `notEquals` for Enum columns

#### Rule: Type compatibility across function forms

If any changed code assumes two functions accept the same type combinations:
- Not all type pairs that work with `IN` work with `equals` (e.g., `toDate('2024-01-01') IN (1)` works but `toDate('2024-01-01') = 1` throws)
- Check `areTypesComparableForEquality` or equivalent validation when converting between function forms

#### Rule: Settings history

If any changed file modifies `SettingsChangesHistory.cpp`:
- New settings entries must be in the correct version block (check the current development version)
- Default values in history must match actual defaults — `{true, true}` means "old default = true, new default = true"
- Missing or wrong history entries cause upgrade/downgrade issues

#### Rule: Test file conventions

If any changed file is in `tests/queries/0_stateless/`:
- `.reference` files must use `default` for database names, NOT `${CLICKHOUSE_DATABASE}` or random names (the test runner normalizes these)
- Do not add `no-parallel` or other `no-*` tags unless strictly necessary
- Prefer creating a new test file over extending existing ones
- Shell tests (`.sh`) should use `$CLICKHOUSE_CLIENT` not hardcoded paths

#### Rule: C++ style (blocking)

If any changed C++ file uses K&R-style braces (opening brace on same line as control statement):
- Opening brace must be on a new line (Allman style). CI style check will reject this.

What NOT to flag: braces in lambda expressions, initializer lists, or single-line blocks where Allman style is not enforced.

#### Rule: No sleep for synchronization

If any changed C++ file uses `sleep`, `usleep`, `std::this_thread::sleep_for`, or similar to wait for a condition:
- Never use sleep to fix race conditions — use proper synchronization primitives (condition variables, events, atomic operations)
- Sleep is acceptable only in test utilities or intentional delay scenarios (e.g., rate limiting)

#### Rule: General correctness

Apply these checks to all changed code:
- **Off-by-one errors**: Loop bounds, array indices, string slicing, fence-post problems
- **Null/empty checks**: Missing null checks before dereferencing, empty container `.front()`/`.back()`, unchecked `std::optional` access
- **Resource leaks**: Opened files/connections not closed on error paths, missing RAII
- **Integer overflow**: Arithmetic on user-controlled sizes, implicit narrowing conversions (`size_t` → `int`), signed/unsigned comparison
- **Exception safety**: Manual cleanup without RAII, partial state updates before potential throw
- **Concurrency**: Shared mutable state without locks, iterator invalidation during concurrent modification, use-after-move
- **Logic errors**: Inverted conditions, wrong operator (`&&` vs `||`), missing `break`/`[[fallthrough]]` in switch, unreachable code after early return
- **API misuse**: Wrong function for the job, missing unwrapping of wrapper types, incorrect overload resolution
- **Semantic preservation**: When rewriting expressions, verify identical behavior for edge cases (NULLs, empty sets, type mismatches, boundary values, negative numbers, MAX/MIN values)

#### Rule: Test coverage

If the diff adds new logic but no tests, or adds tests that don't cover edge cases:
- Are Nullable inputs tested?
- Are empty/single/many-element cases tested?
- Are boundary values tested?
- Are error paths tested?
- Do NOT flag missing tests for trivial changes (typo fixes, comment updates)

---

### 4. Report findings

For each issue found:

```
[file:line] description

Explanation of why this is a bug and what could go wrong.

Suggested fix: concrete suggestion with code if applicable.
```

Rules for findings:
- Write like a human reviewer, not a bot. No emoji headers, no bold category labels, no formulaic structure.
- Be specific: reference exact file paths, line numbers, variable names.
- Be actionable: suggest fixes, not just problems.
- Wrap ClickHouse identifiers in inline code blocks.
- Say "exception" not "crash" for logical errors. Say "ASan" not "ASAN".
- Prioritize: correctness bugs first, then performance, then minor issues.
- If nothing significant is found, say so briefly. Don't manufacture issues.

### 5. Summary

End with a one-line verdict:
- "No issues found — looks good to commit."
- "Found N issue(s) — fix before committing." (list severity: critical/minor for each)
