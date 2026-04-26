# Design: Comprehensive Review of `projection-text-index-squash`

## Problem and goal

We need a correctness-focused, comprehensive review of the `projection-text-index-squash` worktree that covers both:

- branch-owned changes (relative to upstream base), and
- current uncommitted working tree changes.

The output must be a prioritized issue list with file/line references, clear impact, and concrete fix suggestions.

## Scope

In scope:

- Review of code deltas in the target worktree branch diff and uncommitted changes.
- Prioritization by correctness risk (primary), then confidence and impact.
- Explicit reporting of blocked/uncertain coverage areas.

Out of scope:

- Pure style-only comments unless they materially affect correctness.
- Unrelated repository-wide refactoring outside touched or tightly coupled areas.

## Selected approach

Chosen approach: **two-stage review pipeline**.

1. Stage A gathers high-signal evidence (`git` base/diff context, changed-file map, and risk hotspots).
2. Stage B performs deep correctness analysis and synthesizes one consolidated, prioritized report.

Rationale: best balance between depth and coverage while reducing missed cross-file logic issues.

## Architecture

The review process is a linear pipeline with explicit handoffs:

1. Scope resolution
2. Evidence collection
3. Correctness analysis
4. Finding deduplication and ranking
5. Report synthesis

Each stage emits structured outputs consumed by the next stage; failures are surfaced, not hidden.

## Components

### 1) Scope resolver

- Determines branch creation base from branch reflog (creation point) for accurate branch-owned diff.
- Includes uncommitted deltas (`staged` + `unstaged`) in analysis scope.

### 2) Evidence collector

- Builds changed-file inventory.
- Identifies hotspots (cross-module edits, invariants touched, parser/planner/optimizer boundary changes, and test deltas).

### 3) Correctness analyzer

- Reviews control/data flow changes and invariant preservation.
- Looks for exception-prone edge cases and semantic regressions.
- Verifies behavior consistency across related call paths.

### 4) Report synthesizer

- Merges duplicate findings from multiple analysis passes.
- Produces prioritized findings with severity, confidence, file/line references, and fix guidance.

## Data flow

1. Resolve base commit.
2. Collect branch diff and working tree delta.
3. Partition changes by subsystem/risk.
4. Analyze each partition for correctness issues.
5. Merge and deduplicate findings.
6. Rank by severity and confidence.
7. Emit final review report.

## Error handling and uncertainty policy

- If any stage cannot complete (missing refs, parse ambiguity, incomplete context), report the blocked scope explicitly with cause.
- Never silently skip unknown areas.
- Attach confidence notes where certainty is limited.

## Output contract

Final review output must include:

1. Prioritized issues (highest correctness risk first).
2. Precise references (`file:line`).
3. Why each issue matters (impact/risk).
4. Severity label (`critical` / `high` / `medium` / `low`) and confidence level.
5. Concrete remediation suggestions.
6. Coverage note listing any unreviewed or low-confidence areas.

## Validation and success criteria

The design is successful if execution produces a review that is:

- comprehensive for in-scope deltas,
- correctness-first,
- actionable (fix-ready guidance),
- explicit about uncertainty and coverage limits.

## Notes

This spec intentionally optimizes for correctness and potential exceptions, per user preference.
