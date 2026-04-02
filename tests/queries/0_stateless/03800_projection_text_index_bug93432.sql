-- Tests hasAllTokens on empty Array with projection text index.
-- Adapted from 02346_text_index_bug93432.sql for projection text index.

SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
  col Array(String),
  PROJECTION idx INDEX col TYPE text(tokenizer=array)
)
ENGINE=MergeTree() ORDER BY tuple()
AS SELECT [];

SELECT * from tab WHERE hasAllTokens(col, 'abc');

DROP TABLE tab;
