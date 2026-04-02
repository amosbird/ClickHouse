-- Tests that projection text indexes built on mapValues(m) work with the IN operator.
-- Adapted from 04035_text_index_map_values_in.sql

SET enable_analyzer = 1;
SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt32,
    key String,
    map Map(String, String),
    PROJECTION idx_key INDEX key TYPE text(tokenizer = 'splitByNonAlpha'),
    PROJECTION map_values_idx INDEX mapValues(map) TYPE text(tokenizer = 'array')
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 1, index_granularity_bytes = '10M', min_bytes_for_wide_part = 0;

INSERT INTO tab VALUES (0, 'a', {'service':'web-api'}), (1, 'b', {'service':'backend'}), (2, 'c', {'service':'frontend'});

-- map['service'] IN with multiple values: should return rows 0 and 1
SELECT id FROM tab WHERE map['service'] IN ('web-api', 'backend', 'dashboard') ORDER BY id;

-- Verify count
SELECT count() FROM tab WHERE map['service'] IN ('web-api', 'backend', 'dashboard');

-- map['service'] IN with single value: should return row 2
SELECT id FROM tab WHERE map['service'] IN ('frontend') ORDER BY id;

-- Verify count
SELECT count() FROM tab WHERE map['service'] IN ('frontend');

-- Tuple IN with (key, map['service']): should return row 0
SELECT id FROM tab WHERE (key, map['service']) IN (('a', 'frontend'), ('a', 'web-api')) ORDER BY id;

-- Verify count
SELECT count() FROM tab WHERE (key, map['service']) IN (('a', 'frontend'), ('a', 'web-api'));

-- Tuple IN with (map['service'], key): should return row 2
SELECT id FROM tab WHERE (map['service'], key) IN (('frontend', 'c'), ('web-api', 'c')) ORDER BY id;

-- Verify count
SELECT count() FROM tab WHERE (map['service'], key) IN (('frontend', 'c'), ('web-api', 'c'));

DROP TABLE tab;
