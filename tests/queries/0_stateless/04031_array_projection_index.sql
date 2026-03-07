-- Tags: no-random-settings, no-random-merge-tree-settings
-- { echoOn }

SET enable_analyzer = 1;
SET min_table_rows_to_use_projection_index = 0;

------------------------------------------------------------------------------
-- 1. Map column — basic arrayElement rewrite
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_proj;

CREATE TABLE t_map_proj
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_proj VALUES
    (1, {'env': 'prod', 'region': 'us'}),
    (2, {'env': 'staging', 'region': 'eu'}),
    (3, {'env': 'prod', 'region': 'eu'}),
    (4, {'env': 'dev', 'region': 'asia'}),
    (5, {'region': 'us'});

OPTIMIZE TABLE t_map_proj FINAL;

-- Exact key lookup: labels['env'] = 'prod'
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_proj WHERE labels['env'] = 'prod')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_proj WHERE labels['env'] = 'prod' ORDER BY id;

-- Different key: labels['region'] = 'eu'
SELECT * FROM t_map_proj WHERE labels['region'] = 'eu' ORDER BY id;

-- Key not present in some rows: labels['env'] = 'dev'
SELECT * FROM t_map_proj WHERE labels['env'] = 'dev' ORDER BY id;

-- No match
SELECT * FROM t_map_proj WHERE labels['env'] = 'nonexistent' ORDER BY id;

DROP TABLE t_map_proj;

------------------------------------------------------------------------------
-- 2. Map column — default value safety check
--    labels['missing_key'] = '' should NOT use the projection for reading
--    because arrayElement returns '' (default) for missing keys
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_default;

CREATE TABLE t_map_default
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_default VALUES
    (1, {'a': '1'}),
    (2, {'b': '2'}),
    (3, {});

OPTIMIZE TABLE t_map_default FINAL;

-- This should NOT use projection for reading because labels['x'] = '' is true for rows without key 'x'
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_default WHERE labels['x'] = '')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- This SHOULD use projection (non-default value)
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_default WHERE labels['a'] = '1')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_default WHERE labels['a'] = '1' ORDER BY id;

DROP TABLE t_map_default;

------------------------------------------------------------------------------
-- 3. Array column — has() rewrite
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_arr_proj;

CREATE TABLE t_arr_proj
(
    id UInt64,
    tags Array(String),
    PROJECTION tags_proj INDEX tags TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_arr_proj VALUES
    (1, ['fast', 'reliable']),
    (2, ['slow', 'cheap']),
    (3, ['fast', 'cheap']),
    (4, []),
    (5, ['reliable']);

OPTIMIZE TABLE t_arr_proj FINAL;

-- has(tags, 'fast')
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_arr_proj WHERE has(tags, 'fast'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_arr_proj WHERE has(tags, 'fast') ORDER BY id;

-- has(tags, 'reliable')
SELECT * FROM t_arr_proj WHERE has(tags, 'reliable') ORDER BY id;

-- No match
SELECT * FROM t_arr_proj WHERE has(tags, 'nonexistent') ORDER BY id;

DROP TABLE t_arr_proj;

------------------------------------------------------------------------------
-- 4. mapContains rewrite
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_mapcontains;

CREATE TABLE t_mapcontains
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_mapcontains VALUES
    (1, {'a': '1', 'b': '2'}),
    (2, {'c': '3'}),
    (3, {'a': '4'}),
    (4, {});

OPTIMIZE TABLE t_mapcontains FINAL;

SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_mapcontains WHERE mapContains(labels, 'a'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_mapcontains WHERE mapContains(labels, 'a') ORDER BY id;

-- has() on Map column is semantically equivalent to mapContains()
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_mapcontains WHERE has(labels, 'a'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_mapcontains WHERE has(labels, 'a') ORDER BY id;

DROP TABLE t_mapcontains;

------------------------------------------------------------------------------
-- 5. Multi-part and merge
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_multipart;

CREATE TABLE t_multipart
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

-- Two separate inserts = two parts
INSERT INTO t_multipart VALUES (1, {'env': 'prod'}), (2, {'env': 'staging'});
INSERT INTO t_multipart VALUES (3, {'env': 'prod'}), (4, {'env': 'dev'});

-- Before merge
SELECT * FROM t_multipart WHERE labels['env'] = 'prod' ORDER BY id;

-- After merge
OPTIMIZE TABLE t_multipart FINAL;
SELECT * FROM t_multipart WHERE labels['env'] = 'prod' ORDER BY id;

DROP TABLE t_multipart;

------------------------------------------------------------------------------
-- 6. Error cases
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_bad;

-- Array index on non-Array/Map column
CREATE TABLE t_bad (id UInt64, PROJECTION p INDEX id TYPE array) ENGINE = MergeTree ORDER BY (); -- { serverError INCORRECT_QUERY }

DROP TABLE IF EXISTS t_bad;

------------------------------------------------------------------------------
-- 7. Map with numeric types
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_numeric;

CREATE TABLE t_map_numeric
(
    id UInt64,
    metrics Map(String, Float64),
    PROJECTION metrics_proj INDEX metrics TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_numeric VALUES
    (1, {'cpu': 0.5, 'mem': 0.8}),
    (2, {'cpu': 0.9, 'mem': 0.3}),
    (3, {'cpu': 0.1});

OPTIMIZE TABLE t_map_numeric FINAL;

SELECT * FROM t_map_numeric WHERE metrics['cpu'] > 0.4 ORDER BY id;
SELECT * FROM t_map_numeric WHERE metrics['mem'] = 0.8 ORDER BY id;

DROP TABLE t_map_numeric;

------------------------------------------------------------------------------
-- 8. LIKE/REGEXP on Map values — projection used for key filtering
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_like;

CREATE TABLE t_map_like
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_like VALUES
    (1, {'env': 'production', 'region': 'us-east-1'}),
    (2, {'env': 'staging', 'region': 'eu-west-1'}),
    (3, {'env': 'prod-canary', 'region': 'eu-west-2'}),
    (4, {'env': 'development', 'region': 'ap-south-1'}),
    (5, {'region': 'us-west-2'});

OPTIMIZE TABLE t_map_like FINAL;

-- LIKE pattern on value: labels['env'] LIKE '%prod%'
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_like WHERE labels['env'] LIKE '%prod%')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_like WHERE labels['env'] LIKE '%prod%' ORDER BY id;

-- match() (REGEXP) on value
SELECT * FROM t_map_like WHERE match(labels['env'], '^prod') ORDER BY id;

-- LIKE on region
SELECT * FROM t_map_like WHERE labels['region'] LIKE 'eu%' ORDER BY id;

DROP TABLE t_map_like;

------------------------------------------------------------------------------
-- 9. Arbitrary functions on labels['key'] — all use projection via
--    arrayElement rewrite as long as f(default_value) is false
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_funcs;

CREATE TABLE t_map_funcs
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_funcs VALUES
    (1, {'env': 'production', 'tier': 'gold', 'version': '3'}),
    (2, {'env': 'staging', 'tier': 'silver', 'version': '12'}),
    (3, {'env': 'prod-canary', 'tier': 'gold', 'version': '7'}),
    (4, {'env': 'development', 'tier': 'bronze', 'version': '100'}),
    (5, {'tier': 'gold'});

OPTIMIZE TABLE t_map_funcs FINAL;

-- IN with list (should use projection: '' NOT IN ('production','staging'))
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_funcs WHERE labels['env'] IN ('production', 'staging'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_funcs WHERE labels['env'] IN ('production', 'staging') ORDER BY id;

-- startsWith (should use projection: startsWith('', 'prod') = false)
SELECT * FROM t_map_funcs WHERE startsWith(labels['env'], 'prod') ORDER BY id;

-- endsWith
SELECT * FROM t_map_funcs WHERE endsWith(labels['env'], 'canary') ORDER BY id;

-- length() > N (should use projection: length('') > 8 = false)
SELECT * FROM t_map_funcs WHERE length(labels['env']) > 8 ORDER BY id;

-- toUInt64OrZero on string value (should use projection: toUInt64OrZero('') > 5 = false)
SELECT * FROM t_map_funcs WHERE toUInt64OrZero(labels['version']) > 5 ORDER BY id;

-- notEquals: '' != 'production' = true → evaluateOnDefault returns true → but analyzer
-- rewrites to notEquals which still gets rewritten by projection (correctness ensured
-- by projection expansion containing all key-value pairs)
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_funcs WHERE labels['env'] != 'production')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- NOT mapContains: NOT mapContains({}, 'env') = true → projection still analyzed
-- but selects all marks (no filtering effect)
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_funcs WHERE NOT mapContains(labels, 'env'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

DROP TABLE t_map_funcs;

------------------------------------------------------------------------------
-- 10. Map with numeric values — comparison operators
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_cmp;

CREATE TABLE t_map_cmp
(
    id UInt64,
    metrics Map(String, Float64),
    PROJECTION metrics_proj INDEX metrics TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_cmp VALUES
    (1, {'cpu': 0.5, 'mem': 0.8, 'disk': -1.0}),
    (2, {'cpu': 0.9, 'mem': 0.3, 'disk': 0.5}),
    (3, {'cpu': 0.1, 'disk': 0.2}),
    (4, {'cpu': 0.0, 'mem': 0.0});

OPTIMIZE TABLE t_map_cmp FINAL;

-- Greater than (should use projection: 0 > 0.4 = false)
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_cmp WHERE metrics['cpu'] > 0.4)
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_cmp WHERE metrics['cpu'] > 0.4 ORDER BY id;

-- Greater than or equal (should use projection: 0 >= 0.5 = false)
SELECT * FROM t_map_cmp WHERE metrics['cpu'] >= 0.5 ORDER BY id;

-- Less than: 0 < 0.5 = true → evaluateOnDefault returns true, but analyzer rewrites;
-- projection still analyzed but with broader mark selection
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_cmp WHERE metrics['cpu'] < 0.5)
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- Less than with negative threshold (should use projection: 0 < -0.5 = false)
SELECT * FROM t_map_cmp WHERE metrics['disk'] < -0.5 ORDER BY id;

-- Equals zero: 0 = 0 = true → evaluateOnDefault returns true, but analyzer rewrites;
-- projection still analyzed
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_cmp WHERE metrics['cpu'] = 0)
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- Not equals zero (should use projection: 0 != 0 = false)
SELECT * FROM t_map_cmp WHERE metrics['cpu'] != 0 ORDER BY id;

DROP TABLE t_map_cmp;

------------------------------------------------------------------------------
-- 11. Compound predicates — AND / OR with multiple keys
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_compound;

CREATE TABLE t_map_compound
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_compound VALUES
    (1, {'env': 'prod', 'region': 'us', 'tier': 'gold'}),
    (2, {'env': 'staging', 'region': 'eu', 'tier': 'silver'}),
    (3, {'env': 'prod', 'region': 'eu', 'tier': 'gold'}),
    (4, {'env': 'dev', 'region': 'asia', 'tier': 'bronze'}),
    (5, {'region': 'us', 'tier': 'gold'});

OPTIMIZE TABLE t_map_compound FINAL;

-- AND with two different keys on the same Map — projection cannot be used (different keys conflict)
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_compound WHERE labels['env'] = 'prod' AND labels['region'] = 'eu')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_compound WHERE labels['env'] = 'prod' AND labels['region'] = 'eu' ORDER BY id;

-- AND with the SAME key — projection CAN be used
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_compound WHERE labels['env'] = 'prod' AND labels['env'] != 'staging')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_compound WHERE labels['env'] = 'prod' AND labels['env'] != 'staging' ORDER BY id;

-- OR with two different keys — projection cannot be used
SELECT * FROM t_map_compound WHERE labels['env'] = 'dev' OR labels['tier'] = 'gold' ORDER BY id;

-- AND with mapContains and arrayElement using different keys — projection cannot be used
SELECT * FROM t_map_compound WHERE mapContains(labels, 'env') AND labels['tier'] = 'gold' ORDER BY id;

-- AND with mapContains and arrayElement using the SAME key — projection CAN be used
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_compound WHERE mapContains(labels, 'env') AND labels['env'] = 'prod')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_compound WHERE mapContains(labels, 'env') AND labels['env'] = 'prod' ORDER BY id;

DROP TABLE t_map_compound;

------------------------------------------------------------------------------
-- 12. Array column — has() with AND / NOT
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_arr_compound;

CREATE TABLE t_arr_compound
(
    id UInt64,
    tags Array(String),
    PROJECTION tags_proj INDEX tags TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_arr_compound VALUES
    (1, ['fast', 'reliable', 'new']),
    (2, ['slow', 'cheap']),
    (3, ['fast', 'cheap']),
    (4, []),
    (5, ['reliable', 'new']);

OPTIMIZE TABLE t_arr_compound FINAL;

-- AND: has(tags, 'fast') AND has(tags, 'reliable')
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_arr_compound WHERE has(tags, 'fast') AND has(tags, 'reliable'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_arr_compound WHERE has(tags, 'fast') AND has(tags, 'reliable') ORDER BY id;

-- NOT has: NOT has([], 'fast') = true → projection still analyzed but no filtering
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_arr_compound WHERE NOT has(tags, 'fast'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

DROP TABLE t_arr_compound;

------------------------------------------------------------------------------
-- 13. Map — LIKE/match patterns that match empty string (should NOT use projection)
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_unsafe_pattern;

CREATE TABLE t_map_unsafe_pattern
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_unsafe_pattern VALUES
    (1, {'env': 'prod'}),
    (2, {'env': 'staging'}),
    (3, {});

OPTIMIZE TABLE t_map_unsafe_pattern FINAL;

-- LIKE '%' matches empty string — should NOT use projection
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_unsafe_pattern WHERE labels['env'] LIKE '%')
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- match('', '.*') = true — should NOT use projection
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_unsafe_pattern WHERE match(labels['env'], '.*'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');

-- match with non-matching pattern — should use projection
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_unsafe_pattern WHERE match(labels['env'], '^prod'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_unsafe_pattern WHERE match(labels['env'], '^prod') ORDER BY id;

DROP TABLE t_map_unsafe_pattern;

------------------------------------------------------------------------------
-- 14. IN with empty-string element (should NOT use projection)
------------------------------------------------------------------------------

DROP TABLE IF EXISTS t_map_in_default;

CREATE TABLE t_map_in_default
(
    id UInt64,
    labels Map(String, String),
    PROJECTION labels_proj INDEX labels TYPE array
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    index_granularity = 1, min_bytes_for_wide_part = 0,
    min_bytes_for_full_part_storage = 0, enable_vertical_merge_algorithm = 0;

INSERT INTO t_map_in_default VALUES
    (1, {'a': 'x'}),
    (2, {'b': 'y'}),
    (3, {});

OPTIMIZE TABLE t_map_in_default FINAL;

-- IN ('x', '') — '' is in the list, so labels['key'] = '' matches missing keys.
-- evaluateOnDefault should detect this and skip the projection.
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_in_default WHERE labels['a'] IN ('x', ''))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_in_default WHERE labels['a'] IN ('x', '') ORDER BY id;

-- IN ('x', 'y') — no default value → should use projection
SELECT trimLeft(explain)
FROM (EXPLAIN projections = 1 SELECT * FROM t_map_in_default WHERE labels['a'] IN ('x', 'y'))
WHERE explain LIKE '%ReadFromMergeTree%' OR match(explain, '^\s+[A-Z][a-z]+(\s+[A-Z][a-z]+)*:');
SELECT * FROM t_map_in_default WHERE labels['a'] IN ('x', 'y') ORDER BY id;

DROP TABLE t_map_in_default;
