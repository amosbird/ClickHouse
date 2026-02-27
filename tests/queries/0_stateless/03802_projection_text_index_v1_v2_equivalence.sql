-- Tags: no-fasttest
-- no-fasttest: It can be slow

-- Equivalence test: v1 and v2 posting list formats must produce identical query results.
-- Two tables with the same schema (only posting_list_version differs) receive the same data.
-- Every query is run against both tables and the results must match.

SET enable_full_text_index = 1;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0.0;
SET use_skip_indexes_on_data_read = 0;

DROP TABLE IF EXISTS tab_eq_v1;
DROP TABLE IF EXISTS tab_eq_v2;

----------------------------------------------------
-- Schema: identical except posting_list_version
----------------------------------------------------

CREATE TABLE tab_eq_v1(
    k UInt64,
    s String,
    PROJECTION af INDEX s TYPE text(tokenizer = 'splitByNonAlpha', posting_list_block_size = 128, posting_list_version = 1)
) ENGINE = MergeTree() ORDER BY k
  SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi';

CREATE TABLE tab_eq_v2(
    k UInt64,
    s String,
    PROJECTION af INDEX s TYPE text(tokenizer = 'splitByNonAlpha', posting_list_block_size = 128, posting_list_version = 2)
) ENGINE = MergeTree() ORDER BY k
  SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi';

----------------------------------------------------
-- Insert identical data (small posting lists)
----------------------------------------------------
SELECT 'Part 1: Small data';

INSERT INTO tab_eq_v1 SELECT number, if(number % 3 = 0, 'apple banana', if(number % 3 = 1, 'cherry date', 'elderberry fig')) FROM numbers(300);
INSERT INTO tab_eq_v2 SELECT number, if(number % 3 = 0, 'apple banana', if(number % 3 = 1, 'cherry date', 'elderberry fig')) FROM numbers(300);

-- Single token queries
SELECT 'hasToken apple';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'apple');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'apple');

SELECT 'hasToken cherry';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'cherry');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'cherry');

SELECT 'hasToken elderberry';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'elderberry');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'elderberry');

-- AND (intersection) — tokens from different groups, expect 0
SELECT 'AND intersection (empty)';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'apple') AND hasToken(s, 'cherry');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'apple') AND hasToken(s, 'cherry');

-- AND — tokens from the same group, expect 100
SELECT 'AND same group';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'apple') AND hasToken(s, 'banana');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'apple') AND hasToken(s, 'banana');

-- OR (union)
SELECT 'OR union';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'apple') OR hasToken(s, 'cherry');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'apple') OR hasToken(s, 'cherry');

-- Non-existent token
SELECT 'non-existent token';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'grape');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'grape');

-- Actual row values
SELECT 'row values';
SELECT k, s FROM tab_eq_v1 WHERE hasToken(s, 'apple') ORDER BY k LIMIT 5;
SELECT k, s FROM tab_eq_v2 WHERE hasToken(s, 'apple') ORDER BY k LIMIT 5;

----------------------------------------------------
-- Insert more data to trigger large posting lists
----------------------------------------------------
SELECT 'Part 2: Large data (triggers large posting list path)';

TRUNCATE TABLE tab_eq_v1;
TRUNCATE TABLE tab_eq_v2;

INSERT INTO tab_eq_v1 SELECT number, if(number % 5 = 0, 'common frequent', if(number % 5 = 1, 'common medium', if(number % 5 = 2, 'rare special', if(number % 5 = 3, 'unique odd', 'unique even')))) FROM numbers(5000);
INSERT INTO tab_eq_v2 SELECT number, if(number % 5 = 0, 'common frequent', if(number % 5 = 1, 'common medium', if(number % 5 = 2, 'rare special', if(number % 5 = 3, 'unique odd', 'unique even')))) FROM numbers(5000);

SELECT 'large: hasToken common';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'common');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'common');

SELECT 'large: hasToken rare';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'rare');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'rare');

SELECT 'large: hasToken frequent';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'frequent');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'frequent');

SELECT 'large: AND common AND frequent';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'common') AND hasToken(s, 'frequent');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'common') AND hasToken(s, 'frequent');

SELECT 'large: AND common AND rare (empty)';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'common') AND hasToken(s, 'rare');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'common') AND hasToken(s, 'rare');

SELECT 'large: OR rare OR unique';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'rare') OR hasToken(s, 'unique');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'rare') OR hasToken(s, 'unique');

SELECT 'large: 3-way AND common AND medium (not frequent)';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'common') AND hasToken(s, 'medium');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'common') AND hasToken(s, 'medium');

----------------------------------------------------
-- Multi-part merge test
----------------------------------------------------
SELECT 'Part 3: After merge';

TRUNCATE TABLE tab_eq_v1;
TRUNCATE TABLE tab_eq_v2;

INSERT INTO tab_eq_v1 SELECT number, if(number % 2 = 0, 'merge alpha', 'merge beta') FROM numbers(2000);
INSERT INTO tab_eq_v1 SELECT number + 2000, if((number + 2000) % 2 = 0, 'merge alpha', 'merge beta') FROM numbers(2000);

INSERT INTO tab_eq_v2 SELECT number, if(number % 2 = 0, 'merge alpha', 'merge beta') FROM numbers(2000);
INSERT INTO tab_eq_v2 SELECT number + 2000, if((number + 2000) % 2 = 0, 'merge alpha', 'merge beta') FROM numbers(2000);

OPTIMIZE TABLE tab_eq_v1 FINAL;
OPTIMIZE TABLE tab_eq_v2 FINAL;

SELECT 'after merge: hasToken alpha';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'alpha');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'alpha');

SELECT 'after merge: hasToken beta';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'beta');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'beta');

SELECT 'after merge: AND alpha AND beta (empty)';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'alpha') AND hasToken(s, 'beta');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'alpha') AND hasToken(s, 'beta');

SELECT 'after merge: AND merge AND alpha';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'merge') AND hasToken(s, 'alpha');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'merge') AND hasToken(s, 'alpha');

SELECT 'after merge: OR alpha OR beta';
SELECT count() FROM tab_eq_v1 WHERE hasToken(s, 'alpha') OR hasToken(s, 'beta');
SELECT count() FROM tab_eq_v2 WHERE hasToken(s, 'alpha') OR hasToken(s, 'beta');

----------------------------------------------------
-- Cleanup
----------------------------------------------------
DROP TABLE tab_eq_v1;
DROP TABLE tab_eq_v2;
