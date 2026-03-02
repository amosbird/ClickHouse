#pragma once
#include <absl/container/flat_hash_map.h>

#include <base/defines.h>
#include <base/types.h>
#include <Common/PODArray.h>

#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

namespace DB
{

struct TokenPostingsInfo;
class WriteBuffer;
class ReadBuffer;
class IColumn;
struct LargePostingListReaderStream;
using LargePostingListReaderStreamPtr = std::shared_ptr<LargePostingListReaderStream>;

/// Lazy cursor over a compressed posting list (sorted row IDs for a token).
///
/// Storage layout (two-level hierarchy):
///   Large blocks  — variable-size segments of the posting list, each stored as a
///                   contiguous region in the .lpst stream with its own Index Section.
///   Packed blocks — fixed-size 128-element groups within a large block, delta-encoded
///                   and compressed with TurboPFor.  The last packed block in a large
///                   block may be shorter (the "tail block").
///
/// Each large block's Index Section (read in `prepare`) stores two parallel arrays:
///   `packed_block_last_doc_ids[j]` — last doc_id of packed block j
///   `packed_block_offsets[j]`      — absolute byte offset of packed block j in .lpst
/// These enable O(log N) seek via binary search + random file access.
///
/// Embedded postings (small cardinality tokens) are stored inline as a Roaring Bitmap
/// in the dictionary stream and decoded entirely in `prepare`; no .lpst stream is used.
///
/// Two access patterns:
///   1. Iterator: `valid` / `value` / `next` / `seek` — for leapfrog intersection.
///   2. Linear scan: `linearOr` / `linearAnd` — for brute-force bitmap operations.
class PostingListCursor
{
public:
    /// Construct a cursor with its own .lpst reader stream (large posting lists).
    PostingListCursor(LargePostingListReaderStreamPtr owned_stream_, const TokenPostingsInfo & info_);

    /// Construct a cursor without a stream (embedded posting lists only).
    PostingListCursor(const TokenPostingsInfo & info_);

    /// Set bits in `data` for all doc_ids in [row_offset, row_offset + num_rows).
    void linearOr(UInt8 * data, size_t row_offset, size_t num_rows);

    /// Increment counters in `data` for all doc_ids in [row_offset, row_offset + num_rows).
    void linearAnd(UInt8 * data, size_t row_offset, size_t num_rows);

    /// Move to the next doc_id.
    void next();

    /// True if cursor points to a valid doc_id.
    bool valid() const { return is_valid; }

    /// Current doc_id.  Undefined when `valid` returns false.
    uint32_t value() const { return current_values[index]; }

    /// Advance to the first doc_id >= target.
    void seek(uint32_t target);

    /// Posting list density: cardinality / (max_doc_id - min_doc_id + 1).
    /// Used to choose between leapfrog and brute-force algorithms.
    double density() const { return density_val; }

    /// Total number of doc_ids in the posting list.
    /// Used to sort cursors by selectivity for leapfrog intersection.
    UInt32 cardinality() const;

private:
    static constexpr size_t TURBOPFOR_BLOCK_SIZE = 128;

    /// Load metadata for `large_block_idx`-th large block.
    /// For large postings: reads the Index Section from .lpst (packed block index),
    /// but does NOT decode any packed block data yet.
    /// For embedded postings: decodes the entire Roaring Bitmap into `current_values`.
    void prepare(size_t large_block);

    void linearOrImpl(size_t large_block, UInt8 *, size_t row_begin, size_t row_end);
    void linearAndImpl(size_t large_block, UInt8 *, size_t row_begin, size_t row_end);

    /// Seek to the first doc_id >= target within the current large block.
    /// Uses binary search on `packed_block_last_doc_ids` for O(log N) access.
    /// Returns false if target exceeds this large block's range.
    bool seekImpl(uint32_t target);

    /// Decode the next packed block (128 doc_ids, or fewer for the tail block).
    /// When `need_seek_before_decode` is set, seeks to the absolute stream offset
    /// from the packed block index before reading.  For large block 0's packed
    /// block 0, prepends `first_doc_id` which is stored separately in the dictionary.
    /// Returns false when no more packed blocks remain in the current large block.
    bool decodeNextBlock();

    LargePostingListReaderStream * stream = nullptr;   /// Non-owning pointer (may alias owned_stream).
    LargePostingListReaderStreamPtr owned_stream;      /// Owning handle; nullptr for embedded postings.

    const TokenPostingsInfo & info;

    std::vector<uint32_t> current_values;  /// Decoded doc_ids of the current packed block.
    size_t index = 0;                      /// Read position within current_values.

    /// Per-large-block packed block layout (recomputed in `prepare`).
    size_t block_count = 0;              /// Total packed blocks, including the tail block.
    size_t current_block = 0;            /// Index of the packed block being iterated.
    size_t tail_size = 0;                /// Element count of the tail block (< 128), 0 if aligned.
    UInt32 large_block_doc_count = 0;    /// Total doc count in the current large block.
    UInt32 last_decoded_doc_id = 0;      /// Last doc_id decoded (delta base for next block).

    /// Packed block index loaded from Index Section in `prepare`.
    /// Enables O(log N) seek within a large block.
    std::vector<UInt32> packed_block_last_doc_ids;  /// packed_block_last_doc_ids[j] = last doc_id of packed block j.
    std::vector<UInt64> packed_block_offsets;        /// packed_block_offsets[j] = absolute byte offset in .lpst.

    /// Large block iteration state.
    size_t total_large_blocks = 0;             /// Number of large blocks for this token.
    size_t current_large_block_idx = 0;        /// Index of the large block currently loaded.
    bool has_prepared_first_large_block = false;

    bool is_valid = true;
    bool is_embedded = false;

    /// When true, `decodeNextBlock` seeks to the packed block offset before reading.
    /// Set after `prepare` (Index Section read moves the stream position) and after
    /// `seekImpl` (random jump).  Cleared after the first seek-based decode so that
    /// sequential packed-block reads can proceed without redundant seeks.
    bool need_seek_before_decode = true;

    double density_val = 0;
};

using PostingListCursorPtr = std::shared_ptr<PostingListCursor>;
using PostingListCursorMap = absl::flat_hash_map<std::string_view, PostingListCursorPtr>;

/// Union (OR) of posting lists: set output[row] = 1 if the row appears in ANY posting list.
/// Each cursor performs a linear scan over its doc_ids.
/// `brute_force_apply` and `density_threshold` are unused (kept for API symmetry with intersection).
void lazyUnionPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool brute_force_apply, float density_threshold);

/// Intersection (AND) of posting lists: set output[row] = 1 only if the row appears in ALL posting lists.
///
/// Adaptive algorithm selection based on posting list density:
///   - n == 1:  direct linear scan (degenerate case, same as union).
///   - Dense (min density >= threshold) or `brute_force_apply`:
///     Brute-force bitmap counting — first cursor sets bits, remaining cursors increment counters,
///     then a final pass keeps only rows where count == n.  Favors sequential memory access.
///   - Sparse:  leapfrog intersection — cursors sorted by ascending cardinality, the sparsest
///     cursor leads and others seek forward.  Favors skipping large unused ranges.
void lazyIntersectPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool brute_force_apply, float density_threshold);

}
