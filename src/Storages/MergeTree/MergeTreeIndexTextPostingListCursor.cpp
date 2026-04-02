#include <Storages/MergeTree/MergeTreeIndexTextPostingListCursor.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Storages/MergeTree/ProjectionIndex/LengthPrefixedInt.h>
#include <Common/ProfileEvents.h>
#include <Common/TargetSpecific.h>
#include <algorithm>
#include <cstring>

#if USE_MULTITARGET_CODE
#include <immintrin.h>
#endif

#include <turbopfor.h>

namespace ProfileEvents
{
    extern const Event TextIndexLazyPackedBlocksDecoded;
    extern const Event TextIndexLazyPackedBlocksSkipped;
    extern const Event TextIndexLazySeekCount;
    extern const Event TextIndexLazyLargeBlocksPrepared;
    extern const Event TextIndexLazyBruteForceIntersections;
    extern const Event TextIndexLazyLeapfrogIntersections;
    extern const Event TextIndexLazyLargeBlocksSkippedDense;
    extern const Event TextIndexLazyLargeBlocksSkippedCovered;
    extern const Event TextIndexLazyPackedBlocksSkippedCovered;
}

namespace DB
{

namespace
{

/// Convenience alias for the shared prefix-varint codec.
inline void readPrefixVarUInt32(UInt32 & x, ReadBuffer & istr)
{
    LengthPrefixedInt::readUInt32(x, istr);
}

} // anonymous namespace

PostingListCursor::PostingListCursor(LargePostingListReaderStreamPtr owned_stream_, const TokenPostingsInfo & info_)
    : stream(owned_stream_.get())
    , owned_stream(std::move(owned_stream_))
    , info(info_)
    , total_large_blocks(info.offsets.size())
{
    /// Compute global density once: cardinality / total_range_span.
    if (!info.ranges.empty())
    {
        UInt32 global_begin = static_cast<UInt32>(info.ranges.front().begin);
        UInt32 global_end = static_cast<UInt32>(info.ranges.back().end);
        UInt32 range_span = global_end - global_begin + 1;
        density_val = (range_span > 0) ? static_cast<double>(info.cardinality) / static_cast<double>(range_span) : 1.0;
    }
    else
        density_val = 1.0;

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
    /// Skip if this large block is already loaded.
    if (has_prepared_first_large_block && large_block_idx == current_large_block_idx)
        return;

    ProfileEvents::increment(ProfileEvents::TextIndexLazyLargeBlocksPrepared);
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

    /// Bulk-read the entire Data Section into memory.
    /// Data Section spans [block_meta.offset, block_meta.index_offset).
    /// This eliminates per-packed-block seeks in `probeAndDecodePackedBlock`.
    {
        UInt64 data_start = block_meta.offset;
        UInt64 data_end = block_meta.index_offset;
        chassert(data_end > data_start);
        size_t data_size = static_cast<size_t>(data_end - data_start);
        data_section_buffer.resize(data_size);
        data_section_base_offset = data_start;
        stream->seek(data_start);
        auto & data_buf2 = *stream->getDataBuffer();
        data_buf2.readStrict(reinterpret_cast<char *>(data_section_buffer.data()), data_size);
    }

    if (block_count > 0 || large_block_idx == 0)
        is_valid = true;
    else
        is_valid = false;
}

bool PostingListCursor::probeAndDecodePackedBlock(size_t block_idx)
{
    chassert(block_idx < packed_block_offsets.size());

    current_block = block_idx;
    arithmetic_mode = false;

    /// Compute the delta base for TurboPFor decoding.
    uint32_t delta_base;
    if (block_idx == 0)
    {
        delta_base = static_cast<uint32_t>(info.ranges[current_large_block_idx].begin);
        if (current_large_block_idx > 0)
            --delta_base;
    }
    else
        delta_base = packed_block_last_doc_ids[block_idx - 1];

    /// For large block 0, packed block 0: the first_doc_id is prepended from
    /// the dictionary, breaking any arithmetic sequence.  Skip straight to decode.
    bool prepend_first_doc_id = (current_large_block_idx == 0 && block_idx == 0);

    /// Read from the in-memory Data Section buffer instead of seeking the stream.
    size_t buf_offset = static_cast<size_t>(packed_block_offsets[block_idx] - data_section_base_offset);
    chassert(buf_offset < data_section_buffer.size());

    const uint8_t * buf_ptr = data_section_buffer.data() + buf_offset;
    const uint8_t * buf_end = data_section_buffer.data() + data_section_buffer.size();

    /// Inline PrefixVarInt decode for the length header.
    UInt32 bytes;
    {
        const uint8_t * p = buf_ptr;
        chassert(p < buf_end);
        const uint8_t first_byte = *p++;
        if (first_byte <= 176)
            bytes = first_byte;
        else if (first_byte <= 240)
            bytes = ((first_byte - 177) << 8) + *p++ + 177;
        else if (first_byte <= 248)
        {
            bytes = ((first_byte - 241) << 16) + (p[0] << 8) + p[1] + 16561;
            p += 2;
        }
        else if (first_byte == 249)
        {
            bytes = (static_cast<UInt32>(p[0]) << 16) | (p[1] << 8) | p[2];
            p += 3;
        }
        else
        {
            bytes = (static_cast<UInt32>(p[0]) << 24) | (static_cast<UInt32>(p[1]) << 16) | (p[2] << 8) | p[3];
            p += 4;
        }
        buf_ptr = p;
    }

    UInt32 count = (block_idx + 1 == block_count && tail_size > 0)
                       ? static_cast<UInt32>(tail_size)
                       : static_cast<UInt32>(TURBOPFOR_BLOCK_SIZE);

    /// --- Arithmetic probe (only when first_doc_id prepend is not needed) ---
    if (!prepend_first_doc_id && bytes > 0)
    {
        chassert(buf_ptr + bytes <= buf_end);
        const uint8_t * payload_start = buf_ptr;
        uint8_t header_byte = *payload_start;

        uint32_t constant_value = 0;
        bool is_arithmetic = false;

        if (header_byte == 0x00)
        {
            /// All-zero block: all deltas are 0, step = 1.
            constant_value = 0;
            is_arithmetic = true;
        }
        else if ((header_byte & 0xC0u) == 0xC0u)
        {
            /// Constant block: header = 0xC0 | b.
            unsigned b = header_byte & 0x3Fu;
            unsigned bytes_stored = (b + 7u) / 8u;

            if (bytes_stored == 0)
            {
                constant_value = 0;
                is_arithmetic = true;
            }
            else
            {
                const uint8_t * cv_payload = payload_start + 1;
                constant_value = 0;
                for (unsigned i = 0; i < bytes_stored && i < 4; ++i)
                    constant_value |= static_cast<uint32_t>(cv_payload[i]) << (8u * i);
                if (b < 32u)
                    constant_value &= (1u << b) - 1u;
                is_arithmetic = true;
            }
        }

        if (is_arithmetic)
        {
            ProfileEvents::increment(ProfileEvents::TextIndexLazyPackedBlocksSkipped);
            arithmetic_mode = true;
            arithmetic_step = constant_value + 1;
            arithmetic_first = delta_base + arithmetic_step;
            arithmetic_count = count;
            decoded_count = count;
            index = 0;

            return true;
        }

        /// Non-arithmetic block — decode from the in-memory buffer.
        uint8_t * src_ptr = const_cast<uint8_t *>(payload_start);

        last_decoded_doc_id = delta_base;
        UInt32 actual_count = count;
        decoded_count = actual_count;
        uint32_t * decode_dst = decoded_values;

        if (count == TURBOPFOR_BLOCK_SIZE)
            turbopfor::p4D1Dec128v32(src_ptr, TURBOPFOR_BLOCK_SIZE, decode_dst, last_decoded_doc_id);
        else
            turbopfor::p4D1Dec32(src_ptr, count, decode_dst, last_decoded_doc_id);

        last_decoded_doc_id = decoded_values[actual_count - 1];
        index = 0;

        ProfileEvents::increment(ProfileEvents::TextIndexLazyPackedBlocksDecoded);
        return false;
    }

    /// --- Direct decode path (prepend_first_doc_id or bytes == 0) ---
    {
        chassert(buf_ptr + bytes <= buf_end);
        uint8_t * src_ptr = const_cast<uint8_t *>(buf_ptr);

        UInt32 actual_count = prepend_first_doc_id ? count + 1 : count;
        decoded_count = actual_count;
        uint32_t * decode_dst = decoded_values + (prepend_first_doc_id ? 1 : 0);

        last_decoded_doc_id = delta_base;

        if (count == TURBOPFOR_BLOCK_SIZE)
            turbopfor::p4D1Dec128v32(src_ptr, TURBOPFOR_BLOCK_SIZE, decode_dst, last_decoded_doc_id);
        else
            turbopfor::p4D1Dec32(src_ptr, count, decode_dst, last_decoded_doc_id);

        if (prepend_first_doc_id)
            decoded_values[0] = static_cast<uint32_t>(info.ranges[0].begin);

        last_decoded_doc_id = decoded_values[actual_count - 1];
        index = 0;

        ProfileEvents::increment(ProfileEvents::TextIndexLazyPackedBlocksDecoded);
        return false;
    }
}

void PostingListCursor::seek(uint32_t target)
{
    ProfileEvents::increment(ProfileEvents::TextIndexLazySeekCount);

    /// When a reader is reused across read tasks, the target may be before
    /// the currently loaded large block.  Reset to scan from the beginning.
    if (current_large_block_idx > 0
        && current_large_block_idx < total_large_blocks
        && target < info.ranges[current_large_block_idx].begin)
    {
        current_large_block_idx = 0;
        has_prepared_first_large_block = false;
    }

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
    if (unlikely(is_embedded))
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

    /// Check if target falls within the already-decoded/active packed block.
    if (index < decoded_count)
    {
        if (arithmetic_mode)
        {
            /// Arithmetic block: compute position directly.
            uint32_t last_in_block = arithmetic_first + (static_cast<uint32_t>(decoded_count) - 1) * arithmetic_step;
            if (target <= last_in_block)
            {
                if (target <= arithmetic_first)
                {
                    index = 0;
                    return true;
                }
                uint32_t offset_from_first = target - arithmetic_first;
                uint32_t idx = offset_from_first / arithmetic_step;
                uint32_t actual = arithmetic_first + idx * arithmetic_step;
                if (actual < target && idx + 1 < static_cast<uint32_t>(decoded_count))
                {
                    ++idx;
                }
                index = idx;
                return true;
            }
        }
        else
        {
            auto it = std::lower_bound(decoded_values + index, decoded_values + decoded_count, target);
            if (it != decoded_values + decoded_count)
            {
                index = static_cast<size_t>(it - decoded_values);
                return true;
            }
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
        /// Probe the header and, if non-arithmetic, decode in one pass
        /// (avoids a redundant seek + length-prefix re-read).
        if (probeAndDecodePackedBlock(j))
        {
            /// Direct arithmetic computation — no decompression needed.
            /// arithmetic_mode / arithmetic_step / arithmetic_first / arithmetic_count
            /// are already populated by probeAndDecodePackedBlock.

            if (target <= arithmetic_first)
            {
                index = 0;
            }
            else
            {
                uint32_t offset_from_first = target - arithmetic_first;
                uint32_t idx = offset_from_first / arithmetic_step;
                uint32_t actual = arithmetic_first + idx * arithmetic_step;
                if (actual < target && idx + 1 < arithmetic_count)
                    ++idx;
                index = idx;
            }

            return true;
        }

        /// Non-arithmetic block already decoded by probeAndDecodePackedBlock.
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
            /// More packed blocks in this large block — probe + decode.
            probeAndDecodePackedBlock(current_block);
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
        probeAndDecodePackedBlock(0);
    }
}

enum class PadOp { Or, And };

/// Scatter-write into `out` for doc_ids in values[begin..length).
/// PadOp::Or assigns 1, PadOp::And increments the counter.
/// 4-wide loop with prefetch for cache-line utilization (~5-10% improvement
/// on dense posting list iteration in real-world benchmarks).
template <PadOp op>
inline void padColumn(UInt8 * __restrict out, const uint32_t * values, size_t row_begin, size_t begin, size_t length)
{
    const uint32_t * p = values + begin;
    const uint32_t * end = values + length;

    if (p >= end)
        return;

    const size_t count = static_cast<size_t>(end - p);

    /// Dense fill optimization for PadOp::Or: when the doc_id array covers most
    /// of its range (>= 75% fill rate), it's faster to memset the entire range
    /// to 1 and then clear the gaps, rather than scatter-writing each doc_id.
    /// For 100 out of 128 doc_ids this does memset(128) + 28 zero-writes
    /// instead of 100 scatter-writes.
    if constexpr (op == PadOp::Or)
    {
        uint32_t range_span = values[length - 1] - values[begin] + 1;
        if (count >= 32 && count * 4 >= range_span * 3)  // >= 75% fill
        {
            size_t off = values[begin] - row_begin;
            memset(out + off, 1, range_span);

            /// Clear gaps: walk the sorted array and zero out positions not present.
            /// Use two pointers: expected (sequential) vs actual (decoded doc_ids).
            uint32_t expected = values[begin];
            for (size_t i = begin; i < length; ++i)
            {
                while (expected < values[i])
                {
                    out[expected - row_begin] = 0;
                    ++expected;
                }
                expected = values[i] + 1;
            }
            return;
        }
    }

    const uint32_t * loop_end = p + (count / 4) * 4;

    for (; p < loop_end; p += 4)
    {
        __builtin_prefetch(p + 16, 0, 3);
        if (p + 8 < end)
            __builtin_prefetch(&out[p[8] - row_begin], 1, 0);

        if constexpr (op == PadOp::Or)
        {
            out[p[0] - row_begin] = 1;
            out[p[1] - row_begin] = 1;
            out[p[2] - row_begin] = 1;
            out[p[3] - row_begin] = 1;
        }
        else
        {
            ++out[p[0] - row_begin];
            ++out[p[1] - row_begin];
            ++out[p[2] - row_begin];
            ++out[p[3] - row_begin];
        }
    }

    switch (end - p)
    {
        case 3:
            if constexpr (op == PadOp::Or)
                out[p[2] - row_begin] = 1;
            else
                ++out[p[2] - row_begin];
            [[fallthrough]];
        case 2:
            if constexpr (op == PadOp::Or)
                out[p[1] - row_begin] = 1;
            else
                ++out[p[1] - row_begin];
            [[fallthrough]];
        case 1:
            if constexpr (op == PadOp::Or)
                out[p[0] - row_begin] = 1;
            else
                ++out[p[0] - row_begin];
            [[fallthrough]];
        default: break;
    }
}

/// Scatter-write arithmetic-sequence doc_ids into `out` for the range [row_begin, row_end].
/// Avoids full TurboPFor decompression for constant-delta and all-zero blocks.
template <PadOp op>
inline void padArithmeticBlock(UInt8 * __restrict out, uint32_t first_doc_id, uint32_t last_doc_id, uint32_t step, size_t row_begin, size_t row_end)
{
    uint32_t start = std::max(first_doc_id, static_cast<uint32_t>(row_begin));
    uint32_t end = std::min(last_doc_id, static_cast<uint32_t>(row_end));

    if (start > end)
        return;

    /// Fast path for step=1 (zero-delta / consecutive doc_ids) — the most common
    /// arithmetic block pattern.  Replaces N scalar stores with a single memset
    /// (Or) or a tight loop that the compiler auto-vectorizes to SIMD paddb (And).
    if (step == 1)
    {
        size_t off = start - row_begin;
        size_t count = end - start + 1;
        if constexpr (op == PadOp::Or)
        {
            memset(out + off, 1, count);
        }
        else
        {
            UInt8 * __restrict dst = out + off;
            for (size_t i = 0; i < count; ++i)
                ++dst[i];
        }
        return;
    }

    /// General case: non-unit step.
    uint32_t offset = start - first_doc_id;
    uint32_t idx = (offset + step - 1) / step;  /// ceil division
    uint32_t doc_id = first_doc_id + idx * step;

    while (doc_id <= end)
    {
        if constexpr (op == PadOp::Or)
            out[doc_id - row_begin] = 1;
        else
            ++out[doc_id - row_begin];
        doc_id += step;
    }
}

/// Check whether a byte buffer contains no zero bytes.
/// In the `linearOr` path, buffer values are only 0 or 1, so "no zeros"
/// is equivalent to "all ones" but avoids the more expensive broadcast of 1.
///
/// Called from two sites with different typical sizes:
///   - Packed block check (Level 2b): 128 bytes (TURBOPFOR_BLOCK_SIZE), or 1–127 for tail blocks.
///   - Large block check (Level 2a): variable, up to ~500 KB.
///
/// Uses the ClickHouse multi-target dispatch pattern:
///   - AVX2 path with 4x loop unrolling (128 bytes/iteration) when available.
///   - Scalar fallback via `memchr` on all other platforms (ARM, macOS, non-AVX2 x86).

#if USE_MULTITARGET_CODE
DECLARE_X86_64_V3_SPECIFIC_CODE(
inline bool hasNoZeros(const UInt8 * data, size_t count)
{
    const UInt8 * p = data;
    const UInt8 * end = data + count;
    const __m256i zero = _mm256_setzero_si256();

    /// Phase 1: Process 128 bytes (4 x 32) per iteration.
    /// OR-accumulate zero-match masks and check once per 128-byte chunk.
    while (end - p >= 128)
    {
        __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 32));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 64));
        __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 96));

        __m256i eq0 = _mm256_cmpeq_epi8(v0, zero);
        __m256i eq1 = _mm256_cmpeq_epi8(v1, zero);
        __m256i eq2 = _mm256_cmpeq_epi8(v2, zero);
        __m256i eq3 = _mm256_cmpeq_epi8(v3, zero);

        __m256i any_zero = _mm256_or_si256(_mm256_or_si256(eq0, eq1),
                                           _mm256_or_si256(eq2, eq3));
        if (_mm256_movemask_epi8(any_zero))
            return false;
        p += 128;
    }

    /// Phase 2: Handle remaining 32–127 bytes one vector at a time.
    while (end - p >= 32)
    {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero)))
            return false;
        p += 32;
    }

    /// Phase 3: Tail (< 32 bytes) — delegate to `memchr`.
    return memchr(p, 0, static_cast<size_t>(end - p)) == nullptr;
}
) /// DECLARE_X86_64_V3_SPECIFIC_CODE
#endif

inline bool hasNoZeros(const UInt8 * data, size_t count)
{
#if USE_MULTITARGET_CODE
    if (isArchSupported(TargetArch::x86_64_v3))
        return TargetSpecific::x86_64_v3::hasNoZeros(data, count);
#endif

    /// Scalar fallback: `memchr` uses platform-optimized SIMD internally
    /// (SSE/AVX on x86_64, NEON on aarch64 via glibc/bionic).
    return memchr(data, 0, count) == nullptr;
}

void PostingListCursor::linearOrImpl(size_t large_block, UInt8 * __restrict out, size_t row_begin, size_t row_end, bool skip_covered_checks)
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
        padColumn<PadOp::Or>(out, decoded_values, row_begin, begin_idx, end_idx);
        return;
    }

    /// Use packed_block_last_doc_ids[] to determine the range of packed blocks
    /// that overlap [row_begin, row_end], skipping all blocks outside this range
    /// without any I/O or decoding.
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

    /// Cache frequently accessed data on the stack to avoid repeated pointer chasing.
    const uint8_t * const data_buf = data_section_buffer.data();
    const size_t data_buf_sz = data_section_buffer.size();
    const UInt64 base_offset = data_section_base_offset;
    const UInt32 range_begin = static_cast<UInt32>(info.ranges[large_block].begin);
    const bool is_first_large_block = (current_large_block_idx == 0);

    /// Batch-decode buffer: accumulate decoded doc_ids from multiple consecutive
    /// packed blocks, then scatter them all at once.  This amortizes padColumn
    /// function-call and loop overhead, and enables the dense-fill optimization
    /// to trigger across a wider range of doc_ids.
    static constexpr size_t BATCH_BLOCKS = 16;
    static constexpr size_t BATCH_BUF_SIZE = BATCH_BLOCKS * TURBOPFOR_BLOCK_SIZE + 1;
    alignas(16) uint32_t batch_buf[BATCH_BUF_SIZE];
    size_t batch_count = 0;

    /// Flush the batch buffer: scatter all accumulated doc_ids to the output.
    auto flushBatch = [&](bool clip_start, bool clip_end)
    {
        if (batch_count == 0)
            return;

        size_t begin_idx = 0;
        size_t end_idx = batch_count;

        if (clip_start && batch_buf[0] < row_begin)
        {
            auto it = std::lower_bound(batch_buf, batch_buf + batch_count, static_cast<uint32_t>(row_begin));
            if (it == batch_buf + batch_count)
            {
                batch_count = 0;
                return;
            }
            begin_idx = static_cast<size_t>(it - batch_buf);
        }
        if (clip_end && batch_buf[batch_count - 1] > row_end)
        {
            auto it_e = std::upper_bound(batch_buf, batch_buf + batch_count, static_cast<uint32_t>(row_end));
            end_idx = static_cast<size_t>(it_e - batch_buf);
        }

        padColumn<PadOp::Or>(out, batch_buf, row_begin, begin_idx, end_idx);
        batch_count = 0;
    };

    for (size_t blk = blk_start; blk < blk_end; ++blk)
    {
        /// Level 2b — Already-covered packed block skip.
        if (!skip_covered_checks)
        {
            uint32_t pb_first = (blk == 0) ? range_begin : packed_block_last_doc_ids[blk - 1] + 1;
            uint32_t pb_last = packed_block_last_doc_ids[blk];

            uint32_t check_begin = std::max(pb_first, static_cast<uint32_t>(row_begin));
            uint32_t check_end = std::min(pb_last, static_cast<uint32_t>(row_end));
            if (check_begin <= check_end)
            {
                size_t off = check_begin - row_begin;
                size_t cnt = check_end - check_begin + 1;
                if (hasNoZeros(out + off, cnt))
                {
                    ProfileEvents::increment(ProfileEvents::TextIndexLazyPackedBlocksSkippedCovered);
                    /// Flush before skip to avoid mixing disjoint ranges.
                    /// Always clip both ends: the batch may still contain values
                    /// from the first block that are outside [row_begin, row_end].
                    flushBatch(true, true);
                    continue;
                }
            }
        }

        /// --- Inlined decode (replaces probeAndDecodePackedBlock call) ---

        /// Compute delta base.
        uint32_t delta_base;
        if (blk == 0)
        {
            delta_base = range_begin;
            if (current_large_block_idx > 0)
                --delta_base;
        }
        else
            delta_base = packed_block_last_doc_ids[blk - 1];

        bool prepend_first_doc_id = (is_first_large_block && blk == 0);

        /// Read from in-memory Data Section buffer.
        size_t buf_offset = static_cast<size_t>(packed_block_offsets[blk] - base_offset);
        chassert(buf_offset < data_buf_sz);

        const uint8_t * buf_ptr = data_buf + buf_offset;
        const uint8_t * buf_end_ptr = data_buf + data_buf_sz;

        /// Inline PrefixVarInt decode for length header.
        UInt32 bytes;
        {
            const uint8_t * p = buf_ptr;
            chassert(p < buf_end_ptr);
            const uint8_t first_byte = *p++;
            if (first_byte <= 176)
                bytes = first_byte;
            else if (first_byte <= 240)
                bytes = ((first_byte - 177) << 8) + *p++ + 177;
            else if (first_byte <= 248)
            {
                bytes = ((first_byte - 241) << 16) + (p[0] << 8) + p[1] + 16561;
                p += 2;
            }
            else if (first_byte == 249)
            {
                bytes = (static_cast<UInt32>(p[0]) << 16) | (p[1] << 8) | p[2];
                p += 3;
            }
            else
            {
                bytes = (static_cast<UInt32>(p[0]) << 24) | (static_cast<UInt32>(p[1]) << 16) | (p[2] << 8) | p[3];
                p += 4;
            }
            buf_ptr = p;
        }

        UInt32 count = (blk + 1 == block_count && tail_size > 0)
                           ? static_cast<UInt32>(tail_size)
                           : static_cast<UInt32>(TURBOPFOR_BLOCK_SIZE);

        /// --- Arithmetic probe ---
        if (!prepend_first_doc_id && bytes > 0)
        {
            chassert(buf_ptr + bytes <= buf_end_ptr);
            const uint8_t * payload_start = buf_ptr;
            uint8_t header_byte = *payload_start;

            uint32_t constant_value = 0;
            bool is_arithmetic = false;

            if (header_byte == 0x00)
            {
                constant_value = 0;
                is_arithmetic = true;
            }
            else if ((header_byte & 0xC0u) == 0xC0u)
            {
                unsigned b = header_byte & 0x3Fu;
                unsigned bytes_stored = (b + 7u) / 8u;

                if (bytes_stored == 0)
                {
                    constant_value = 0;
                    is_arithmetic = true;
                }
                else
                {
                    const uint8_t * cv_payload = payload_start + 1;
                    constant_value = 0;
                    for (unsigned i = 0; i < bytes_stored && i < 4; ++i)
                        constant_value |= static_cast<uint32_t>(cv_payload[i]) << (8u * i);
                    if (b < 32u)
                        constant_value &= (1u << b) - 1u;
                    is_arithmetic = true;
                }
            }

            if (is_arithmetic)
            {
                /// Flush batch before arithmetic (arithmetic handled separately).
                flushBatch(true, true);
                uint32_t step = constant_value + 1;
                uint32_t first = delta_base + step;
                uint32_t last_doc_id = first + (count - 1) * step;
                padArithmeticBlock<PadOp::Or>(out, first, last_doc_id, step, row_begin, row_end);
                continue;
            }

            /// Non-arithmetic: decode to batch buffer.
            uint8_t * src_ptr = const_cast<uint8_t *>(payload_start);
            uint32_t * decode_dst = batch_buf + batch_count;

            if (count == TURBOPFOR_BLOCK_SIZE)
                turbopfor::p4D1Dec128v32(src_ptr, TURBOPFOR_BLOCK_SIZE, decode_dst, delta_base);
            else
                turbopfor::p4D1Dec32(src_ptr, count, decode_dst, delta_base);
            batch_count += count;
        }
        else
        {
            /// Direct decode path (prepend_first_doc_id or bytes == 0).
            chassert(buf_ptr + bytes <= buf_end_ptr);
            uint8_t * src_ptr = const_cast<uint8_t *>(buf_ptr);

            uint32_t * decode_dst = batch_buf + batch_count + (prepend_first_doc_id ? 1 : 0);

            if (count == TURBOPFOR_BLOCK_SIZE)
                turbopfor::p4D1Dec128v32(src_ptr, TURBOPFOR_BLOCK_SIZE, decode_dst, delta_base);
            else
                turbopfor::p4D1Dec32(src_ptr, count, decode_dst, delta_base);

            if (prepend_first_doc_id)
            {
                batch_buf[batch_count] = static_cast<uint32_t>(info.ranges[0].begin);
                batch_count += count + 1;
            }
            else
            {
                batch_count += count;
            }
        }

        /// Flush when batch is full or on last block.
        if (batch_count >= BATCH_BLOCKS * TURBOPFOR_BLOCK_SIZE || blk + 1 == blk_end)
        {
            flushBatch(true, true);
        }
    }

    /// Flush any remaining values.
    flushBatch(true, true);
}

void PostingListCursor::linearOr(UInt8 * data, size_t row_offset, size_t num_rows, bool skip_covered_checks)
{
    if (num_rows == 0)
        return;

    /// When a reader is reused across read tasks (same data part, different mark
    /// ranges), the new task's rows may start BEFORE the position we left off.
    /// Reset the scan start so we don't skip large blocks that cover earlier rows.
    /// Must also reset has_prepared_first_large_block so that prepare() re-reads the
    /// Index Section for the rewound large block (otherwise it returns early thinking
    /// the block is already loaded, but the packed_block metadata is stale).
    if (current_large_block_idx > 0
        && current_large_block_idx < total_large_blocks
        && row_offset < info.ranges[current_large_block_idx].begin)
    {
        current_large_block_idx = 0;
        has_prepared_first_large_block = false;
    }

    for (size_t i = current_large_block_idx; i < total_large_blocks; ++i)
    {
        auto large_block = i;
        size_t lb_begin = info.ranges[large_block].begin;
        size_t lb_end = info.ranges[large_block].end;

        if (row_offset > lb_end)
            continue;

        if ((row_offset + num_rows) < lb_begin)
            break;

        size_t end = std::min(lb_end, row_offset + num_rows - 1);

        /// Compute the clipped region that overlaps [row_offset, row_offset+num_rows)
        /// and [lb_begin, lb_end].  Used by both skip checks below.
        size_t clip_begin = std::max(lb_begin, row_offset);
        size_t clip_end = end;  /// already min'd above
        size_t clip_off = clip_begin - row_offset;
        size_t clip_count = clip_end - clip_begin + 1;

        /// Level 1 — Dense large block memset: if every row in the large block's
        /// range has a posting, we can memset the overlapping region directly,
        /// skipping `prepare` (Index Section I/O) and all packed block processing.
        {
            size_t actual_doc_count = info.offsets[large_block].block_doc_count;
            /// For large block 0 (non-embedded), `block_doc_count` excludes the
            /// first_doc_id which is stored inline in the dictionary.  The range
            /// however covers [first_doc_id, last_doc_id], so add 1.
            if (large_block == 0 && !is_embedded)
                actual_doc_count += 1;

            size_t range_span = lb_end - lb_begin + 1;
            if (actual_doc_count == range_span)
            {
                memset(data + clip_off, 1, clip_count);
                ProfileEvents::increment(ProfileEvents::TextIndexLazyLargeBlocksSkippedDense);
                continue;
            }
        }

        /// Level 2a — Already-covered large block skip: if a previous cursor
        /// already set the entire overlapping region to 1, skip this large block
        /// entirely (including Index Section I/O from `prepare`).
        /// Skipped when the caller guarantees the output is freshly zeroed.
        if (!skip_covered_checks && hasNoZeros(data + clip_off, clip_count))
        {
            ProfileEvents::increment(ProfileEvents::TextIndexLazyLargeBlocksSkippedCovered);
            continue;
        }

        prepare(large_block);
        linearOrImpl(large_block, data, row_offset, end, skip_covered_checks);
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
        padColumn<PadOp::And>(out, decoded_values, row_begin, idx, length);
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

    for (size_t blk = blk_start; blk < blk_end; ++blk)
    {
        /// Probe the header; if non-arithmetic, decode in one pass.
        if (probeAndDecodePackedBlock(blk))
        {
            uint32_t last_doc_id = arithmetic_first + (arithmetic_count - 1) * arithmetic_step;
            padArithmeticBlock<PadOp::And>(out, arithmetic_first, last_doc_id, arithmetic_step, row_begin, row_end);
            continue;
        }

        /// Non-arithmetic block already decoded by probeAndDecodePackedBlock.

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

            padColumn<PadOp::And>(out, decoded_values, row_begin, begin_idx, end_idx);
        }
        else
        {
            padColumn<PadOp::And>(out, decoded_values, row_begin, 0, decoded_count);
        }
    }
}

void PostingListCursor::linearAnd(UInt8 * data, size_t row_offset, size_t num_rows)
{
    /// Same backward-seek guard as linearOr — see comment there.
    if (current_large_block_idx > 0
        && current_large_block_idx < total_large_blocks
        && row_offset < info.ranges[current_large_block_idx].begin)
    {
        current_large_block_idx = 0;
        has_prepared_first_large_block = false;
    }

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

        for (size_t i = 1; i < n; ++i)
        {
            if (vals[i] < min_val)
                min_val = vals[i];
            else if (vals[i] > max_val)
                max_val = vals[i];
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
            for (size_t i = 0; i < n; ++i)
            {
                if (vals[i] < max_val)
                {
                    cursors[i]->seek(max_val);
                    if (!cursors[i]->valid())
                        return;
                    vals[i] = cursors[i]->value();
                }
            }
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
    cursors[0]->linearOr(out, row_offset, num_rows, /*skip_covered_checks=*/ true);

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

    /// Sort by descending density so the densest cursor fills the output buffer
    /// first.  Subsequent cursors benefit from `hasNoZeros` short-circuiting:
    /// already-covered regions are skipped without I/O or decoding.
    std::stable_sort(cursors.begin(), cursors.end(),
        [](const PostingListCursorPtr & a, const PostingListCursorPtr & b)
        { return a->density() > b->density(); });

    /// The first cursor writes to a freshly zeroed buffer, so `hasNoZeros`
    /// covered-region checks are guaranteed to return false — skip them.
    for (size_t i = 0; i < cursors.size(); ++i)
        cursors[i]->linearOr(out, row_offset, num_rows, /*skip_covered_checks=*/ i == 0);
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
        cursors.front()->linearOr(out, row_offset, num_rows, /*skip_covered_checks=*/ true);
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
        ProfileEvents::increment(ProfileEvents::TextIndexLazyBruteForceIntersections);
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

    ProfileEvents::increment(ProfileEvents::TextIndexLazyLeapfrogIntersections);
    intersectLeapfrog(out, cursors, row_offset, end);
}

}
