-- Tests vertical merges for columns with projection text index.
-- Adapted from 02346_text_index_vertical_merge.sql for projection text index.
-- Two separate projections (one for each column) replace the two skip indexes.

SET enable_full_text_index = 1;
SET use_skip_indexes = 1;
SET mutations_sync = 2;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    c1 String,
    c2 String
)
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, vertical_merge_algorithm_min_rows_to_activate = 1, vertical_merge_algorithm_min_columns_to_activate = 1;

INSERT INTO tab SELECT number, 'c1' || toString(number), 'c2' || toString(number + 1) FROM numbers(10000);

ALTER TABLE tab ADD PROJECTION idx_c1 INDEX c1 TYPE text(tokenizer = ngrams(3));
ALTER TABLE tab ADD PROJECTION idx_c2 INDEX c2 TYPE text(tokenizer = ngrams(3));

INSERT INTO tab SELECT number, 'c1' || toString(number), 'c2' || toString(number + 1) FROM numbers(10000);

SELECT count() FROM tab WHERE hasAllTokens(c1, 'c11') AND hasAllTokens(c2, 'c21');

OPTIMIZE TABLE tab FINAL;

SELECT count() FROM tab WHERE hasAllTokens(c1, 'c11') AND hasAllTokens(c2, 'c21');

DROP TABLE tab;
