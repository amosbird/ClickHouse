-- Tests merge with empty blocks from TTL DELETE with projection text index.
-- The projection text index is created with the table from the start (since projection
-- text index doesn't support ALTER ADD after data exists without MATERIALIZE).
-- Adapted from 04033_text_index_empty_block_during_merge.sql

SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt32,
    key UInt32,
    text String,
    dt DateTime,
    PROJECTION idx_text INDEX text TYPE text(tokenizer = 'splitByNonAlpha')
)
ENGINE = MergeTree
ORDER BY id
TTL dt + INTERVAL 1 MONTH DELETE WHERE key < 5
SETTINGS merge_max_block_size = 1024;

SYSTEM STOP MERGES tab;

INSERT INTO tab (id, key, text, dt)
SELECT number, number / 5000, 'hello world', toDateTime('2000-01-01 00:00:00')
FROM numbers(50000);

SYSTEM START MERGES tab;

OPTIMIZE TABLE tab FINAL;

SELECT count() FROM tab WHERE hasAllTokens(text, 'hello');

DROP TABLE tab;
