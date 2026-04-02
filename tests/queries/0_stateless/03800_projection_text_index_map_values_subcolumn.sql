-- Tests that text indexes built on mapValues(m) work correctly when the analyzer
-- rewrites arrayElement(m, 'key') into the map.key_* subcolumn form.
-- Adapted from 04039_text_index_map_values_subcolumn.sql for projection text indexes.

SET enable_analyzer = 1;
SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt32,
    map Map(String, String),
    PROJECTION idx_mv INDEX mapValues(map) TYPE text(tokenizer = 'array'),
    PROJECTION idx_mk INDEX mapKeys(map) TYPE text(tokenizer = 'array')
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 1, index_granularity_bytes = '10M', min_bytes_for_wide_part = 0;

INSERT INTO tab VALUES (0, {'service':'web-api'}), (1, {'service':'backend'}), (2, {'service':'frontend'});

-- mapValues index with IN operator (subcolumn form via analyzer)
SELECT id FROM tab WHERE map['service'] IN ('web-api', 'backend') ORDER BY id;

-- mapValues index with equality (subcolumn form via analyzer)
SELECT id FROM tab WHERE map['service'] = 'web-api' ORDER BY id;

-- empty string IN should not skip granules
SELECT id FROM tab WHERE map['nonexistent'] IN ('') ORDER BY id;

-- single value IN
SELECT id FROM tab WHERE map['service'] IN ('frontend') ORDER BY id;

DROP TABLE tab;
