---
name: tsearch-port
description: Evaluate and port TSearch commits from the internal TSearchEngine/contrib/ClickHouse repo to upstream ClickHouse. Use when the user says "TSearch 移植", "evaluate TSearch commit", or "port from TSearch".
argument-hint: <commit-hash> [--dry-run]
disable-model-invocation: false
---

# TSearch Port Evaluation Skill

## What is TSearch

TSearch (also called TitanSearch / TSearchEngine) is Tencent's internal search engine built on top of ClickHouse. The ClickHouse fork lives at:

- **Internal repo**: `~/git/TSearchEngine/contrib/ClickHouse`

## What "Porting" Means

"Porting a TSearch commit" means taking a feature or fix originally developed in the TSearch
fork and adapting it for the current upstream ClickHouse `master`. This involves:

1. **Evaluation**: Is this feature useful for upstream? Does upstream already have it (possibly
   implemented differently)? What's the conflict surface?
2. **Adaptation**: The TSearch fork is based on ~23.12 era ClickHouse. Upstream master is now
   at a much later version. APIs, class signatures, file structures have changed significantly.
   A direct cherry-pick almost never works — manual adaptation is always required.
3. **Quality**: Upstream ClickHouse has strict CI, style checks, and review standards. Ported
   code must meet these standards (Allman braces, tests, documentation, etc.).

## Evaluation Criteria

When evaluating whether a TSearch commit is worth porting, assess:

### 1. Upstream Overlap
- Does upstream ClickHouse already have this feature (possibly under a different name)?

### 2. Invasiveness
- How many core files does it touch? (Context.h/cpp, DatabaseCatalog.h/cpp are high-risk)
- Does it change class hierarchies or data structures visible across compilation units?

### 3. Conflict Surface
- How much has the touched code changed between the TSearch fork base (~23.12) and current master?
- Use: `git log --oneline --first-parent master -- <file>` to gauge churn.

### 4. Standalone Value
- Is the feature useful without the rest of the TSearch ecosystem?

### 5. Maintenance Burden
- Will this feature require ongoing maintenance as upstream evolves?

## Evaluation Workflow

```bash
# 1. Read the commit in the TSearch repo
TSEARCH=~/git/TSearchEngine/contrib/ClickHouse
git -C $TSEARCH log -1 --format='%H %ai %an%n%B' <commit>
git -C $TSEARCH diff --stat <commit>^..<commit>
git -C $TSEARCH diff <commit>^..<commit>

# 2. Check if upstream already has something similar
git log --oneline --all --grep="<keyword>" | head -20
grep -rn "<key_identifier>" src/ --include="*.h" | head -10

# 3. Check conflict surface — how much have the touched files changed
for f in $(git -C $TSEARCH diff --name-only <commit>^..<commit>); do
    echo "=== $f ==="
    git log --oneline --first-parent master -- "$f" | head -5
done

# 4. Check titan/* branches for already-ported versions
git log --oneline --all --author="Amos Bird\|amosbird" --grep="<keyword>" | head -10

# 5. Try a test merge (in a throwaway worktree)
# Use the dev skill to create a worktree, then attempt the port
```

## Output Format

When evaluating a commit, produce a structured assessment:

```
## Commit: <hash> — <title>

### Summary
<What the commit does in 2-3 sentences>

### Files Changed
<list with brief description of each change>

### Upstream Overlap
<Does upstream have this? How does it differ?>

### Invasiveness: LOW / MEDIUM / HIGH
<Why>

### Conflict Surface: LOW / MEDIUM / HIGH
<Which files have diverged significantly>

### Standalone Value: LOW / MEDIUM / HIGH
<Is it useful without the rest of TSearch?>

### Recommendation: PORT / SKIP / DEFER / ADAPT
- PORT: Worth porting as-is (with adaptation to current API)
- SKIP: Not worth the effort (upstream has it, too invasive, low value)
- DEFER: Wait for upstream to evolve in this direction first
- ADAPT: The idea is good but needs a fundamentally different implementation for upstream

### Notes
<Any additional context, caveats, or suggestions>
```
