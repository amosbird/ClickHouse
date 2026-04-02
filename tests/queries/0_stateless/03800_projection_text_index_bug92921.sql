-- Tests text index projection + normal projection coexistence during merge.
-- Adapted from 02346_text_index_bug92921.sql for projection text index.

SET enable_full_text_index = 1;
SET use_skip_indexes = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    s Array(String),
    PROJECTION idx INDEX s TYPE text(tokenizer = sparseGrams),
    PROJECTION p (SELECT s ORDER BY s)
)
ENGINE = MergeTree() ORDER BY tuple();

INSERT INTO TABLE tab (s) VALUES (['A']);
INSERT INTO TABLE tab (s) VALUES (['B']);

OPTIMIZE TABLE tab FINAL;

DROP TABLE tab;
