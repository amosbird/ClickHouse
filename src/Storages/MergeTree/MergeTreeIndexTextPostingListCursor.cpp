#include <Storages/MergeTree/MergeTreeIndexTextPostingListCursor.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <algorithm>

#include <turbopfor.h>
namespace DB
{

namespace ErrorCodes
{
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
}

namespace
{

/// Prefix-based variable-length integer decoding (mirrors PostingListData.cpp VarInt encoding).
/// Used for reading compressed block sizes and Index Section fields in .lpst files.
///
/// Encoding scheme (first byte determines length):
///   [0, 176]     → 1 byte   (value = first byte)
///   [177, 240]   → 2 bytes  (value up to 16560)
///   [241, 248]   → 3 bytes  (value up to 540848)
///   249          → 4 bytes  (value up to 2^24 - 1)
///   250          → 5 bytes  (value up to 2^32 - 1)
inline void readPrefixVarUInt32(UInt32 & x, ReadBuffer & istr)
{
    static constexpr UInt32 ONE_BYTE_MAX = 176;
    static constexpr UInt8 TWO_BYTE_MARKER_END = 240;
    static constexpr UInt8 THREE_BYTE_MARKER_END = 248;
    static constexpr UInt8 FOUR_BYTE_MARKER = 249;

    static constexpr UInt32 TWO_BYTE_OFFSET = 177;
    static constexpr UInt32 THREE_BYTE_OFFSET = 16561;

    if (istr.eof()) [[unlikely]]
        throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");

    const UInt8 first_byte = *istr.position()++;

    if (first_byte <= ONE_BYTE_MAX)
    {
        x = first_byte;
        return;
    }

    if (istr.eof()) [[unlikely]]
        throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");
    const UInt8 second_byte = *istr.position()++;

    if (first_byte <= TWO_BYTE_MARKER_END)
    {
        x = ((first_byte - 177) << 8) + second_byte + TWO_BYTE_OFFSET;
        return;
    }

    if (istr.eof()) [[unlikely]]
        throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");
    const UInt8 third_byte = *istr.position()++;

    if (first_byte <= THREE_BYTE_MARKER_END)
    {
        x = ((first_byte - 241) << 16) + (second_byte << 8) + third_byte + THREE_BYTE_OFFSET;
        return;
    }

    if (istr.eof()) [[unlikely]]
        throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");
    const UInt8 fourth_byte = *istr.position()++;

    if (first_byte == FOUR_BYTE_MARKER)
    {
        x = (second_byte << 16) | (third_byte << 8) | fourth_byte;
        return;
    }

    if (istr.eof()) [[unlikely]]
        throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");
    const UInt8 fifth_byte = *istr.position()++;
    x = (UInt32(second_byte) << 24) | (UInt32(third_byte) << 16) | (UInt32(fourth_byte) << 8) | fifth_byte;
}

} // anonymous namespace

PostingListCursor::PostingListCursor(LargePostingListReaderStreamPtr owned_stream_, const TokenPostingsInfo & info_)
    : stream(owned_stream_.get())
    , owned_stream(std::move(owned_stream_))
    , info(info_)
    , total_large_blocks(info.offsets.size())
{
    if (total_large_blocks > 0)
        prepare(0);
    else if (info.embedded_postings)
    {
        /// Embedded postings with no ranges/offsets — call prepare to decode them.
        prepare(0);
    }
    else
        is_valid = false;
}

PostingListCursor::PostingListCursor(const TokenPostingsInfo & info_)
    : PostingListCursor(nullptr, info_)
{
}

UInt32 PostingListCursor::cardinality() const
{
    return info.cardinality;
}

void PostingListCursor::prepare(size_t large_block_idx)
{
    has_prepared_first_large_block = true;

    decoded_count = 0;

    if (info.embedded_postings)
    {
        /// Embedded posting list: already materialized as a Roaring Bitmap.
        /// Decode all doc_ids into decoded_values in one shot (at most 6 entries).
        chassert(!stream);
        decoded_count = info.embedded_postings->cardinality();
        chassert(decoded_count <= TURBOPFOR_BLOCK_SIZE + 1);
        info.embedded_postings->toUint32Array(decoded_values);
        current_block = 0;
        block_count = 1;
        current_large_block_idx = large_block_idx;
        is_valid = decoded_count > 0;
        is_embedded = true;

        if (decoded_count >= 2)
            density_val = static_cast<double>(decoded_count) / static_cast<double>(decoded_values[decoded_count - 1] - decoded_values[0] + 1);
        else
            density_val = 1.0;
        return;
    }

    /// Large posting list: read from .lpst stream, TurboPFor delta-encoded.
    /// Each large block has a corresponding `LargePostingBlockMeta`.
    chassert(stream);
    chassert(large_block_idx < info.offsets.size());
    chassert(large_block_idx < info.ranges.size());

    const auto & block_meta = info.offsets[large_block_idx];
    large_block_doc_count = block_meta.block_doc_count;

    /// Compute packed block structure from the total doc count.
    size_t full_blocks = large_block_doc_count / TURBOPFOR_BLOCK_SIZE;
    tail_size = large_block_doc_count % TURBOPFOR_BLOCK_SIZE;
    block_count = full_blocks + (tail_size > 0 ? 1 : 0);

    current_block = 0;
    current_large_block_idx = large_block_idx;
    need_seek_before_decode = true;

    /// Density = doc_count / row_id_span.  Used for algorithm selection.
    const auto & range = info.ranges[large_block_idx];
    UInt32 range_span = static_cast<UInt32>(range.end) - static_cast<UInt32>(range.begin) + 1;
    density_val = (range_span > 0) ? static_cast<double>(large_block_doc_count + (large_block_idx == 0 ? 1 : 0)) / static_cast<double>(range_span) : 1.0;

    /// Seek to the Index Section and read the packed block index.
    chassert(block_meta.index_offset != 0);
    stream->seek(block_meta.index_offset);
    auto & data_buf = *stream->getDataBuffer();

    /// Index Section layout: [num_packed_blocks] [last_doc_ids...] [offsets...]
    UInt32 num_packed_blocks;
    readPrefixVarUInt32(num_packed_blocks, data_buf);
    chassert(num_packed_blocks == block_count);

    packed_block_last_doc_ids.resize(num_packed_blocks);
    packed_block_offsets.resize(num_packed_blocks);

    for (UInt32 j = 0; j < num_packed_blocks; ++j)
        readPrefixVarUInt32(packed_block_last_doc_ids[j], data_buf);
    for (UInt32 j = 0; j < num_packed_blocks; ++j)
        readVarUInt(packed_block_offsets[j], data_buf);

    if (block_count > 0 || large_block_idx == 0)
        is_valid = true;
    else
        is_valid = false;
}

bool PostingListCursor::decodeNextBlock()
{
    if (current_block >= block_count)
        return false;

    chassert(stream);

    chassert(current_block < packed_block_offsets.size());

    if (need_seek_before_decode)
    {
        stream->seek(packed_block_offsets[current_block]);
        need_seek_before_decode = false;
    }

    /// Compute the delta base for TurboPFor decoding.
    /// Block 0 of each large block uses the large block's range.begin as base;
    /// subsequent blocks use the last doc_id of the previous packed block.
    if (current_block == 0)
    {
        last_decoded_doc_id = static_cast<UInt32>(info.ranges[current_large_block_idx].begin);
        if (current_large_block_idx > 0)
            --last_decoded_doc_id;
    }
    else
        last_decoded_doc_id = packed_block_last_doc_ids[current_block - 1];

    auto &data_buf = *stream->getDataBuffer();

    /// Read the compressed payload length (prefix-varint encoded).
    UInt32 bytes;
    readPrefixVarUInt32(bytes, data_buf);

    UInt32 count = (current_block + 1 == block_count && tail_size > 0) ? static_cast<UInt32>(tail_size) : 128U;

    uint8_t * src_ptr;
    if (data_buf.available() >= bytes)
    {
        src_ptr = reinterpret_cast<uint8_t *>(data_buf.position());
        data_buf.position() += bytes;
    }
    else
    {
        chassert(bytes <= 512);
        data_buf.readStrict(reinterpret_cast<char *>(stream->packed_buffer), bytes);
        src_ptr = stream->packed_buffer;
    }

    /// `first_doc_id` is stored in the dictionary stream and excluded from TurboPFor encoding.
    /// For large block 0, packed block 0: allocate an extra slot, decode into offset 1,
    /// then write `first_doc_id` into slot 0.
    bool prepend_first_doc_id = (current_large_block_idx == 0 && current_block == 0);
    UInt32 actual_count = prepend_first_doc_id ? count + 1 : count;

    decoded_count = actual_count;
    uint32_t * decode_dst = decoded_values + (prepend_first_doc_id ? 1 : 0);

    if (count == 128)
        turbopfor::p4D1Dec128v32(src_ptr, 128, decode_dst, last_decoded_doc_id);
    else
        turbopfor::p4D1Dec32(src_ptr, count, decode_dst, last_decoded_doc_id);

    if (prepend_first_doc_id)
        decoded_values[0] = static_cast<uint32_t>(info.ranges[0].begin);

    last_decoded_doc_id = decoded_values[actual_count - 1];
    index = 0;

    return true;
}

void PostingListCursor::seek(uint32_t target)
{
    /// Fast path: target may fall within the currently loaded large block.
    if (!is_embedded && seekImpl(target))
        return;

    /// Slow path: scan subsequent large blocks whose range covers the target.
    /// For embedded postings, `seekImpl` was skipped above, so start from current index.
    bool found = false;
    size_t start = is_embedded ? current_large_block_idx : current_large_block_idx + 1;
    for (size_t i = start; i < total_large_blocks; ++i)
    {
        const auto & range = info.ranges[i];
        if (range.end >= target)
        {
            prepare(i);
            if (seekImpl(target))
            {
                found = true;
                break;
            }
        }
    }

    is_valid = found;
}

bool PostingListCursor::seekImpl(uint32_t target)
{
    if (is_embedded)
    {
        /// Embedded: all doc_ids are in decoded_values, use binary search.
        auto it = std::lower_bound(decoded_values, decoded_values + decoded_count, target);
        if (it != decoded_values + decoded_count)
        {
            index = static_cast<size_t>(it - decoded_values);
            return true;
        }
        return false;
    }

    /// Check if target falls within the already-decoded packed block.
    if (index < decoded_count)
    {
        auto it = std::lower_bound(decoded_values + index, decoded_values + decoded_count, target);
        if (it != decoded_values + decoded_count)
        {
            index = static_cast<size_t>(it - decoded_values);
            return true;
        }
    }

    /// Binary search on packed_block_last_doc_ids: find the first packed block
    /// whose last doc_id >= target.
    auto it = std::lower_bound(packed_block_last_doc_ids.begin(), packed_block_last_doc_ids.end(), target);
    if (it == packed_block_last_doc_ids.end())
        return false;

    size_t j = static_cast<size_t>(it - packed_block_last_doc_ids.begin());

    /// If the target block is already decoded, search it directly without re-decoding.
    /// This avoids redundant TurboPFor decode + stream seek when consecutive seeks
    /// land in the same packed block (common in leapfrog intersection).
    if (j != current_block || decoded_count == 0)
    {
        current_block = j;
        need_seek_before_decode = true;
        decodeNextBlock();
    }

    /// Binary search within the decoded packed block.
    auto found_it = std::lower_bound(decoded_values, decoded_values + decoded_count, target);
    if (found_it != decoded_values + decoded_count)
    {
        index = static_cast<size_t>(found_it - decoded_values);
        return true;
    }

    return false;
}

void PostingListCursor::next()
{
    if (!is_valid)
        return;

    ++index;

    if (index >= decoded_count)
    {
        ++current_block;
        if (current_block < block_count)
        {
            /// More packed blocks in this large block — decode sequentially.
            decodeNextBlock();
            return;
        }

        /// Current large block exhausted — advance to next one.
        size_t next_large_block = current_large_block_idx + 1;
        if (next_large_block >= total_large_blocks)
        {
            is_valid = false;
            return;
        }

        prepare(next_large_block);
        decodeNextBlock();
    }
}

/// Scatter-write 1s into `out` for doc_ids in values[begin..length).
/// 4-wide loop with prefetch for cache-line utilization.
inline void padColumnForOr(UInt8 * __restrict out, const uint32_t * values, size_t row_begin, size_t begin, size_t length)
{
    const uint32_t * data_begin = values + begin;
    const uint32_t * data_end = values + length;

    if (data_begin >= data_end)
        return;

    const uint32_t * p = data_begin;
    const size_t count = data_end - data_begin;

    const uint32_t * loop_end = data_begin + (count / 4) * 4;

    for (; p < loop_end; p += 4)
    {
        __builtin_prefetch(p + 16, 0, 3);
        if (p + 4 < data_end)
            __builtin_prefetch(&out[p[4] - row_begin], 1, 0);

        out[p[0] - row_begin] = 1;
        out[p[1] - row_begin] = 1;
        out[p[2] - row_begin] = 1;
        out[p[3] - row_begin] = 1;
    }

    switch (data_end - p)
    {
        case 3: out[p[2] - row_begin] = 1; [[fallthrough]];
        case 2: out[p[1] - row_begin] = 1; [[fallthrough]];
        case 1: out[p[0] - row_begin] = 1; [[fallthrough]];
        default: break;
    }
}

void PostingListCursor::linearOrImpl(size_t large_block, UInt8 * __restrict out, size_t row_begin, size_t row_end)
{
    chassert(large_block < info.ranges.size());

    if (unlikely(is_embedded))
    {
        auto it = std::lower_bound(decoded_values, decoded_values + decoded_count, row_begin);
        if (it == decoded_values + decoded_count)
            return;
        size_t begin_idx = static_cast<size_t>(it - decoded_values);
        auto it_end = std::upper_bound(decoded_values, decoded_values + decoded_count, row_end);
        size_t end_idx = it_end - decoded_values;
        padColumnForOr(out, decoded_values, row_begin, begin_idx, end_idx);
        return;
    }

    /// Use packed_block_last_doc_ids[] to determine the range of packed blocks
    /// that overlap [row_begin, row_end], skipping all blocks outside this range
    /// without any I/O or decoding.
    ///
    /// packed_block_last_doc_ids[j] is the last doc_id in packed block j.
    /// The first doc_id of block j is approximately packed_block_last_doc_ids[j-1]+1
    /// (or info.ranges[large_block].begin for block 0).
    ///
    /// We want the first block whose last_doc_id >= row_begin (it may contain row_begin)
    /// and the last block whose first doc_id could be <= row_end.
    auto blk_start_it = std::lower_bound(packed_block_last_doc_ids.begin(), packed_block_last_doc_ids.end(), static_cast<UInt32>(row_begin));
    if (blk_start_it == packed_block_last_doc_ids.end())
        return;  /// All blocks end before row_begin.
    size_t blk_start = static_cast<size_t>(blk_start_it - packed_block_last_doc_ids.begin());

    /// Find the last block that could contain doc_ids in [row_begin, row_end].
    /// lower_bound(row_end) returns the first block with last_doc_id >= row_end;
    /// that block is the last one we need (it contains or ends at row_end).
    auto blk_end_it = std::lower_bound(packed_block_last_doc_ids.begin(), packed_block_last_doc_ids.end(), static_cast<UInt32>(row_end));
    size_t blk_end;
    if (blk_end_it == packed_block_last_doc_ids.end())
        blk_end = block_count;  /// row_end exceeds all blocks — process them all.
    else
        blk_end = static_cast<size_t>(blk_end_it - packed_block_last_doc_ids.begin()) + 1;  /// +1 for exclusive upper bound.

    /// Seek directly to the first relevant packed block.
    current_block = blk_start;
    need_seek_before_decode = true;

    for (size_t blk = blk_start; blk < blk_end; ++blk)
    {
        current_block = blk;
        if (!decodeNextBlock())
            return;

        if (decoded_count == 0)
            continue;

        /// For the first and last blocks in the range, the block may only partially
        /// overlap [row_begin, row_end] — use binary search to clip.
        /// For all middle blocks, every doc_id is guaranteed to be within
        /// [row_begin, row_end], so skip the binary searches entirely.
        bool is_first_block = (blk == blk_start);
        bool is_last_block = (blk + 1 == blk_end);
        bool need_clip = (is_first_block && decoded_values[0] < row_begin)
                      || (is_last_block && decoded_values[decoded_count - 1] > row_end);

        if (need_clip)
        {
            size_t begin_idx = 0;
            size_t end_idx = decoded_count;

            if (is_first_block && decoded_values[0] < row_begin)
            {
                auto it = std::lower_bound(decoded_values, decoded_values + decoded_count, static_cast<uint32_t>(row_begin));
                if (it == decoded_values + decoded_count)
                    continue;
                begin_idx = static_cast<size_t>(it - decoded_values);
            }
            if (is_last_block && decoded_values[decoded_count - 1] > row_end)
            {
                auto it_end = std::upper_bound(decoded_values, decoded_values + decoded_count, static_cast<uint32_t>(row_end));
                end_idx = static_cast<size_t>(it_end - decoded_values);
            }

            padColumnForOr(out, decoded_values, row_begin, begin_idx, end_idx);
        }
        else
        {
            /// Entire block is within [row_begin, row_end] — no clipping needed.
            padColumnForOr(out, decoded_values, row_begin, 0, decoded_count);
        }
    }
}

void PostingListCursor::linearOr(UInt8 * data, size_t row_offset, size_t num_rows)
{
    for (size_t i = current_large_block_idx; i < total_large_blocks; ++i)
    {
        auto large_block = i;
        size_t begin = info.ranges[large_block].begin;
        size_t end = info.ranges[large_block].end;

        if (row_offset > end)
            continue;

        if ((row_offset + num_rows) < begin)
            break;

        end = std::min(end, row_offset + num_rows - 1);
        prepare(large_block);
        linearOrImpl(large_block, data, row_offset, end);
    }
}

/// Scatter-increment counters in `out` for doc_ids in values[begin..length).
/// 4-wide loop with prefetch for cache-line utilization.
inline void padColumnForAnd(UInt8 * __restrict out, const uint32_t * values, size_t row_begin, size_t begin, size_t length)
{
    const uint32_t *p = values + begin;
    const uint32_t *end = values + length;

    for (; p + 4 <= end; p += 4)
    {
        __builtin_prefetch(p + 16, 0, 3);
        if (p + 8 < end)
            __builtin_prefetch(&out[p[8] - row_begin], 1, 0);

        ++out[p[0] - row_begin];
        ++out[p[1] - row_begin];
        ++out[p[2] - row_begin];
        ++out[p[3] - row_begin];
    }

    switch (end - p)
    {
        case 3: ++out[p[2] - row_begin];
            [[fallthrough]];
        case 2: ++out[p[1] - row_begin];
            [[fallthrough]];
        case 1: ++out[p[0] - row_begin];
            [[fallthrough]];
        default: break;
    }
}

void PostingListCursor::linearAndImpl(size_t large_block, UInt8 * __restrict out, size_t row_begin, size_t row_end)
{
    chassert(large_block < info.ranges.size());

    if (unlikely(is_embedded))
    {
        auto it = std::lower_bound(decoded_values, decoded_values + decoded_count, row_begin);
        if (it == decoded_values + decoded_count)
            return;
        size_t idx = static_cast<size_t>(it - decoded_values);
        auto it_end = std::upper_bound(decoded_values, decoded_values + decoded_count, row_end);
        size_t length = it_end - decoded_values;
        padColumnForAnd(out, decoded_values, row_begin, idx, length);
        return;
    }

    /// Use packed_block_last_doc_ids[] to skip blocks outside [row_begin, row_end]
    /// without decoding.  Same strategy as linearOrImpl.
    auto blk_start_it = std::lower_bound(packed_block_last_doc_ids.begin(), packed_block_last_doc_ids.end(), static_cast<UInt32>(row_begin));
    if (blk_start_it == packed_block_last_doc_ids.end())
        return;
    size_t blk_start = static_cast<size_t>(blk_start_it - packed_block_last_doc_ids.begin());

    auto blk_end_it = std::lower_bound(packed_block_last_doc_ids.begin(), packed_block_last_doc_ids.end(), static_cast<UInt32>(row_end));
    size_t blk_end;
    if (blk_end_it == packed_block_last_doc_ids.end())
        blk_end = block_count;
    else
        blk_end = static_cast<size_t>(blk_end_it - packed_block_last_doc_ids.begin()) + 1;

    current_block = blk_start;
    need_seek_before_decode = true;

    for (size_t blk = blk_start; blk < blk_end; ++blk)
    {
        current_block = blk;
        if (!decodeNextBlock())
            return;

        if (decoded_count == 0)
            continue;

        bool is_first_block = (blk == blk_start);
        bool is_last_block = (blk + 1 == blk_end);
        bool need_clip = (is_first_block && decoded_values[0] < row_begin)
                      || (is_last_block && decoded_values[decoded_count - 1] > row_end);

        if (need_clip)
        {
            size_t begin_idx = 0;
            size_t end_idx = decoded_count;

            if (is_first_block && decoded_values[0] < row_begin)
            {
                auto it = std::lower_bound(decoded_values, decoded_values + decoded_count, static_cast<uint32_t>(row_begin));
                if (it == decoded_values + decoded_count)
                    continue;
                begin_idx = static_cast<size_t>(it - decoded_values);
            }
            if (is_last_block && decoded_values[decoded_count - 1] > row_end)
            {
                auto it_end = std::upper_bound(decoded_values, decoded_values + decoded_count, static_cast<uint32_t>(row_end));
                end_idx = static_cast<size_t>(it_end - decoded_values);
            }

            padColumnForAnd(out, decoded_values, row_begin, begin_idx, end_idx);
        }
        else
        {
            padColumnForAnd(out, decoded_values, row_begin, 0, decoded_count);
        }
    }
}

void PostingListCursor::linearAnd(UInt8 * data, size_t row_offset, size_t num_rows)
{
    for (size_t i = current_large_block_idx; i < total_large_blocks; ++i)
    {
        auto large_block = i;
        size_t begin = info.ranges[large_block].begin;
        size_t end = info.ranges[large_block].end;

        if (row_offset > end)
            continue;

        if ((row_offset + num_rows) < begin)
            break;

        end = std::min(end, row_offset + num_rows - 1);
        prepare(large_block);
        linearAndImpl(large_block, data, row_offset, end);
    }
}

namespace
{

/// Element for the min-heap used in N-way intersection.
struct HeapItem
{
    uint32_t val = 0;
    uint32_t idx = 0;

    HeapItem() = default;
    HeapItem(uint32_t val_, uint32_t idx_) : val(val_), idx(idx_) {}
    bool operator>(const HeapItem & other) const { return val > other.val; }
};

/// Two-cursor intersection.  The lagging cursor seeks to the leading cursor's doc_id.
void intersectTwo(UInt8 * out, PostingListCursorPtr c0, PostingListCursorPtr c1, size_t row_offset, size_t effective_end)
{
    while (c0->valid() && c1->valid())
    {
        uint32_t v0 = c0->value();
        uint32_t v1 = c1->value();
        if (v0 >= effective_end || v1 >= effective_end)
            return;

        if (v0 == v1)
        {
            out[v0 - row_offset] = 1;
            c0->next();
            c1->next();
        }
        else if (v0 < v1)
        {
            c0->seek(v1);
        }
        else
        {
            c1->seek(v0);
        }
    }
}

/// Three-cursor intersection.  All cursors behind the maximum seek forward.
void intersectThree(UInt8 * out, PostingListCursorPtr c0, PostingListCursorPtr c1, PostingListCursorPtr c2, size_t row_offset, size_t effective_end)
{
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    while (c0->valid() && c1->valid() && c2->valid())
    {
        v0 = c0->value();
        v1 = c1->value();
        v2 = c2->value();

        uint32_t max_val = std::max({v0, v1, v2});
        if (max_val >= effective_end)
            return;

        if (v0 == v1 && v1 == v2)
        {
            out[v0 - row_offset] = 1;

            c0->next();
            c1->next();
            c2->next();

        }
        else
        {
            if (v0 < max_val)
                c0->seek(max_val);
            if (v1 < max_val)
                c1->seek(max_val);
            if (v2 < max_val)
                c2->seek(max_val);
        }
    }
}

/// Four-cursor intersection.
void intersectFour(UInt8 * out, PostingListCursorPtr c0, PostingListCursorPtr c1, PostingListCursorPtr c2, PostingListCursorPtr c3, size_t row_offset, size_t effective_end)
{
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    uint32_t v3 = 0;
    while (c0->valid() && c1->valid() && c2->valid() && c3->valid())
    {
        v0 = c0->value();
        v1 = c1->value();
        v2 = c2->value();
        v3 = c3->value();

        uint32_t max_val = std::max({v0, v1, v2, v3});
        if (max_val >= effective_end)
            return;

        if (v0 == v1 && v1 == v2 && v2 == v3)
        {
            out[v0 - row_offset] = 1;

            c0->next();
            c1->next();
            c2->next();
            c3->next();
        }
        else
        {
            if (v0 < max_val)
                c0->seek(max_val);
            if (v1 < max_val)
                c1->seek(max_val);
            if (v2 < max_val)
                c2->seek(max_val);
            if (v3 < max_val)
                c3->seek(max_val);
        }
    }
}

/// N-way leapfrog intersection (N <= 8).
/// Uses a linear scan over cursor values to find min/max each round.
void intersectLeapfrogLinear(UInt8 * out, const std::vector<PostingListCursorPtr> & cursors, size_t row_offset, size_t effective_end)
{
    const size_t n = cursors.size();
    std::vector<uint32_t> vals(n);
    for (size_t i = 0; i < n; ++i)
    {
        vals[i] = cursors[i]->value();
    }

    while (true)
    {
        uint32_t min_val = vals[0];
        uint32_t max_val = vals[0];
        size_t min_idx = 0;

        for (size_t i = 1; i < n; ++i)
        {
            if (vals[i] < min_val)
            {
                min_val = vals[i];
                min_idx = i;
            }
            else if (vals[i] > max_val)
            {
                max_val = vals[i];
            }
        }

        if (max_val >= effective_end)
            return;

        if (min_val == max_val)
        {
            out[min_val - row_offset] = 1;
            for (size_t i = 0; i < n; ++i)
            {
                cursors[i]->next();
                if (!cursors[i]->valid())
                    return;
                vals[i] = cursors[i]->value();
            }
        }
        else
        {
            cursors[min_idx]->seek(max_val);
            if (!cursors[min_idx]->valid())
                return;
            vals[min_idx] = cursors[min_idx]->value();
        }
    }
}

/// N-way leapfrog intersection (N > 8).
/// Uses a min-heap to efficiently extract the minimum cursor each round.
void intersectLeapfrogHeap(UInt8 * out, const std::vector<PostingListCursorPtr> & cursors, size_t row_offset, size_t effective_end)
{
    const size_t n = cursors.size();

    std::vector<HeapItem> heap(n);
    uint32_t max_val = 0;

    for (size_t i = 0; i < n; ++i)
    {
        uint32_t val = cursors[i]->value();
        heap[i] = {val, static_cast<uint32_t>(i)};
        max_val = std::max(max_val, val);
    }
    std::make_heap(heap.begin(), heap.end(), std::greater<>{});

    while (true)
    {
        if (max_val >= effective_end)
            return;

        uint32_t min_val = heap.front().val;

        if (min_val == max_val)
        {
            out[min_val - row_offset] = 1;

            max_val = 0;
            size_t heap_size = n;

            for (size_t i = 0; i < heap_size; ++i)
            {
                uint32_t idx = heap[i].idx;
                cursors[idx]->next();

                if (!cursors[idx]->valid())
                    return;

                uint32_t val = cursors[idx]->value();
                if (val >= effective_end)
                    return;

                heap[i].val = val;
                max_val = std::max(max_val, val);
            }
            std::make_heap(heap.begin(), heap.end(), std::greater<>{});
        }
        else
        {
            uint32_t min_idx = heap.front().idx;
            std::pop_heap(heap.begin(), heap.end(), std::greater<>{});

            cursors[min_idx]->seek(max_val);
            if (!cursors[min_idx]->valid())
                return;

            uint32_t new_val = cursors[min_idx]->value();
            if (new_val >= effective_end)
                return;

            max_val = std::max(max_val, new_val);
            heap.back() = {new_val, min_idx};
            std::push_heap(heap.begin(), heap.end(), std::greater<>{});
        }
    }
}

/// Dispatch to the best leapfrog variant based on cursor count:
///   2 → unrolled two-cursor,  3 → three-cursor,  4 → four-cursor,
///   5..8 → linear scan,  >8 → min-heap.
void intersectLeapfrog(UInt8 * out, const std::vector<PostingListCursorPtr> & cursors, size_t row_offset, size_t effective_end)
{
    if (cursors.size() == 2)
    {
        intersectTwo(out, cursors[0], cursors[1], row_offset, effective_end);
        return;
    }

    if (cursors.size() == 3)
    {
        intersectThree(out, cursors[0], cursors[1], cursors[2], row_offset, effective_end);
        return;
    }

    if (cursors.size() == 4)
    {
        intersectFour(out, cursors[0], cursors[1], cursors[2], cursors[3], row_offset, effective_end);
        return;
    }

    if (cursors.size() <= 8)
    {
        intersectLeapfrogLinear(out, cursors, row_offset, effective_end);
        return;
    }

    intersectLeapfrogHeap(out, cursors, row_offset, effective_end);
}

/// Brute-force intersection via bitmap counting.
/// First cursor sets bits (linearOr), remaining cursors increment counters (linearAnd),
/// then a final pass converts count == n into 1, everything else into 0.
void intersectBruteForce(UInt8 * out, const std::vector<PostingListCursorPtr> & cursors, size_t row_offset, size_t num_rows)
{
    cursors[0]->linearOr(out, row_offset, num_rows);

    for (size_t i = 1; i < cursors.size(); ++i)
        cursors[i]->linearAnd(out, row_offset, num_rows);

    size_t n = cursors.size();
    if (n > 1)
    {
        UInt8 * p = out;
        UInt8 * end = out + num_rows;
        UInt8 * end_loop = out + (num_rows / 4) * 4;
        UInt8 n8 = static_cast<UInt8>(n);

        for (; p < end_loop; p += 4)
        {
            __builtin_prefetch(p + 64, 0, 3);
            __builtin_prefetch(p + 64, 1, 0);

            p[0] = (p[0] == n8);
            p[1] = (p[1] == n8);
            p[2] = (p[2] == n8);
            p[3] = (p[3] == n8);
        }

        while (p < end)
        {
            *p = (*p == n8);
            ++p;
        }
    }
}

} // anonymous namespace

void lazyUnionPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool /*brute_force_apply*/, float /*density_threshold*/)
{
    auto & data = assert_cast<DB::ColumnUInt8 &>(column).getData();
    UInt8 * out = data.data() + column_offset;

    std::vector<PostingListCursorPtr> cursors;
    cursors.reserve(postings.size());
    for (const auto & token : search_tokens)
    {
        auto it = postings.find(token);
        if (it != postings.end())
            cursors.emplace_back(it->second);
    }
    for (const auto & cursor : cursors)
        cursor->linearOr(out, row_offset, num_rows);
}

void lazyIntersectPostingLists(IColumn & column, const PostingListCursorMap & postings, const std::vector<String> & search_tokens, size_t column_offset, size_t row_offset, size_t num_rows, bool brute_force_apply, float density_threshold)
{
    auto & data = assert_cast<DB::ColumnUInt8 &>(column).getData();
    UInt8 * __restrict out = data.data() + column_offset;

    std::vector<PostingListCursorPtr> cursors;
    cursors.reserve(postings.size());
    for (const auto & token : search_tokens)
    {
        auto it = postings.find(token);
        if (it != postings.end())
            cursors.emplace_back(it->second);
    }
    const size_t n = cursors.size();
    const size_t end = row_offset + num_rows;

    if (n == 0)
        return;

    if (n == 1)
    {
        cursors.front()->linearOr(out, row_offset, num_rows);
        return;
    }

    /// Algorithm selection uses the MINIMUM density across all cursors:
    /// brute-force only wins when ALL lists are dense.  A single sparse cursor
    /// makes leapfrog more efficient because it can skip large unused ranges.
    double min_density = std::numeric_limits<double>::max();
    for (size_t i = 0; i < n; ++i)
        min_density = std::min(min_density, cursors[i]->density());

    if (n < 256 && (min_density >= density_threshold || brute_force_apply))
    {
        intersectBruteForce(out, cursors, row_offset, num_rows);
        return;
    }

    /// Sort cursors by ascending cardinality so the sparsest cursor leads
    /// the leapfrog.  The sparse leader advances in large jumps while dense
    /// followers catch up cheaply via seek.
    std::sort(cursors.begin(), cursors.end(),
        [](const PostingListCursorPtr & a, const PostingListCursorPtr & b)
        { return a->cardinality() < b->cardinality(); });

    for (size_t i = 0; i < n; ++i)
    {
        cursors[i]->seek(static_cast<uint32_t>(row_offset));
        if (!cursors[i]->valid() || cursors[i]->value() >= end)
            return;
    }

    intersectLeapfrog(out, cursors, row_offset, end);
}

}
