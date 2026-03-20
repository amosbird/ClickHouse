---
name: self-review
description: Review your own uncommitted/unpushed changes for bugs before committing. Use after implementing a feature or fix, before git commit/push.
argument-hint: "[branch-or-range]"
---

# Self-Review Skill

Review your own changes for bugs, correctness issues, and edge cases before committing. Catches issues that CI review bots would flag — but locally, during development.

Ported from ClickHouse's `.github/copilot-instructions.md` (the prompt behind the `clickhouse-gh` bot AI review) and extended with additional ClickHouse-specific rules.

## Arguments

- `$0` (optional): A git revision range (e.g., `HEAD~3..HEAD`) or branch name to diff against. Default: review uncommitted changes. If working tree is clean, diffs unpushed commits against merge-base with `master`.

## Precision over recall

**False positives are worse than missed nits.** Prefer high precision: if you are not reasonably confident that something is a real problem or a serious risk, do **not** flag it. When in doubt between "possible minor issue" and "no issue" — choose **no issue**.

## Process

### 1. Gather the diff

```bash
git diff --stat
git diff --cached --stat
```

- Uncommitted/staged changes → review `git diff` + `git diff --cached`
- Clean working tree with unpushed commits → use the **reflog-based** method below
- `$0` provided → `git diff $0`

Read the full diff. For large diffs, read file-by-file.

#### Reflog-based diff (for branches that merged upstream)

**NEVER use `git diff master` or `git merge-base HEAD master`** on branches that have merged upstream — the merge-base moves forward and the diff includes massive upstream noise in both directions.

Instead, use the branch's reflog to find its creation point:

```bash
created_at=$(git reflog show <branch> --format='%H' | tail -1)
base=$(git rev-parse "${created_at}^")
git diff ${base}..HEAD -- <paths-we-changed>
```

The last reflog entry is the branch creation commit. `${created_at}^` is the parent, i.e., the point the branch diverged from. This gives a clean diff of only our changes, excluding all upstream merges.

Scope the `-- <paths>` to directories/files we actually modified (e.g., `src/`, `tests/integration/test_foo/`, `docs/`) to further filter out any incidental upstream file touches.

### 2. Understand context

For each changed file, read enough surrounding context to understand what the function/class does, what callers expect, and what invariants exist. Use `Read` on full files, not just diff hunks.

### 3. Apply review rules

Focus on **bugs and correctness**, not style nits. Do not suggest refactors unrelated to the diff. Limit to high-confidence findings; avoid speculative warnings.

#### Primary goals (in priority order)

1. **Correctness & safety** — logic errors, data corruption, missing checks, undefined behavior
2. **Resource management** — memory leaks, FD leaks, lifetime issues, double frees, ownership confusion
3. **Concurrency & robustness** — data races, deadlocks, ABA, misuse of atomics/locks, unsafe shared state
4. **Performance** — hot-path regressions, pathological complexity, unbounded allocations, unnecessary disk/network roundtrips
5. **Maintainability** — over-engineering, duplicated logic, fragile patterns
6. **User-facing quality** — wrong/misleading messages, missing observability for serious failure modes
7. **ClickHouse-specific compliance** — see checklist below

---

#### Rule: Memory & lifetime

- Raw pointers where ownership is unclear or inconsistent with surrounding code
- Missing `delete` / `free` / `unmap` / `close` on early returns or exceptions
- Containers or views returning references/iterators to temporary or moved-from objects
- Use of `std::string_view`, spans, or references to buffers whose lifetime is not guaranteed
- Manual `new`/`delete` instead of RAII where the surrounding code uses RAII types

#### Rule: Resource management

- Opened file descriptors or sockets not closed on all paths (including error paths)
- Leaks in loops where allocation happens inside the loop but deallocation depends on conditions
- Misuse of `std::unique_ptr` / `std::shared_ptr` / intrusive refcounts: cycles, double ownership, or forgotten release

#### Rule: Concurrency & threading

- Access to shared state without appropriate locking/atomics
- Lock ordering changes that could introduce ABBA deadlocks
- Using non-thread-safe data structures from multiple threads
- Mutable globals or singletons accessed from many places

#### Rule: Error handling & observability

- Ignored return values of functions that can fail (IO, network, syscalls)
- Exceptions that cross module boundaries in unexpected ways
- Inconsistent error codes or messages that make debugging impossible
- Missing logs for serious failure modes (data loss risk, query aborts, background task failures)

#### Rule: Data correctness & serialization

- Changes to on-disk or wire formats without explicit versioning, clear upgrade/downgrade behavior, or compatibility tests
- Schema or metadata evolution without migration logic or feature flags
- Silent truncation, overflow, or lossy conversions

#### Rule: Performance & algorithmic behavior

- New allocations or copies in tight loops
- Unbounded structures (maps, vectors) that can grow without limits in long-running processes
- Accidental O(N^2) patterns on large inputs
- Extra syscalls, unnecessary fsyncs, sleeps, or polling in hot paths
- Re-parsing config strings or re-computing derived data on every call instead of caching the result

#### Rule: ClickHouse config tree isolation

ClickHouse has separate config trees loaded independently:
- **Server config** (`config.xml`, `config.d/`): server settings, macros, clusters, storage policies
- **Users config** (`users.xml`, `users.d/`): user definitions, profiles, quotas, roles

Code that receives a `Poco::Util::AbstractConfiguration &` parameter must only read keys that exist in the specific config tree it receives. Common mistakes:
- Reading a server setting (e.g., `database_namespace_separator`, `max_server_memory_usage`) from the users config tree — the key doesn't exist there and silently returns the default
- Reading user-level keys from the server config tree
- Assuming a merged/global config when the caller passes a specific subtree

When validating cross-cutting concerns (e.g., a user property that depends on a server setting), either pass the effective value as a parameter or defer validation to a layer that has access to both configs (e.g., `Context`).

#### Rule: Compilation time & build impact

- Adding non-trivial code (function bodies, method implementations, template definitions) to widely-included headers instead of moving it to `.cpp` files
- Adding or pulling heavy transitive includes into high-fan-out headers (e.g., `Exception.h`, `IColumn.h`, `IDataType.h`)
- Unnecessary template instantiations — use `if constexpr` to prune variants that do not apply
- Large `constexpr` evaluation in headers that the compiler must evaluate in every translation unit

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

### 4. ClickHouse compliance checklist

For non-trivial changes, verify each item (Yes/No/N/A + short note):

- **Data deletions logged?** All data deletion events (files, parts, metadata, ZK entries) must be logged.
- **Serialization formats versioned?** Any format change (columns, aggregates, protocol, settings, replication metadata) must be versioned with upgrade/downgrade resilience.
- **Experimental setting gate present?** New features/behaviors must be gated behind an experimental setting until proven safe.
- **Settings exposed for constants/thresholds?** Avoid magic constants; represent important thresholds as settings with sensible defaults.
- **Backward compatibility preserved?** New versions must be configurable to behave like older versions via `compatibility` settings.
- **`SettingsHistory.cpp` updated?** Required when settings change defaults or are added.
- **Existing tests untouched (only additions)?** Do not delete or relax existing tests.
- **Docs/user-facing notes updated?** If behavior visible to users changed.
- **Core-area change got extra scrutiny?** Query execution, storage engines, replication, Keeper, system tables, MergeTree internals.

### 5. Report findings

#### Severity model

**Blockers** — must fix before merge:
- Incorrectness, data loss, or corruption
- Memory/resource leaks or UB (use-after-free, double free, invalid pointer arithmetic)
- New races, deadlocks, or serious concurrency issues
- Missing serialization versioning/compat for format changes
- Deletion events not logged
- New feature without an experimental gate
- Significant performance regression in a hot path
- Security or privilege issues

**Majors** — serious but not catastrophic:
- Under-tested important edge cases or error paths
- Fragile code that is likely to break under realistic usage
- Hidden magic constants that should be settings
- Confusing or incomplete user-visible behavior/docs
- Compilation time regressions (non-trivial code in widely-included headers)

**Nits** — only mention if they materially improve robustness or clarity:
- Minor refactors that clearly reduce future bug risk
- Small documentation improvements that avoid user confusion

**Do not report as nits**: typos, minor naming preferences, comment wording, pure formatting.

#### Format

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

### 6. Summary

End with a one-line verdict:
- "No issues found — looks good to commit."
- "Found N issue(s) — fix before committing." (list severity: blocker/major/nit for each)
