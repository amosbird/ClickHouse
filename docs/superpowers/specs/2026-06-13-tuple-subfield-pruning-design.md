# Tuple subfield pruning at INSERT and merge

**Date:** 2026-06-13
**Status:** Design
**Author:** Amos Bird
**Depends on:** PR #107305 (named Tuple metadata-only ALTER) — must be merged first

## Problem

In a production game-analytics workload, the customer maintains one `MergeTree` table partitioned by `action_type`. The `data` column is a deeply nested named `Tuple` covering **every event field across every business**. Each partition only ever populates a small subset of the fields — e.g. `action_type=1` rows fill `data.name, data.level, data.server_id`, `action_type=2` rows fill `data.c2s.statistics.gold`, and so on. The remaining hundreds of subfields are written as type defaults.

Today, every subfield is written to disk in every part:

- A login event part contains stream files for `data.battle_time`, `data.fight_power`, all of `data.c2s.*`, all of `data.hero_stats.*`, even though every row is NULL.
- After PR #107305, this gets worse: schema evolution adds new subfields (instantly, metadata-only), but every new part includes a full set of empty stream files for unused fields. A customer with 100 partitions × 280 subfields × thousands of parts ends up with millions of all-default stream files.
- Read time: even though each empty stream is tiny, the per-file open/seek/decompress overhead dominates queries that touch the whole `Tuple` column (`SELECT data FROM x`). We measured this in PR #107305: on 2M rows with 100 added empty subfields, a `SELECT cityHash64(data)` query slowed from 323 ms (no empty streams) to 745 ms (100 empty streams materialized).

## Goal

When writing a part — at INSERT or merge — automatically detect named-`Tuple` subfields whose values are entirely type-defaults in that part, and **omit their stream files** from disk. The part records a **narrowed** Tuple type in its `columns.txt`; reads see the narrowed type and use `CAST(narrowed → full)` (already supported and validated in PR #107305) to materialize defaults on the fly.

This is purely a per-part space and read-IO optimization. Table-level schema is unchanged.

## Non-goals

- Not extending `IDataType` / `IColumn` interfaces beyond what is needed to detect all-default values. `IColumn::hasOnlyTypeDefaults()` (and its `ColumnTuple` / `ColumnArray` / ... overrides) is the only new column-side API. Other concepts from PR #98472 (`skip_empty_columns_on_insert`, `WITH_SKIPPED_COLUMNS` serialization-info version, `skipped_columns` JSON field, `DEFAULT`-expression interaction) are **out of scope**.
- Not changing how queries observe the table. Schema-level `data Tuple(...)` is unchanged; `SELECT data.unused_field` continues to return defaults; `system.parts_columns` continues to show the table-level type after PR #107305's CAST handling.
- Not introducing on-wire or serialization-format versioning. Parts written under this PR are readable by any server that has PR #107305.

## Why this is safe (background — settled by PR #107305)

PR #107305 proved three properties used here:

1. Each subfield of a named `Tuple` is stored as a separate stream named by the field path (e.g. `data.c2s.statistics.gold.bin`), not by position. Omitting a subfield's stream files at write time is structurally identical to never having written it (the `ALTER` case in PR #107305).
2. Reading a part whose `columns.txt` lists a narrower `Tuple` type than the table-level type triggers `performRequiredConversions` → `_CAST(narrowed_tuple, full_tuple)`, which matches by field name, takes existing fields via identity wrapper (zero copy), and constructs the missing fields via `IDataType::createColumnConstWithDefaultValue` once per block. **Subcolumn reads** (`SELECT data.x`) bypass CAST entirely and go through `fillMissingColumns` when the part's narrowed type lacks the subcolumn.
3. Subsequent merges naturally re-materialize the full type if the merged data contains non-default values, because the merge writer re-evaluates per-part type narrowing on the merged data.

This design relies on all three. **PR #107305 must be merged before this PR.**

## Architecture

### Observation: master already has the infrastructure

`MergedBlockOutputStream::finalizePart()` already supports **removing stream files for whole columns** that are "expired" — see `expired_columns` and `IMergedBlockOutputStream::removeEmptyColumnsFromPart`:

```cpp
// IMergedBlockOutputStream.cpp:46
NameSet IMergedBlockOutputStream::removeEmptyColumnsFromPart(...)
{
    if (empty_columns.empty() || isCompactPart(data_part))
        return {};
    // For each empty column, enumerate its substreams, decrement shared stream
    // counts, mark .bin and .mrk files for deletion if no other column needs them,
    // erase from serialization_infos and checksums.
}
```

This runs after the writer has written everything, in finalize. The caller decides which columns are "empty" via two existing mechanisms:

1. **TTL processing** populates `expired_columns` when a TTL expression makes a whole column expire.
2. **Horizontal merge detection** in `MergeTask::prepare` adds storage columns that are missing from every source part and have no `DEFAULT` expression to `expired_columns`.

The data flow is:

```
Writer finishes → finalizePart() {
    auto part_columns = columns_list;
    removeEmptyColumnsFromPart(new_part, part_columns,
                                expired_columns, serialization_infos, checksums);
    // part_columns has the expired columns removed
    new_part->setColumns(part_columns, ...);  // columns.txt no longer mentions them
    // checksums has the expired files removed; physical files removed below
}
```

**This is the exact infrastructure we extend.**

### Extending `removeEmptyColumnsFromPart` to subfield granularity

Instead of operating on a `NameSet` of top-level columns, the extended path operates on a `NameSet` of **storage-level dotted substream names** (e.g. `data.b`, `data.c2s.statistics.gold`). It:

- Enumerates all substreams for the column's serialization.
- For each substream whose dotted name is in the expired set: decrement shared-stream counts, mark `.bin`/`.mrk` files for deletion.
- **New step**: compute the narrowed `DataTypePtr` for the column (the original Tuple type with the expired leaf subfields removed, recursively through `Array`/`Nullable`/`Map`/`Tuple` wrappers) and update the entry in `part_columns`.
- Erase or update the entry in `serialization_infos` accordingly.

`columns_substreams.txt` is regenerated from the (now narrowed) serialization in `finalizePartOnDisk`, so it naturally omits the removed substreams. `checksums.txt` and on-disk file removal are unchanged.

This same function serves the existing top-level case: a fully-expired column corresponds to "every leaf in the column is in the expired set" → the recursive `narrowDataType` returns an empty Tuple → the column is removed from `part_columns` exactly as today.

### Detecting expired subfields

Two paths feed the expired-subfield set, mirroring the existing two paths for expired columns:

#### INSERT path

`MergeTreeDataWriter::writeTempPartImpl` already has the full block before constructing the writer. It walks each Tuple-shaped storage column:

```cpp
NameSet expired_substreams;
for (const auto & [name, type] : columns)
{
    if (!containsNamedTuple(type))
        continue;
    const auto & col = block.getByName(name).column;
    collectAllDefaultLeafSubstreams(name, type, *col, expired_substreams);
}
new_data_part->expired_columns = std::move(expired_substreams);
// pass to writer/finalize as before
```

`collectAllDefaultLeafSubstreams` is a recursive walker:

- For a `ColumnTuple`: for each element index `i`, call `getColumn(i).hasOnlyTypeDefaults()`. If true, every leaf below this element is expired; add their dotted storage names to the set. If false, recurse into nested `Tuple`/`Array`/`Map`/`Nullable` wrappers.
- For `ColumnNullable(Tuple)`: if `null_map_data` is all 1, the entire Tuple value is NULL — all leaves below are expired. Otherwise recurse into the nested Tuple using the nested column.
- For `ColumnArray(Tuple)` / `ColumnMap(K, Tuple)`: if the array element count is 0 everywhere (empty arrays / empty maps), all leaves below are expired. Otherwise recurse using the inner Tuple column.

`hasOnlyTypeDefaults()` is added as a virtual on `IColumn`, with overrides borrowed from PR #98472 (these are the only column-API additions; the rest of #98472 is not used):

| Column type | `hasOnlyTypeDefaults()` |
|---|---|
| `ColumnVector<T>`, `ColumnDecimal<T>` | `memoryIsZero` over data |
| `ColumnString` | all offsets equal 0 |
| `ColumnFixedString` | data is all zero |
| `ColumnNullable` | null_map is all 1 (every row is NULL) OR (null_map all 0 AND nested has only defaults) |
| `ColumnArray` | offsets all 0 (every row is `[]`) OR (offsets all 0/non-zero per row AND nested has only defaults) — for our purpose we use the simpler check: all rows empty |
| `ColumnMap` | underlying `ColumnArray(Tuple(K, V))` empty per row |
| `ColumnTuple` | every element column has only defaults |
| `ColumnLowCardinality` | dictionary is `[default_value]` and indexes all 0 |

For composite columns we use the simplest definition that is safe under `CAST(narrow → full)`: a leaf is "type-default" iff its serialization produces no information that the reader cannot reconstruct via `IDataType::createColumnConstWithDefaultValue`. For Array/Map this means **every row is the empty container** — if any row has a non-empty array, we cannot narrow because the per-row sizes would be lost.

#### Merge path

Two sub-cases:

**Sub-case A: monotonic preservation from source parts**

The source parts may themselves have narrowed types from prior writes. If a leaf is absent from every source part (after CAST is logically applied — i.e. would default-fill), the merged part also cannot have non-default data for that leaf, so it can stay narrowed.

`MergeTask::prepare` already does this for whole columns (`columns_present_in_parts`). We extend it to substreams:

```cpp
// Compute the per-substream "present in any source part" set.
NameSet substreams_present_in_any_part;
for (const auto & source_part : source_parts)
{
    const auto & source_columns = source_part->getColumns();
    for (const auto & col : source_columns)
    {
        serialization_for(col)->enumerateStreams([&](const auto & path) {
            substreams_present_in_any_part.insert(dottedName(col, path));
        });
    }
}

// For each table-level Tuple column, enumerate its full substreams and mark
// those that no source part has as expired.
for (const auto & [name, full_type] : storage_columns)
{
    if (!containsNamedTuple(full_type))
        continue;
    enumerateAllSubstreams(name, full_type, [&](const String & dotted) {
        if (!substreams_present_in_any_part.contains(dotted))
            expired_substreams.insert(dotted);
    });
}
```

**Sub-case B: writer-side re-detection from the merged data**

Even if all source parts have a leaf present, the merged data may still be all defaults (e.g. a TTL `WHERE` filter, an `ALTER UPDATE` that nulled rows, or merge of parts where the leaf coincidentally contains only defaults). The INSERT-path mechanism applies here too because `MergeTreeDataPartWriterOnDisk` uses `block_sample` plus per-block writing — but `block_sample` is `cloneEmpty()` and does not preserve data.

The writer-side detection therefore runs as a **post-write check** in `finalizePart`:

- During `writeColumn`, the writer already accumulates statistics per stream (compressed bytes written, etc.). We add a per-stream `is_all_zero` flag tracked by the granule-level encoder: initialized `true`, set to `false` the first time any non-zero byte is written.
- At finalize, before `removeEmptyColumnsFromPart`, for each Tuple subfield whose all writes were all-zero AND whose `IDataType::insertDefaultInto` produces a value equal to the type's zero bytes (the same correctness check #98472 uses for column-level skip), add it to `expired_substreams`.

This second sub-case is **optional** for the first iteration of the PR — Sub-case A alone is sufficient to preserve narrowing across merges, and writer-side per-stream tracking adds non-trivial infrastructure. We can land sub-case A first and add sub-case B in a follow-up.

For the customer's scenario (PARTITION BY `action_type`, each partition uses a distinct schema subset, source parts are already narrowed), **sub-case A is enough**: the partition's first INSERT establishes the narrowed schema, all subsequent merges within that partition preserve it.

### Setting

A single MergeTree setting controls the entire feature:

```
enable_tuple_subfield_pruning (Bool, default true)
```

When `false`, neither the INSERT nor the merge path adds Tuple subfields to `expired_substreams`. Existing narrowed parts continue to read correctly because the read path (CAST) is unconditional.

### Compact parts

`removeEmptyColumnsFromPart` already returns early when the part is Compact (`if (empty_columns.empty() || isCompactPart(data_part)) return {};`). We keep this behavior — Compact parts share `data.bin` across all columns, so post-write substream removal would require rewriting the file. The customer's table will hit the Wide threshold quickly given the size of `data`; the optimization is most valuable for Wide parts anyway.

This matches the existing convention for the top-level `expired_columns` path and avoids any new edge cases for Compact part handling.

## Detailed component changes

### 1. `src/Columns/IColumn.h` + overrides

Add `virtual bool hasOnlyTypeDefaults() const = 0;` to `IColumn`. Implement overrides:

- `ColumnVector<T>::hasOnlyTypeDefaults`
- `ColumnDecimal<T>::hasOnlyTypeDefaults`
- `ColumnString::hasOnlyTypeDefaults`
- `ColumnFixedString::hasOnlyTypeDefaults`
- `ColumnNullable::hasOnlyTypeDefaults`
- `ColumnArray::hasOnlyTypeDefaults` (size-based: all rows empty)
- `ColumnMap::hasOnlyTypeDefaults`
- `ColumnTuple::hasOnlyTypeDefaults` (element-wise AND)
- `ColumnLowCardinality::hasOnlyTypeDefaults`
- `ColumnConst::hasOnlyTypeDefaults` (delegate to data column)
- Sparse: native-zero
- `ColumnDynamic` / `ColumnObject`: return `false` (out of scope; would need #98472-style reasoning)

These are intentionally the same shape as PR #98472. The implementations themselves are small and reusable.

### 2. `src/Storages/MergeTree/MergeTreeDataWriter.cpp`

Add `collectExpiredTupleSubstreams(columns, block)` helper. Call it in `writeTempPartImpl` right before constructing the writer:

```cpp
auto data_settings = data.getSettings();
if ((*data_settings)[MergeTreeSetting::enable_tuple_subfield_pruning])
{
    auto expired = collectExpiredTupleSubstreams(columns, block, ...);
    new_data_part->expired_columns.merge(std::move(expired));
}
```

`collectExpiredTupleSubstreams` walks each Tuple-shaped column in `columns`, uses `hasOnlyTypeDefaults` on each leaf via the column-aware recursion described in §"Detecting expired subfields → INSERT path".

### 3. `src/Storages/MergeTree/MergeTask.cpp`

Extend the existing `expired_columns` population in `prepare` (around line 643) to substream granularity:

```cpp
NameSet substreams_present_in_any_part;
for (const auto & source_part : source_parts)
    enumerateSubstreamsOfPart(*source_part, substreams_present_in_any_part);

NameSet expired_substreams;
for (const auto & storage_column : global_ctx->storage_columns)
{
    if (containsNamedTuple(storage_column.type))
        addAbsentSubstreamsAsExpired(storage_column, substreams_present_in_any_part, expired_substreams);
}

// Also keep the existing whole-column logic
for (const auto & storage_column : global_ctx->storage_columns)
{
    if (!columns_present_in_parts.contains(storage_column.name) &&
        !columns_desc.getDefault(storage_column.name))
        global_ctx->new_data_part->expired_columns.emplace(storage_column.name);
}

global_ctx->new_data_part->expired_columns.merge(std::move(expired_substreams));
```

### 4. `src/Storages/MergeTree/IMergedBlockOutputStream.cpp`

`removeEmptyColumnsFromPart` is extended to:

- Recognize that an "expired" entry can be a dotted substream name, not just a top-level column name.
- For each top-level column with at least one expired substream, compute the narrowed `DataTypePtr` (recursive Tuple narrowing) and update the entry in `columns` (the `NamesAndTypesList &` output parameter).
- Enumerate the original (un-narrowed) serialization streams to find which `.bin`/`.mrk` files are no longer needed; account for shared streams as before.
- Erase or update the entry in `serialization_infos`.

A new helper `narrowDataType(full_type, expired_substream_set, column_name)` is the recursive type rewriter:

- Tuple: for each element, prefix the recursion with `column_name + "." + element_name`; if the recursive narrowing reduces an element to "fully expired" (empty Tuple at the leaf), drop the element. If all elements are dropped, the column itself is fully expired (re-use existing whole-column path).
- Array: narrow the inner type recursively.
- Nullable / Map(K, ): narrow the value type.
- Other types: return as-is.

### 5. New setting

`src/Storages/MergeTree/MergeTreeSettings.cpp`:

```cpp
DECLARE(Bool, enable_tuple_subfield_pruning, true,
    "When writing or merging a part, omit stream files for named-Tuple subfields "
    "whose values are entirely type-defaults in that part. The table-level schema "
    "is unchanged; reads use `CAST(narrowed_tuple → full_tuple)` to materialize "
    "defaults. Only applies to Wide parts.",
    0)
```

Add an entry in `SettingsChangesHistory.cpp` for the current development version.

### 6. Tests

`tests/queries/0_stateless/0NNNN_tuple_subfield_pruning.sql`:

1. INSERT a Wide part where some Tuple subfields are entirely default. Check `system.parts.bytes_on_disk` is meaningfully smaller. Check `SELECT *` returns the same as the full schema.
2. INSERT and check stream files: list the part directory and verify the all-default subfield's `.bin` is absent.
3. INSERT with all rows truly using all subfields — no narrowing should happen.
4. INSERT into a `PARTITION BY action_type` table simulating the customer scenario: each partition uses a different subset. Verify each part has only the relevant streams.
5. Merge: insert two parts where one has narrowed `data.x` and the other has full `data.x` (both all-default for the column under scope) → merged part remains narrowed.
6. Merge: one part has `data.x` populated, the other narrowed → merged part has full `data.x`.
7. Nested Tuple: narrowing inside `data.c2s.statistics`.
8. `Array(Tuple)`: empty arrays everywhere → narrowed; non-empty arrays → preserved.
9. `Nullable(Tuple)`: all-NULL → narrowed.
10. `Map(K, Tuple)`: empty maps → narrowed.
11. `enable_tuple_subfield_pruning = 0` → no narrowing, full streams written.
12. Compact part: setting on but part is Compact → no narrowing (matches existing top-level convention).
13. Read entire Tuple after narrowing: result equals what the full schema would have produced.
14. Subcolumn read of a narrowed-away leaf: returns the type default.
15. After narrowing, an ALTER MODIFY COLUMN that adds a new subfield → still metadata-only (PR #107305 logic intact).
16. After narrowing, an ALTER MODIFY COLUMN that requires mutation → mutation works on the narrowed part (CAST handles the type difference).
17. Customer schema reproducer (similar to PR #107305 case 15) but using `enable_tuple_subfield_pruning` to demonstrate space savings.

### 7. Documentation

A brief note in `docs/en/operations/settings/merge-tree-settings.md` for the new setting.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| `hasOnlyTypeDefaults` implementation bugs for `ColumnNullable` / `ColumnArray` / `ColumnLowCardinality` produce false positives → data corruption | Unit tests at the column level (similar to PR #98472's existing tests); the "type default round-trip" sanity check (also from #98472) catches the Enum-with-first-value-not-zero class of issues |
| `narrowDataType` produces an empty Tuple (after dropping all elements) but caller expects a valid Tuple type | Empty Tuple is allowed in ClickHouse (`Tuple()`), but for our purpose we should treat it as "drop the column entirely" — i.e. promote to whole-column expiration |
| Reorder of `Tuple` elements during narrowing | Always preserve original order from the full type when narrowing; never reorder |
| Merge picks up narrowed source parts and creates a new narrowed part, but a later merge with a non-narrowed source needs full type → CAST(narrow → full) handles this transparently (PR #107305) | Already covered by PR #107305 — no new logic needed |
| `system.parts_columns` shows the narrowed type, confusing users | `system.parts_columns` already shows the per-part type. Users see the table schema via `DESCRIBE` or `system.columns`. We add an explanatory comment in the column description. (Alternatively, render the table-level type — but that masks the optimization.) |
| Replicated tables: replicas may write parts with different narrowings | Each replica's writer applies the same logic to the same data, so narrowings will match in steady state. If a replica is upgraded and another is not, the upgraded one writes narrowed parts while the older one writes full parts — both are readable by both replicas (no on-disk format change, just fewer files). |
| Compact part conversion: a Wide part is converted to Compact (via merge), narrow type is lost | Compact path returns early from `removeEmptyColumnsFromPart`; the resulting Compact part has the full type. This is acceptable degradation, identical to the existing top-level `expired_columns` behavior. |

## Implementation order

1. **Land PR #107305** (named Tuple metadata-only ALTER) — done, in review.
2. Add `IColumn::hasOnlyTypeDefaults` and overrides (lift from #98472).
3. Add `enable_tuple_subfield_pruning` setting + history entry.
4. Extend `removeEmptyColumnsFromPart` and add `narrowDataType` helper.
5. Wire INSERT path (`MergeTreeDataWriter::writeTempPartImpl`).
6. Wire merge path Sub-case A (`MergeTask::prepare`).
7. Add stateless tests for all cases above.
8. Self-review, commit, PR.

Sub-case B (writer-side per-stream re-detection) is deferred. If merging full-schema parts whose merged data is all-default for some leaf becomes important, add it as a follow-up: it requires per-stream `is_all_zero` flags in `MergeTreeWriterStream` plus the `insertDefaultInto`-equality check.

## References

- PR #107305: metadata-only ALTER for named Tuple subfield additions (must merge first)
- PR #98472: `skip_empty_columns_on_insert` (we lift `IColumn::hasOnlyTypeDefaults` and its overrides from here, but no other code)
- Dynamic v4 narrowing in `remotes/amos/json-per-part-narrowing` (per-part schema narrowing precedent; different mechanism but same spirit)
- `src/Storages/MergeTree/IMergedBlockOutputStream.cpp::removeEmptyColumnsFromPart` (existing infrastructure being extended)
- `src/Storages/MergeTree/MergeTask.cpp` lines 643–650 (existing `expired_columns` population pattern)
