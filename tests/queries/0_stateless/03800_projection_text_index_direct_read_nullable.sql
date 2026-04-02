-- Test that text index direct read optimization works correctly with nullable needles.
-- Adapted from 02346_text_index_direct_read_nullable.sql for projection text index.

SET enable_full_text_index = 1;
SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    key UInt64,
    val String,
    PROJECTION idx INDEX val TYPE text(tokenizer = 'splitByNonAlpha')
)
ENGINE = MergeTree
ORDER BY key;

INSERT INTO tab VALUES (1, 'hello world'), (2, 'foo bar');

-- When hasAnyTokens has a nullable needle, the result type is Nullable(UInt8).
-- The text index direct read optimization should correctly handle this by wrapping the result with toNullable.
SELECT * FROM tab WHERE hasAnyTokens(val, toNullable('hello'));
SELECT * FROM tab PREWHERE hasAnyTokens(val, toNullable('hello'));

-- Test with not() which also returns Nullable(UInt8) when its argument is nullable
SELECT * FROM tab WHERE not(hasAnyTokens(val, toNullable('FOO')));
SELECT * FROM tab PREWHERE not(hasAnyTokens(val, toNullable('FOO')));

-- Test in a more complex expression (similar to the fuzzer query that caused the failure)
SELECT count() FROM tab PREWHERE and(key > 0, not(hasAnyTokens(val, toNullable('FOO'))));

DROP TABLE tab;
