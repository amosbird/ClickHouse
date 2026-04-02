-- Test that the projection text index cannot be created with fixed granularity.

SET enable_full_text_index = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    `id`   UInt64,
    `text` String,
    PROJECTION inv_idx INDEX text TYPE text(tokenizer = 'splitByNonAlpha')
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 32, index_granularity_bytes = 0, min_bytes_for_wide_part = 0; -- {serverError SUPPORT_IS_DISABLED }

-- Test that the projection text index works correctly when the number of rows in a part is smaller than the index_granularity.
-- (Reproduces a crash scenario from the upstream text index test.)

SET use_skip_indexes_on_data_read = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    `id`   UInt64,
    `text` String,
    PROJECTION inv_idx INDEX text TYPE text(tokenizer = 'splitByNonAlpha')
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 32, min_bytes_for_wide_part = 0;

INSERT INTO tab VALUES (0,'a'),(1,'b'),(2,'c');

SELECT id FROM tab WHERE hasToken(text, 'b');

SELECT id FROM tab WHERE hasToken(text, 'c');

TRUNCATE TABLE tab;

INSERT INTO tab VALUES (0,'a'),(1,'b'),(2,'c'),(3,'d');

SELECT id FROM tab WHERE hasToken(text, 'b');

SELECT id FROM tab WHERE hasToken(text, 'd');

INSERT INTO tab SELECT number , 'aaabbbccc' FROM numbers(128);

SELECT id FROM tab WHERE hasToken(text, 'aaabbbccc');

DROP TABLE tab;
