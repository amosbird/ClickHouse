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

/// A cursor for lazily iterating over a compressed posting list stored in
/// ProjectionIndex V2 format (TurboPFor delta-encoded with per-large-block
/// packed block index).
///
/// Posting list: a sorted list of row IDs where a token appears.
/// This cursor decodes blocks on-demand, avoiding full decompression upfront.
/// The V2 Index Session (loaded in `prepare`) enables O(log N) seek to any
/// 128-doc packed block within a large block via binary search on `packed_block_last_doc_ids`
/// and random seek using `packed_block_offsets`.
///
/// Supports two access patterns:
/// 1. Iterator-style: valid() / value() / next() / seek() - for skip-list intersection
/// 2. Linear scan: linearOr() / linearAnd() - for brute-force bitmap operations
///
/// The posting list may span multiple large blocks. Use addLargeBlock()
/// to register additional large blocks before iteration.
class PostingListCursor
{
public:
    /// Construct a cursor for large posting lists backed by a LargePostingListReaderStream.
    PostingListCursor(LargePostingListReaderStream * stream_, const TokenPostingsInfo & info_, size_t large_block);

    /// Construct a cursor that owns an independent LargePostingListReaderStream.
    /// Used in lazy apply mode to give each cursor its own stream, avoiding seek contention.
    PostingListCursor(LargePostingListReaderStreamPtr owned_stream_, const TokenPostingsInfo & info_, size_t large_block);

    /// Construct a cursor for embedded posting lists (no stream needed).
    PostingListCursor(const TokenPostingsInfo & info_, size_t large_block);

    /// Register an additional large block to iterate over.
    void addLargeBlock(size_t);

    /// Brute-force: set bits for all row IDs in range [row_offset, row_offset + num_rows).
    void linearOr(UInt8 * data, size_t row_offset, size_t num_rows);

    /// Brute-force: increment counts for all row IDs in range.
    void linearAnd(UInt8 * data, size_t row_offset, size_t num_rows);

    /// Move to next row ID.
    void next();

    /// Returns true if cursor points to a valid row ID.
    bool valid() const { return is_valid; }

    /// Returns current row ID. Requires valid() == true.
    uint32_t value() const { return current_values[index]; }

    /// Advance to first row ID >= target.
    void seek(uint32_t target);

    /// Returns posting list density: count / (max - min + 1).
    /// Used to decide between skip-list vs brute-force algorithm.
    double density() const { return density_val; }

    /// Returns total cardinality of the posting list.
    /// Used to sort cursors by selectivity for leapfrog intersection.
    UInt32 cardinality() const;

private:
    static constexpr size_t TURBOPFOR_BLOCK_SIZE = 128;

    /// Load and prepare data for the given large block.
    /// For large postings: loads the V2 Index Session (reads Index Section
    /// from .lpst), but does NOT decode any packed block.
    void prepare(size_t large_block);

    void linearOrImpl(size_t large_block, UInt8 *, size_t row_begin, size_t row_end);
    void linearAndImpl(size_t large_block, UInt8 *, size_t row_begin, size_t row_end);

    /// Seek to the first doc_id >= target within the current large block
    /// using the V2 packed block index for O(log N) random access.
    bool seekImpl(uint32_t target);

    /// Decode the next 128-doc (or tail) packed block from the .lpst stream.
    /// When `need_seek_before_decode` is set (after `prepare` or `seek`), seeks to
    /// the absolute offset from the packed block index, computes the correct delta
    /// base, and for the first large block's packed block 0, prepends `first_doc_id`.
    /// Returns false if no more packed blocks remain in the current large block.
    bool decodeNextBlock();

    inline void maybeEraseUnusedLargeBlocks(int unused_large_block_index)
    {
        chassert(static_cast<size_t>(unused_large_block_index) < large_blocks.size());
        if (unused_large_block_index >= 0 && large_blocks.size() > 1)
        {
            large_blocks.erase(large_blocks.begin(), large_blocks.begin() + unused_large_block_index + 1);
        }
    }

    LargePostingListReaderStream * stream = nullptr;
    LargePostingListReaderStreamPtr owned_stream;

    const TokenPostingsInfo & info;

    /// Decoded row IDs of current packed block
    std::vector<uint32_t> current_values;
    /// Position within current_values
    size_t index = 0;

    /// Number of 128-doc packed blocks in the current large block (including tail block)
    size_t block_count = 0;
    /// Current packed block being iterated
    size_t current_block = 0;
    /// Size of the tail packed block (< 128), or 0 if perfectly aligned
    size_t tail_size = 0;
    /// Total doc count in the current large block (block_doc_count from LargePostingBlockMeta)
    UInt32 large_block_doc_count = 0;
    /// Last decoded doc_id (for delta decoding continuity)
    UInt32 last_decoded_doc_id = 0;

    /// V2 Index section for the current large block: packed block level index
    /// loaded in `prepare`, enables O(log N) seek within a large block.
    /// `packed_block_last_doc_ids[j]` is the last doc_id of the j-th 128-doc packed block.
    /// `packed_block_offsets[j]` is the absolute byte offset of the j-th packed block in .lpst.
    std::vector<UInt32> packed_block_last_doc_ids;
    std::vector<UInt64> packed_block_offsets;

    /// Large blocks this cursor covers (indexes into info.offsets / info.ranges)
    std::vector<size_t> large_blocks;
    size_t current_large_block_idx = std::numeric_limits<size_t>::max();

    bool is_valid = true;
    bool is_embedded = false;
    /// When true, decodeNextBlock will seek to the packed block offset before reading.
    /// Set after prepare (Index Section read leaves stream at wrong position) and after
    /// seekImpl (random jump). Cleared after the first seek so that sequential block
    /// reads skip the redundant seek — the stream is already at the next block's start.
    bool need_seek_before_decode = true;
    double density_val = 0;
};

using PostingListCursorPtr = std::shared_ptr<PostingListCursor>;
using PostingListCursorMap = absl::flat_hash_map<std::string_view, PostingListCursorPtr>;

/// Compute union (OR) of multiple posting lists using lazy decoding.
///
/// Used for TextSearchMode::Any - a row matches if it contains ANY of the search tokens.
/// Iterates through each posting list and sets corresponding bits in the output column.
///
/// Note: brute_force_apply and density_threshold parameters are unused in union operation
/// since linear scan is always used (no optimization benefit from skip-list for OR).
///
/// @param column         Output column (UInt8), sets bit to 1 for rows matching any token
/// @param postings       Map from token to its posting list cursor
/// @param search_tokens  List of tokens to search (determines iteration order)
/// @param column_offset  Starting position in output column to write results
/// @param row_offset     First row ID in the processing range
/// @param num_rows       Number of rows to process
/// @param brute_force_apply  Unused (kept for API consistency with intersection)
/// @param density_threshold  Unused (kept for API consistency with intersection)
void lazyUnionPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool brute_force_apply, float density_threshold);

/// Compute intersection (AND) of multiple posting lists using lazy decoding.
///
/// Used for TextSearchMode::All - a row matches only if it contains ALL search tokens.
/// Employs adaptive algorithm selection based on posting list density:
///
/// Algorithm selection:
///   - Single list (n=1): direct linear scan, equivalent to union
///   - Dense lists (density >= threshold) or brute_force_apply=true:
///     Uses brute-force bitmap counting - each cursor marks its row IDs,
///     then count rows where all cursors have set bits (sequential memory access)
///   - Sparse lists: uses skip-list based leapfrog intersection -
///     cursors advance together, only decode blocks as needed (fewer elements to process)
///
/// The density-based switching optimizes for different access patterns:
///   - Sparse posting lists: skip-list is faster due to fewer elements
///   - Dense posting lists: brute-force is faster due to sequential memory access
///
/// @param column         Output column (UInt8), sets bit to 1 for rows matching all tokens
/// @param postings       Map from token to its posting list cursor
/// @param search_tokens  List of tokens to search (determines which cursors to use)
/// @param column_offset  Starting position in output column to write results
/// @param row_offset     First row ID in the processing range
/// @param num_rows       Number of rows to process
/// @param brute_force_apply  Force brute-force algorithm regardless of density
/// @param density_threshold  Switch to brute-force if average density >= this value
void lazyIntersectPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool brute_force_apply, float density_threshold);

}
