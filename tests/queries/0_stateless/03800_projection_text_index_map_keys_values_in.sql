-- Tests that projection text indexes on mapKeys(m) and mapValues(m) cooperate to skip granules with IN on large data.
-- Adapted from 04036_text_index_map_keys_values_in.sql
--
-- Data layout (200000 rows, 25 granules with index_granularity=8192):
--   rows 0-49999:       {'service': 'web-api'}
--   rows 50000-99999:   {'service': 'backend'}
--   rows 100000-149999: {'service': 'frontend', 'env': 'prod'}
--   rows 150000-199999: {'service': 'worker',   'env': 'staging'}

SET enable_analyzer = 1;
SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    map Map(String, String),
    PROJECTION idx_map_keys INDEX mapKeys(map) TYPE text(tokenizer = 'array'),
    PROJECTION idx_map_values INDEX mapValues(map) TYPE text(tokenizer = 'array')
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 8192, index_granularity_bytes = '10M', min_bytes_for_wide_part = 0;

INSERT INTO tab SELECT
    number,
    CAST(
        arrayConcat(
            [('service', multiIf(number < 50000, 'web-api', number < 100000, 'backend', number < 150000, 'frontend', 'worker'))],
            if(number >= 100000, [('env', if(number < 150000, 'prod', 'staging'))], [])
        ),
        'Map(String, String)'
    )
FROM numbers(200000);

-- env = 'prod': rows 100000-149999 => 50000 rows
SELECT count() FROM tab WHERE map['env'] IN ('prod');

-- service = 'worker' AND env = 'prod': no rows (worker has staging, not prod)
SELECT count() FROM tab WHERE map['service'] IN ('worker') AND map['env'] IN ('prod');

-- service IN ('web-api', 'frontend'): rows 0-49999 + 100000-149999 => 100000 rows
SELECT count() FROM tab WHERE map['service'] IN ('web-api', 'frontend');

-- env IN ('prod', 'staging'): rows 100000-199999 => 100000 rows
SELECT count() FROM tab WHERE map['env'] IN ('prod', 'staging');

DROP TABLE tab;
