#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>

#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <Storages/MergeTree/MergeTreeDataPartWriterOnDisk.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergedPartOffsets.h>
#include <Common/Arena.h>
#include <Common/Exception.h>

#include <vp4.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace VarInt
{

void throwReadAfterEOF()
{
    throw Exception(ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF, "Attempt to read after eof");
}

inline UInt32 readVarUInt32(const uint8_t *& istr)
{
    const UInt8 first_byte = *istr++;

    if (first_byte <= 176)
        return first_byte;

    const UInt8 second_byte = *istr++;

    if (first_byte <= 240)
        return ((first_byte - 177) << 8) + second_byte + 177;

    const UInt8 third_byte = *istr++;

    if (first_byte <= 248)
        return ((first_byte - 241) << 16) + (second_byte << 8) + third_byte + 16561;

    const UInt8 fourth_byte = *istr++;

    if (first_byte == 249)
        return (second_byte << 16) | (third_byte << 8) | fourth_byte;

    const UInt8 fifth_byte = *istr++;

    return (UInt32(second_byte) << 24) | (UInt32(third_byte) << 16) | (UInt32(fourth_byte) << 8) | fifth_byte;
}

template <bool check_eof>
inline void readVarUInt32Impl(UInt32 & x, ReadBuffer & istr)
{
    /// First byte determines the encoding format
    if constexpr (check_eof)
        if (istr.eof()) [[unlikely]]
            throwReadAfterEOF();

    const UInt8 first_byte = *istr.position()++;

    if (first_byte <= 176)
    {
        /// Single byte encoding
        x = first_byte;
        return;
    }

    /// Multi-byte encoding - check if we have enough data
    if constexpr (check_eof)
        if (istr.eof()) [[unlikely]]
            throwReadAfterEOF();

    const UInt8 second_byte = *istr.position()++;

    if (first_byte <= 240)
    {
        /// Two-byte encoding
        x = ((first_byte - 177) << 8) + second_byte + 177;
        return;
    }

    /// Three or more bytes - check if we have enough data
    if constexpr (check_eof)
        if (istr.eof()) [[unlikely]]
            throwReadAfterEOF();

    const UInt8 third_byte = *istr.position()++;

    if (first_byte <= 248)
    {
        /// Three-byte encoding
        x = ((first_byte - 241) << 16) + (second_byte << 8) + third_byte + 16561;
        return;
    }

    /// Four or five bytes - check if we have enough data
    if constexpr (check_eof)
        if (istr.eof()) [[unlikely]]
            throwReadAfterEOF();

    const UInt8 fourth_byte = *istr.position()++;

    if (first_byte == 249)
    {
        /// Four-byte encoding
        x = (second_byte << 16) | (third_byte << 8) | fourth_byte;
        return;
    }

    /// Five-byte encoding - check if we have enough data
    if constexpr (check_eof)
        if (istr.eof()) [[unlikely]]
            throwReadAfterEOF();

    const UInt8 fifth_byte = *istr.position()++;

    /// Five-byte encoding
    x = (UInt32(second_byte) << 24) | (UInt32(third_byte) << 16) | (UInt32(fourth_byte) << 8) | fifth_byte;
}

template <bool check_eof>
inline void writeVarUInt32Impl(UInt32 x, WriteBuffer & ostr)
{
    /// Choose encoding based on the value range
    if (x <= 176)
    {
        /// Single byte encoding
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(x);
        ++ostr.position();
        return;
    }

    if (x <= 16560)
    {
        /// Two-byte encoding
        x -= 177;
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(177 + (x >> 8));
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(x & 0xFF);
        ++ostr.position();
        return;
    }

    if (x <= 540848)
    {
        /// Three-byte encoding
        x -= 16561;
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(241 + (x >> 16));
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>((x >> 8) & 0xFF);
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(x & 0xFF);
        ++ostr.position();
        return;
    }

    if (x <= 16777215)
    {
        /// Four-byte encoding
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(249);
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>((x >> 16) & 0xFF);
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>((x >> 8) & 0xFF);
        ++ostr.position();
        if constexpr (check_eof)
            ostr.nextIfAtEnd();
        *ostr.position() = static_cast<uint8_t>(x & 0xFF);
        ++ostr.position();
        return;
    }

    /// Five-byte encoding
    if constexpr (check_eof)
        ostr.nextIfAtEnd();
    *ostr.position() = static_cast<uint8_t>(250);
    ++ostr.position();
    if constexpr (check_eof)
        ostr.nextIfAtEnd();
    *ostr.position() = static_cast<uint8_t>((x >> 24) & 0xFF);
    ++ostr.position();
    if constexpr (check_eof)
        ostr.nextIfAtEnd();
    *ostr.position() = static_cast<uint8_t>((x >> 16) & 0xFF);
    ++ostr.position();
    if constexpr (check_eof)
        ostr.nextIfAtEnd();
    *ostr.position() = static_cast<uint8_t>((x >> 8) & 0xFF);
    ++ostr.position();
    if constexpr (check_eof)
        ostr.nextIfAtEnd();
    *ostr.position() = static_cast<uint8_t>(x & 0xFF);
    ++ostr.position();
}

inline void readVarUInt32(UInt32 & x, ReadBuffer & istr)
{
    if (istr.available() >= 5)
        readVarUInt32Impl<false>(x, istr);
    else
        readVarUInt32Impl<true>(x, istr);
}

inline void writeVarUInt32(UInt32 x, WriteBuffer & ostr)
{
    if (ostr.available() >= 5)
        writeVarUInt32Impl<false>(x, ostr);
    else
        writeVarUInt32Impl<true>(x, ostr);
}

}

void PostingListChunk::write(WriteBuffer & wb) const
{
    wb.write(reinterpret_cast<const char *>(data()), len);
}

void PostingListWriter::add(UInt32 doc_id, Arena * arena, uint8_t * packed_buffer)
{
    if (doc_count == 0)
    {
        first_doc_id = doc_id;
        last_doc_id = doc_id;
        ++doc_count;
        return;
    }

    if (doc_id < last_doc_id)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Received out of order doc id. doc_id = {}, last_doc_id = {}", doc_id, last_doc_id);

    if (doc_id == last_doc_id)
        return;

    /// TODO(amos): Arena reallocation here causes memory waste, because the old buffer cannot be reclaimed or reused.
    /// We may replace it with a small-size bucket / buddy-style allocator to reuse buffers and reduce realloc + copy
    /// overhead.
    switch (doc_count)
    {
        case 1:
            doc_delta_buffer = reinterpret_cast<UInt32 *>(arena->alignedAlloc(4 * 4, 4));
            break;
        case 5:
            doc_delta_buffer
                = reinterpret_cast<UInt32 *>(arena->alignedRealloc(reinterpret_cast<char *>(doc_delta_buffer), 4 * 4, 8 * 4, 4));
            break;
        case 9:
            doc_delta_buffer
                = reinterpret_cast<UInt32 *>(arena->alignedRealloc(reinterpret_cast<char *>(doc_delta_buffer), 8 * 4, 16 * 4, 4));
            break;
        case 17:
            doc_delta_buffer
                = reinterpret_cast<UInt32 *>(arena->alignedRealloc(reinterpret_cast<char *>(doc_delta_buffer), 16 * 4, 32 * 4, 4));
            break;
        case 33:
            doc_delta_buffer
                = reinterpret_cast<UInt32 *>(arena->alignedRealloc(reinterpret_cast<char *>(doc_delta_buffer), 32 * 4, 64 * 4, 4));
            break;
        case 65:
            doc_delta_buffer
                = reinterpret_cast<UInt32 *>(arena->alignedRealloc(reinterpret_cast<char *>(doc_delta_buffer), 64 * 4, 128 * 4, 16));
            break;
        default:
            break;
    }

    UInt8 doc_buffer_up_to = (doc_count - 1) % 128;
    UInt32 doc_delta = doc_id - last_doc_id - 1;
    doc_delta_buffer[doc_buffer_up_to] = doc_delta;

    last_doc_id = doc_id;
    ++doc_buffer_up_to;
    ++doc_count;

    if (doc_buffer_up_to == 128)
    {
        uint8_t * packed_buffer_end = p4enc128v32(doc_delta_buffer, 128, packed_buffer);
        size_t len = static_cast<UInt32>(packed_buffer_end - packed_buffer);
        chassert(len <= 512);
        auto * place = arena->alignedAlloc(len + sizeof(PostingListChunk), alignof(PostingListChunk));
        PostingListChunk * cur_block = new (place) PostingListChunk(last_doc_id, len);
        memcpy(cur_block->data(), packed_buffer, len);
        if (!blocks_head)
            blocks_head = cur_block;
        else
            *blocks_tail = cur_block;
        blocks_tail = &cur_block->next;
    }
}

class LargePostingBlockWriter
{
public:
    LargePostingBlockWriter(WriteBuffer & meta_out_, WriteBuffer & data_out_, UInt32 docs_per_large_block_)
        : meta_out(meta_out_)
        , data_out(data_out_)
        , docs_per_large_block(docs_per_large_block_)
        , current_block_offset(data_out.count())
    {
    }

    void addBlock(UInt32 last_doc_id, const char * data, UInt32 bytes)
    {
        VarInt::writeVarUInt32(bytes, data_out);
        data_out.write(data, bytes);

        docs_in_current_block += 128;
        current_block_last_doc_id = last_doc_id;

        if (docs_in_current_block >= docs_per_large_block)
            flushLargeBlock();
    }

    void finish(UInt32 num_large_blocks_expected)
    {
        if (docs_in_current_block > 0)
            flushLargeBlock();

        chassert(num_large_blocks_written == num_large_blocks_expected);
    }

private:
    void flushLargeBlock()
    {
        VarInt::writeVarUInt32(current_block_last_doc_id, meta_out);
        writeVarUInt(current_block_offset, meta_out);

        current_block_offset = data_out.count();
        docs_in_current_block = 0;
        ++num_large_blocks_written;
    }

    WriteBuffer & meta_out;
    WriteBuffer & data_out;

    UInt32 docs_per_large_block;
    UInt32 docs_in_current_block = 0;
    UInt32 current_block_last_doc_id = 0;

    UInt64 current_block_offset;
    UInt32 num_large_blocks_written = 0;
};

void PostingListWriter::finish(
    WriteBuffer & wb, WriteBuffer & large_posting, uint8_t * packed_buffer, const MergeTreeIndexTextParams & index_params) const
{
    VarInt::writeVarUInt32(doc_count, wb);
    if (doc_count == 0)
        return;

    VarInt::writeVarUInt32(first_doc_id, wb);

    /// Single doc: nothing more to write
    if (doc_count == 1)
        return;

    /// Very small posting list:
    /// inline encode all doc deltas directly into wb
    if (doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
    {
        uint8_t * packed_buffer_end = p4enc32(doc_delta_buffer, doc_count - 1, packed_buffer);
        UInt32 len = static_cast<UInt32>(packed_buffer_end - packed_buffer);
        VarInt::writeVarUInt32(len, wb);
        wb.write(reinterpret_cast<const char *>(packed_buffer), len);
        return;
    }

    /// PostingList Format
    /// --------------------------------------------
    /// Large posting list mode
    ///
    /// wb:
    ///   doc_count
    ///   first_doc_id
    ///   num_large_blocks
    ///   [last_doc_id, offset] * N
    ///
    /// large_posting:
    ///   Packed128Block #0
    ///   Packed128Block #1
    ///   ...
    /// --------------------------------------------

    /// Align posting_list_block_size up to 128 docs, so that each large block
    /// consists of an integral number of packed-128 blocks.
    const UInt32 docs_per_large_block = (index_params.posting_list_block_size + 127) & ~127;

    /// The first document is stored inline, so only (doc_count - 1) documents
    /// are written into the large_posting stream.
    const UInt32 large_doc_count = doc_count - 1;

    /// Total number of large blocks in large_posting, computed as ceil division.
    const UInt32 num_large_blocks = (large_doc_count + docs_per_large_block - 1) / docs_per_large_block;

    chassert(num_large_blocks >= 1);
    VarInt::writeVarUInt32(num_large_blocks, wb);

    LargePostingBlockWriter block_writer(wb, large_posting, docs_per_large_block);

    /// Iterate packed 128-doc chunks
    PostingListChunk * it = blocks_head;
    while (it != nullptr)
    {
        block_writer.addBlock(it->last_doc_id, reinterpret_cast<const char *>(it->data()), it->len);
        it = it->next;
    }

    /// Tail packed block (large_doc_count % 128)
    UInt8 doc_buffer_up_to = large_doc_count % 128;
    if (doc_buffer_up_to > 0)
    {
        uint8_t * packed_buffer_end = p4enc32(doc_delta_buffer, doc_buffer_up_to, packed_buffer);
        UInt32 len = static_cast<UInt32>(packed_buffer_end - packed_buffer);
        block_writer.addBlock(last_doc_id, reinterpret_cast<const char *>(packed_buffer), len);
    }

    block_writer.finish(num_large_blocks);
}

ReaderStreamEntry::ReaderStreamEntry(LargePostingListReaderStreamPtr stream_, UInt32 first_doc_id_, UInt32 doc_count_, UInt64 offset_)
    : stream(std::move(stream_))
    , first_doc_id(first_doc_id_)
    , doc_count(doc_count_)
    , offset(offset_)
{
    chassert(stream);
}

ReaderStreamVector::ReaderStreamVector(LargePostingListReaderStreamPtr stream, UInt32 first_doc_id, UInt32 doc_count, UInt64 offset)
    : entries({{std::move(stream), first_doc_id, doc_count, offset}})
{
}

void ReaderStreamVector::add(LargePostingListReaderStreamPtr stream, UInt32 first_doc_id, UInt32 doc_count, UInt64 offset)
{
    for (const auto & e : entries)
    {
        if (e.stream.get() == stream.get())
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Duplicate LargePostingListReaderStream detected in merge");
    }

    entries.emplace_back(std::move(stream), first_doc_id, doc_count, offset);
}

struct ReaderStreamCursor
{
    LargePostingListReaderStreamPtr stream;

    UInt32 * doc_buffer;
    UInt32 last_doc_id;
    UInt32 left_count;
    UInt32 buf_size;
    UInt32 pos;

    ReaderStreamCursor(LargePostingListReaderStreamPtr s, UInt32 first_doc_id, UInt32 total_count)
        : stream(std::move(s))
        , doc_buffer(stream->doc_buffer)
        , last_doc_id(first_doc_id)
        , left_count(total_count - 1)
        , buf_size(1)
        , pos(0)
    {
        if (stream->merged_part_offsets)
        {
            if (stream->merged_part_offsets->isMappingEnabled())
                doc_buffer[0] = (*stream->merged_part_offsets)[stream->part_index, first_doc_id];
            else
                doc_buffer[0] = first_doc_id + stream->part_starting_offset;
        }
        else
        {
            doc_buffer[0] = first_doc_id;
        }
        chassert(doc_buffer);
    }

    /// Embedded postings c'tor
    ReaderStreamCursor(UInt32 * doc_buffer_, UInt32 buf_size_)
        : doc_buffer(doc_buffer_)
        , left_count(0)
        , buf_size(buf_size_)
        , pos(0)
    {
        chassert(doc_buffer);
    }

    UInt32 ALWAYS_INLINE current() const
    {
        chassert(pos < buf_size);
        return doc_buffer[pos];
    }

    void ALWAYS_INLINE next()
    {
        chassert(pos < buf_size);
        ++pos;
        if (pos >= buf_size)
            loadNextBlock();
    }

    bool ALWAYS_INLINE empty() const { return buf_size == pos && left_count == 0; }

    /// Batch emit all remaining documents in current buffer and beyond
    template <typename Emit>
    void emitAll(Emit && emit)
    {
        while (!empty())
        {
            size_t remaining = buf_size - pos;
            for (size_t i = 0; i < remaining; ++i)
                emit(doc_buffer[pos + i]);
            pos = buf_size;
            loadNextBlock();
        }
    }

    /// Emit up to next_min (exclusive), loading blocks as needed
    template <typename Emit>
    void emitUntil(UInt32 next_min, Emit && emit)
    {
        chassert(current() < next_min);
        while (!empty())
        {
            // Emit entire block if all elements < next_min
            if (doc_buffer[buf_size - 1] < next_min)
            {
                for (size_t i = pos; i < buf_size; ++i)
                    emit(doc_buffer[i]);
                pos = buf_size;
                loadNextBlock();
            }
            else
            {
                // Emit partial block up to next_min
                const UInt32 * it = std::lower_bound(doc_buffer + pos, doc_buffer + buf_size, next_min);
                for (const UInt32 * p = doc_buffer + pos; p != it; ++p)
                    emit(*p);
                pos = it - doc_buffer;
                break;
            }
        }
    }

    bool ALWAYS_INLINE operator<(const ReaderStreamCursor & rhs) const { return current() < rhs.current(); }

private:
    void loadNextBlock()
    {
        if (left_count == 0)
            return;

        UInt32 count = std::min<UInt32>(128, left_count);
        auto & large_posting_buffer = *stream->getDataBuffer();
        UInt32 bytes;
        VarInt::readVarUInt32(bytes, large_posting_buffer);

        uint8_t * packed_buffer = stream->packed_buffer;

        if (large_posting_buffer.available() >= bytes)
        {
            uint8_t * packed_buffer_end
                = p4d1dec128v32(reinterpret_cast<uint8_t *>(large_posting_buffer.position()), count, doc_buffer, last_doc_id);
            large_posting_buffer.position() = reinterpret_cast<char *>(packed_buffer_end);
        }
        else
        {
            chassert(bytes <= 512);
            large_posting_buffer.readStrict(reinterpret_cast<char *>(packed_buffer), bytes);
            uint8_t * packed_buffer_end = p4d1dec128v32(packed_buffer, count, doc_buffer, last_doc_id);
            chassert(packed_buffer_end - packed_buffer == bytes);
        }

        last_doc_id = doc_buffer[count - 1];
        if (stream->merged_part_offsets)
        {
            if (stream->merged_part_offsets->isMappingEnabled())
            {
                for (UInt32 i = 0; i < count; ++i)
                    doc_buffer[i] = (*stream->merged_part_offsets)[stream->part_index, doc_buffer[i]];
            }
            else
            {
                for (UInt32 i = 0; i < count; ++i)
                    doc_buffer[i] += stream->part_starting_offset;
            }
        }

        left_count -= count;
        buf_size = count;
        pos = 0;
    }
};

struct ReaderStreamCursorNode
{
    ReaderStreamCursor * cursor;

    /// Inverted so that the priority queue elements are removed in ascending order.
    bool ALWAYS_INLINE operator<(const ReaderStreamCursorNode & rhs) const { return cursor->current() > rhs.cursor->current(); }
};

using ReaderStreamQueue = std::priority_queue<ReaderStreamCursorNode, std::vector<ReaderStreamCursorNode>>;

template <typename EmitFirst, typename Emit>
void mergePostingCursors(std::vector<ReaderStreamCursor> & cursors, EmitFirst && emit_first, Emit && emit)
{
    cursors.erase(std::remove_if(cursors.begin(), cursors.end(), [](const auto & c) { return c.empty(); }), cursors.end());

    if (cursors.empty())
        return;

    if (cursors.size() == 1)
    {
        emit_first(cursors[0].current());
        cursors[0].next();
        cursors[0].emitAll(emit);
        return;
    }

    ReaderStreamQueue heap;
    for (auto & c : cursors)
        heap.push(ReaderStreamCursorNode{&c});

    /// Emit first doc
    {
        auto cur = heap.top();
        heap.pop();
        emit_first(cur.cursor->current());
        cur.cursor->next();
        if (!cur.cursor->empty())
            heap.push(cur);
    }

    while (!heap.empty())
    {
        auto cur = heap.top();
        heap.pop();

        if (heap.empty())
            cur.cursor->emitAll(emit);
        else
            cur.cursor->emitUntil(heap.top().cursor->current(), emit);

        if (!cur.cursor->empty())
            heap.push(cur);
    }
}

LazyPostingStream::LazyPostingStream(const UInt32 * embedded_postings, UInt32 num_embedded_docs, ReaderStreamVector streams_)
    : merged_embedded_postings(embedded_postings, embedded_postings + num_embedded_docs)
    , streams(std::move(streams_))
{
}

LazyPostingStream::~LazyPostingStream() = default;

void PostingListStream::read(ReadBuffer & in, LargePostingListReaderStreamPtr stream)
{
    VarInt::readVarUInt32(doc_count, in);
    if (doc_count == 0)
        return;

    /// Last document id, used as base for delta decoding
    UInt32 last_doc_id;
    VarInt::readVarUInt32(last_doc_id, in);

    chassert(stream);

    if (stream->merged_part_offsets)
    {
        if (stream->merged_part_offsets->isMappingEnabled())
            embedded_postings[0] = (*stream->merged_part_offsets)[stream->part_index, last_doc_id];
        else
            embedded_postings[0] = last_doc_id + stream->part_starting_offset;
    }
    else
    {
        embedded_postings[0] = last_doc_id;
    }

    if (doc_count == 1)
        return;

    if (doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
    {
        UInt32 bytes;
        VarInt::readVarUInt32(bytes, in);
        UInt32 * doc_buffer = stream->doc_buffer;
        uint8_t * packed_buffer = stream->packed_buffer;
        if (in.available() >= bytes)
        {
            uint8_t * packed_buffer_end = p4d1dec128v32(reinterpret_cast<uint8_t *>(in.position()), doc_count - 1, doc_buffer, last_doc_id);
            in.position() = reinterpret_cast<char *>(packed_buffer_end);
        }
        else
        {
            chassert(bytes <= 512);
            in.readStrict(reinterpret_cast<char *>(packed_buffer), bytes);
            uint8_t * packed_buffer_end = p4d1dec128v32(packed_buffer, doc_count - 1, doc_buffer, last_doc_id);
            chassert(packed_buffer_end - packed_buffer == bytes);
        }

        if (stream->merged_part_offsets)
        {
            if (stream->merged_part_offsets->isMappingEnabled())
            {
                for (UInt32 i = 1; i < doc_count; ++i)
                    embedded_postings[i] = (*stream->merged_part_offsets)[stream->part_index, doc_buffer[i - 1]];
            }
            else
            {
                for (UInt32 i = 1; i < doc_count; ++i)
                    embedded_postings[i] = doc_buffer[i - 1] + stream->part_starting_offset;
            }
        }
        else
        {
            memcpy(&embedded_postings[1], doc_buffer, (doc_count - 1) * sizeof(UInt32));
        }

        return;
    }

    UInt32 num_large_blocks;
    VarInt::readVarUInt32(num_large_blocks, in);

    chassert(num_large_blocks >= 1);

    UInt32 dummy_id;
    VarInt::readVarUInt32(dummy_id, in);
    UInt64 large_posting_offset;
    readVarUInt(large_posting_offset, in);

    /// Skip metadata of large posting blocks
    for (UInt32 i = 1; i < num_large_blocks; ++i)
    {
        UInt64 dummy_offset;
        VarInt::readVarUInt32(dummy_id, in);
        readVarUInt(dummy_offset, in);
    }

    lazy_posting_stream
        = std::make_unique<LazyPostingStream>(nullptr, 0, ReaderStreamVector{stream, last_doc_id, doc_count, large_posting_offset});
}

void PostingListStream::write(WriteBuffer & wb, LargePostingListWriterStream & stream, const MergeTreeIndexTextParams & index_params) const
{
    VarInt::writeVarUInt32(doc_count, wb);
    if (doc_count == 0)
        return;

    /// --------------------------------------------
    /// Small posting list
    /// --------------------------------------------
    if (doc_count == 1)
    {
        VarInt::writeVarUInt32(embedded_postings[0], wb);
        return;
    }

    UInt32 * doc_delta_buffer = stream.doc_buffer;
    uint8_t * packed_buffer = stream.packed_buffer;

    if (doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
    {
        VarInt::writeVarUInt32(embedded_postings[0], wb);
        for (UInt32 i = 1; i < doc_count; ++i)
            doc_delta_buffer[i - 1] = embedded_postings[i] - embedded_postings[i - 1] - 1;
        uint8_t * end = p4enc32(doc_delta_buffer, doc_count - 1, packed_buffer);
        UInt32 len = static_cast<UInt32>(end - packed_buffer);
        VarInt::writeVarUInt32(len, wb);
        wb.write(reinterpret_cast<const char *>(packed_buffer), len);
        return;
    }

    /// --------------------------------------------
    /// Large posting list
    /// --------------------------------------------

    chassert(lazy_posting_stream);

    std::vector<ReaderStreamCursor> cursors;
    if (!lazy_posting_stream->merged_embedded_postings.empty())
        cursors.emplace_back(lazy_posting_stream->merged_embedded_postings.data(), lazy_posting_stream->merged_embedded_postings.size());
    for (const auto & lazy_stream : lazy_posting_stream->streams)
        cursors.emplace_back(lazy_stream.stream, lazy_stream.first_doc_id, lazy_stream.doc_count);

    UInt32 last_doc_id;

    /// Align to 128-doc blocks
    const UInt32 docs_per_large_block = (index_params.posting_list_block_size + 127) & ~127;
    const UInt32 large_doc_count = doc_count - 1;
    const UInt32 num_large_blocks = (large_doc_count + docs_per_large_block - 1) / docs_per_large_block;
    LargePostingBlockWriter block_writer(wb, stream.plain_hashing, docs_per_large_block);

    UInt32 buffered = 0;
    auto flush128 = [&]()
    {
        uint8_t * end = p4enc128v32(doc_delta_buffer, 128, packed_buffer);
        block_writer.addBlock(last_doc_id, reinterpret_cast<const char *>(packed_buffer), static_cast<UInt32>(end - packed_buffer));
        buffered = 0;
    };

    auto flush_tail = [&]()
    {
        if (buffered == 0)
            return;

        uint8_t * end = p4enc32(doc_delta_buffer, buffered, packed_buffer);
        block_writer.addBlock(last_doc_id, reinterpret_cast<const char *>(packed_buffer), static_cast<UInt32>(end - packed_buffer));
        buffered = 0;
    };

    mergePostingCursors(
        cursors,
        [&](UInt32 first_doc_id)
        {
            last_doc_id = first_doc_id;
            VarInt::writeVarUInt32(first_doc_id, wb);
            VarInt::writeVarUInt32(num_large_blocks, wb);
        },
        [&](UInt32 doc_id)
        {
            if (doc_id <= last_doc_id)
                throwReadAfterEOF();
            chassert(doc_id > last_doc_id);
            doc_delta_buffer[buffered++] = doc_id - last_doc_id - 1;
            last_doc_id = doc_id;
            if (buffered == 128)
                flush128();
        });

    flush_tail();
    block_writer.finish(num_large_blocks);
}

void PostingListStream::collect(UInt32 * buf) const
{
    if (doc_count == 0)
        return;

    /// --------------------------------------------
    /// Small posting list
    /// --------------------------------------------
    if (doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
    {
        memcpy(buf, embedded_postings, doc_count * sizeof(UInt32));
        return;
    }

    /// --------------------------------------------
    /// Large posting list
    /// --------------------------------------------

    chassert(lazy_posting_stream);

    std::vector<ReaderStreamCursor> cursors;
    if (!lazy_posting_stream->merged_embedded_postings.empty())
        cursors.emplace_back(lazy_posting_stream->merged_embedded_postings.data(), lazy_posting_stream->merged_embedded_postings.size());
    for (const auto & lazy_stream : lazy_posting_stream->streams)
        cursors.emplace_back(lazy_stream.stream, lazy_stream.first_doc_id, lazy_stream.doc_count);

    UInt32 buffered = 0;
    auto emit = [&](UInt32 doc_id) { buf[buffered++] = doc_id; };
    mergePostingCursors(cursors, emit, emit);
}

void PostingListStream::merge(const PostingListStream & other)
{
    if (other.doc_count == 0)
        return;

    // -----------------------------
    // Case 0: this is empty, take other
    // -----------------------------
    if (doc_count == 0)
    {
        /// TODO(amos): check if this const cast is safe
        *this = PostingListStream(std::move(const_cast<PostingListStream &>(other)));
        return;
    }

    const bool lhs_embedded = doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS;
    const bool rhs_embedded = other.doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS;

    /// TODO(amos): if const cast is not valid, use the following code
    // if (doc_count == 0)
    // {
    //     if (rhs_embedded)
    //     {
    //         std::copy(other.embedded_postings, other.embedded_postings + other.doc_count, embedded_postings);
    //     }
    //     else
    //     {
    //         lazy_posting_stream = std::make_unique<LazyPostingStream>(
    //             other.lazy_posting_stream->merged_embedded_postings.data(),
    //             static_cast<UInt32>(other.lazy_posting_stream->merged_embedded_postings.size()),
    //             other.lazy_posting_stream->streams);
    //     }
    //     doc_count = other.doc_count;
    //     return;
    // }

    // -----------------------------
    // Case 1: both embedded
    // -----------------------------
    if (lhs_embedded && rhs_embedded)
    {
        chassert(!lazy_posting_stream);
        chassert(!other.lazy_posting_stream);

        UInt32 total_count = doc_count + other.doc_count;

        if (total_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
        {
            // in-place merge
            std::copy(other.embedded_postings, other.embedded_postings + other.doc_count, embedded_postings + doc_count);
            std::inplace_merge(embedded_postings, embedded_postings + doc_count, embedded_postings + total_count);
            doc_count = total_count;
            return;
        }

        lazy_posting_stream = std::make_unique<LazyPostingStream>();
        lazy_posting_stream->merged_embedded_postings.reserve(total_count);

        std::merge(
            embedded_postings,
            embedded_postings + doc_count,
            other.embedded_postings,
            other.embedded_postings + other.doc_count,
            std::back_inserter(lazy_posting_stream->merged_embedded_postings));

        doc_count = total_count;
        return;
    }

    // -----------------------------
    // Case 2: at least one side is lazy
    // -----------------------------
    if (lhs_embedded)
    {
        chassert(!lazy_posting_stream);
        lazy_posting_stream = std::make_unique<LazyPostingStream>(embedded_postings, doc_count);
    }
    else
    {
        chassert(lazy_posting_stream);
    }

    UInt32 sz = lazy_posting_stream->merged_embedded_postings.size();
    if (rhs_embedded)
    {
        chassert(!other.lazy_posting_stream);
        lazy_posting_stream->merged_embedded_postings.insert(
            lazy_posting_stream->merged_embedded_postings.end(), other.embedded_postings, other.embedded_postings + other.doc_count);
    }
    else
    {
        chassert(other.lazy_posting_stream);
        lazy_posting_stream->merged_embedded_postings.insert(
            lazy_posting_stream->merged_embedded_postings.end(),
            other.lazy_posting_stream->merged_embedded_postings.begin(),
            other.lazy_posting_stream->merged_embedded_postings.end());
    }

    // in-place merge (assume both halves sorted, no duplicates)
    const auto mid = lazy_posting_stream->merged_embedded_postings.begin() + sz;
    std::inplace_merge(lazy_posting_stream->merged_embedded_postings.begin(), mid, lazy_posting_stream->merged_embedded_postings.end());

    // merge streams
    if (other.lazy_posting_stream)
        lazy_posting_stream->streams.merge(other.lazy_posting_stream->streams);

    doc_count += other.doc_count;

    chassert(doc_count > MAX_SIZE_OF_EMBEDDED_POSTINGS);
}

void PostingListInMemory::addRangeClosed(UInt32 min, UInt32 max)
{
    if (!empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "PostingListInMemory::addRangeClosed only supports adding range when empty");

    if (max < min)
        return;

    new (&m.bitmap) PostingListBitmap();
    m.bitmap.addRangeClosed(min, max);
}

void PostingListInMemory::addRange(UInt32 min, UInt32 max)
{
    if (!empty())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "PostingListInMemory::addRange only supports adding range when empty");

    if (max <= min)
        return;

    new (&m.bitmap) PostingListBitmap();
    m.bitmap.addRange(min, max);
}

void PostingListInMemory::write(WriteBuffer & out, UInt32 * doc_delta_buffer, uint8_t * packed_buffer) const
{
    size_t write_bytes = out.count();
    const UInt32 doc_count = size();
    VarInt::writeVarUInt32(doc_count, out);
    if (doc_count == 0)
        return;

    chassert(doc_delta_buffer);
    chassert(packed_buffer);

    UInt32 last_doc_id = 0;
    UInt32 buffered = 0;
    bool first = true;

    auto flush = [&](bool final)
    {
        if (buffered == 0)
            return;

        uint8_t * end = final ? p4enc32(doc_delta_buffer, buffered, packed_buffer) : p4enc128v32(doc_delta_buffer, 128, packed_buffer);

        const UInt32 len = static_cast<UInt32>(end - packed_buffer);
        chassert(len <= 512);

        VarInt::writeVarUInt32(len, out);
        out.write(reinterpret_cast<const char *>(packed_buffer), len);

        buffered = 0;
    };

    auto write_doc = [&](UInt32 doc_id)
    {
        if (first)
        {
            /// First doc id is written as-is
            VarInt::writeVarUInt32(doc_id, out);
            last_doc_id = doc_id;
            first = false;
            return;
        }

        if (doc_id <= last_doc_id)
        {
            if (doc_id == last_doc_id)
                return;

            throw Exception(ErrorCodes::INCORRECT_DATA, "Received out of order doc id. doc_id = {}, last_doc_id = {}", doc_id, last_doc_id);
        }

        doc_delta_buffer[buffered++] = doc_id - last_doc_id - 1;
        last_doc_id = doc_id;

        if (buffered == 128)
            flush(false);
    };

    if (isBitmap())
    {
        for (UInt32 doc_id : m.bitmap)
            write_doc(doc_id);
    }
    else
    {
        for (UInt32 doc_id : m.set)
            write_doc(doc_id);
    }

    flush(true);
    write_bytes = out.count() - write_bytes;
    if (write_bytes > std::numeric_limits<UInt32>::max())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "write_bytes = {} > max uint32", write_bytes);
}

void PostingListInMemory::read(
    ReadBuffer & in, UInt32 * doc_buffer, uint8_t * packed_buffer, const MergedPartOffsets * merged_part_offsets, size_t part_index)
{
    UInt32 left_count;
    UInt32 last_doc_id;
    VarInt::readVarUInt32(left_count, in);
    VarInt::readVarUInt32(last_doc_id, in);

    if (merged_part_offsets)
        add(static_cast<UInt32>((*merged_part_offsets)[part_index, last_doc_id]));
    else
        add(last_doc_id);

    --left_count;

    while (left_count > 0)
    {
        UInt32 bytes;
        VarInt::readVarUInt32(bytes, in);
        UInt32 count = std::min(left_count, UInt32(128));
        if (in.available() >= bytes)
        {
            uint8_t * packed_buffer_end = p4d1dec128v32(reinterpret_cast<uint8_t *>(in.position()), count, doc_buffer, last_doc_id);
            in.position() = reinterpret_cast<char *>(packed_buffer_end);
        }
        else
        {
            chassert(bytes <= 512);
            in.readStrict(reinterpret_cast<char *>(packed_buffer), bytes);
            uint8_t * packed_buffer_end = p4d1dec128v32(packed_buffer, count, doc_buffer, last_doc_id);
            chassert(packed_buffer_end - packed_buffer == bytes);
        }

        left_count -= count;
        last_doc_id = doc_buffer[count - 1]; /// before offset re-mapping
        if (merged_part_offsets)
        {
            for (UInt32 i = 0; i < count; ++i)
                doc_buffer[i] = static_cast<UInt32>((*merged_part_offsets)[part_index, doc_buffer[i]]);
        }
        addMany(count, doc_buffer);
    }
}

}
