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
    if (doc_count < 9)
    {
        uint8_t * packed_buffer_end = p4enc32(doc_delta_buffer, doc_count - 1, packed_buffer);
        UInt32 len = static_cast<UInt32>(packed_buffer_end - packed_buffer);
        VarInt::writeVarUInt32(len, wb);
        wb.write(reinterpret_cast<const char *>(packed_buffer), len);
        return;
    }

    /// --------------------------------------------
    /// Large posting list mode
    ///
    /// wb:
    ///   doc_count
    ///   first_doc_id
    ///   first_large_block_offset
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

LazyPostingStream::LazyPostingStream(
    UInt32 last_doc_id_, LargePostingListReaderStreamPtr stream_, const MergedPartOffsets * merged_part_offsets_, size_t part_index_)
    : last_doc_id(last_doc_id_)
    , streams({std::move(stream_)})
    , merged_part_offsets(merged_part_offsets_)
    , part_index(part_index_)
{
}

LazyPostingStream::~LazyPostingStream() = default;

void PostingListStream::read(
    ReadBuffer & in,
    LargePostingListReaderStreamPtr stream,
    const MergedPartOffsets * merged_part_offsets,
    size_t part_index,
    Arena * arena)
{
    VarInt::readVarUInt32(doc_count, in);
    if (doc_count == 0)
        return;

    /// Last document id, used as base for delta decoding
    UInt32 last_doc_id;
    VarInt::readVarUInt32(last_doc_id, in);

    auto create_embedded_postings = [arena]()
    {
        auto * place = arena->alignedAlloc(sizeof(PostingListBitmap), alignof(PostingListBitmap));
        new (place) PostingListBitmap();
        return reinterpret_cast<PostingListBitmap *>(place);
    };

    if (doc_count == 1)
    {
        embedded_postings = create_embedded_postings();
        if (merged_part_offsets)
            embedded_postings->add((*merged_part_offsets)[part_index, last_doc_id]);
        else
            embedded_postings->add(last_doc_id);
        return;
    }

    chassert(stream);

    UInt32 * doc_buffer = stream->doc_buffer;
    uint8_t * packed_buffer = stream->packed_buffer;

    if (doc_count < 9)
    {
        UInt32 bytes;
        VarInt::readVarUInt32(bytes, in);
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

        embedded_postings = create_embedded_postings();
        if (merged_part_offsets)
        {
            embedded_postings->add((*merged_part_offsets)[part_index, last_doc_id]);
            for (UInt32 i = 0; i < doc_count - 1; ++i)
                embedded_postings->add((*merged_part_offsets)[part_index, doc_buffer[i]]);
        }
        else
        {
            embedded_postings->add(last_doc_id);
            embedded_postings->addMany(doc_count - 1, doc_buffer);
        }

        return;
    }

    /// Eager reading
    {
        UInt32 num_large_blocks;
        VarInt::readVarUInt32(num_large_blocks, in);

        chassert(num_large_blocks >= 1);

        UInt32 dummy_id;
        VarInt::readVarUInt32(dummy_id, in);
        UInt64 large_posting_offset;
        readVarUInt(large_posting_offset, in);
        chassert(static_cast<long>(large_posting_offset) == stream->getPosition());

        /// Skip metadata of large posting blocks
        for (UInt32 i = 1; i < num_large_blocks; ++i)
        {
            UInt64 dummy_offset;
            VarInt::readVarUInt32(dummy_id, in);
            readVarUInt(dummy_offset, in);
        }

        embedded_postings = create_embedded_postings();
        if (merged_part_offsets)
            embedded_postings->add(static_cast<UInt32>((*merged_part_offsets)[part_index, last_doc_id]));
        else
            embedded_postings->add(last_doc_id);

        UInt32 left_count = doc_count - 1;
        auto & large_posting_buffer = *stream->getDataBuffer();
        while (left_count > 0)
        {
            UInt32 bytes;
            VarInt::readVarUInt32(bytes, large_posting_buffer);
            UInt32 count = std::min(left_count, UInt32(128));
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

            left_count -= count;
            last_doc_id = doc_buffer[count - 1]; /// before offset re-mapping
            if (merged_part_offsets)
            {
                for (UInt32 i = 0; i < count; ++i)
                    doc_buffer[i] = static_cast<UInt32>((*merged_part_offsets)[part_index, doc_buffer[i]]);
            }
            embedded_postings->addMany(count, doc_buffer);
        }
    }

    /// TODO(amos): lazy

    // auto * place = arena->alignedAlloc(sizeof(LazyPostingStream), alignof(LazyPostingStream));
    // new (place) LazyPostingStream(last_doc_id, std::move(stream), merged_part_offsets, part_index);
    // lazy_posting_stream = reinterpret_cast<LazyPostingStream *>(place);

    // readVarUInt(lazy_posting_stream->large_posting_offset, in);
    // chassert(static_cast<long>(lazy_posting_stream->large_posting_offset) == stream->getPosition());

    // /// Skip metadata of large posting blocks
    // UInt32 num_large_blocks;
    // VarInt::readVarUInt32(num_large_blocks, in);
    // for (UInt32 i = 0; i < num_large_blocks; ++i)
    // {
    //     UInt32 dummy_id;
    //     UInt64 dummy_offset;
    //     VarInt::readVarUInt32(dummy_id, in);
    //     readVarUInt(dummy_offset, in);
    // }
}

void PostingListStream::write(WriteBuffer & wb, LargePostingListWriterStream & stream, const MergeTreeIndexTextParams & index_params) const
{
    VarInt::writeVarUInt32(doc_count, wb);
    if (doc_count == 0)
        return;

    UInt32 last_doc_id = 0;
    bool first = true;

    /// --------------------------------------------
    /// Small posting list
    /// --------------------------------------------
    if (doc_count == 1)
    {
        chassert(embedded_postings);
        UInt32 doc_id = embedded_postings->minimum();
        VarInt::writeVarUInt32(doc_id, wb);
        return;
    }

    UInt32 * doc_delta_buffer = stream.doc_buffer;
    uint8_t * packed_buffer = stream.packed_buffer;

    if (doc_count < 9)
    {
        chassert(embedded_postings);

        UInt32 buffered = 0;
        auto emit = [&](UInt32 doc_id)
        {
            if (first)
            {
                VarInt::writeVarUInt32(doc_id, wb);
                last_doc_id = doc_id;
                first = false;
                return;
            }

            doc_delta_buffer[buffered++] = doc_id - last_doc_id - 1;
            last_doc_id = doc_id;
        };

        for (UInt32 doc_id : *embedded_postings)
            emit(doc_id);

        uint8_t * end = p4enc32(doc_delta_buffer, buffered, packed_buffer);
        UInt32 len = static_cast<UInt32>(end - packed_buffer);
        VarInt::writeVarUInt32(len, wb);
        wb.write(reinterpret_cast<const char *>(packed_buffer), len);
        return;
    }

    /// --------------------------------------------
    /// Large posting list
    /// --------------------------------------------

    /// TODO(amos): lazy
    chassert(embedded_postings);

    /// Align to 128-doc blocks
    const UInt32 docs_per_large_block = (index_params.posting_list_block_size + 127) & ~127;
    const UInt32 large_doc_count = doc_count - 1;
    const UInt32 num_large_blocks = (large_doc_count + docs_per_large_block - 1) / docs_per_large_block;

    /// Write first doc inline
    auto write_first_doc = [&](UInt32 doc_id)
    {
        VarInt::writeVarUInt32(doc_id, wb);
        VarInt::writeVarUInt32(num_large_blocks, wb);
        last_doc_id = doc_id;
        first = false;
    };

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

    auto emit = [&](UInt32 doc_id)
    {
        if (first)
        {
            write_first_doc(doc_id);
            return;
        }

        chassert(doc_id > last_doc_id);
        doc_delta_buffer[buffered++] = doc_id - last_doc_id - 1;
        last_doc_id = doc_id;

        if (buffered == 128)
            flush128();
    };

    for (UInt32 doc_id : *embedded_postings)
        emit(doc_id);

    flush_tail();
    block_writer.finish(num_large_blocks);

    /// TODO(amos): lazy and merge
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
