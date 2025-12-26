#pragma once

#include <base/defines.h>
#include <base/types.h>

#include <memory>
#include <vector>

namespace roaring
{

class Roaring;

}

namespace DB
{

class Arena;
class ReadBuffer;
class WriteBuffer;
class MergedPartOffsets;
struct MergeTreeIndexTextParams;

struct alignas(16) PostingListChunk
{
    PostingListChunk * next = nullptr;
    UInt32 last_doc_id;
    UInt32 len;

    explicit PostingListChunk(UInt32 last_doc_id_, UInt32 len_) noexcept
        : last_doc_id(last_doc_id_)
        , len(len_)
    {
    }

    /// Returns a pointer to the memory buffer immediately following this chunk structure.
    ///
    /// This uses the "trailing array" / "struct hack" pattern: the actual data buffer is allocated together with the
    /// struct, so that memory is contiguous. For example:
    ///
    ///   // allocate a chunk with len bytes of data
    ///   PostingListChunk* chunk =
    ///       (PostingListChunk*)malloc(sizeof(PostingListChunk) + len);
    ///   chunk->len = len;
    ///   uint8_t* buffer = chunk->data();
    ///
    /// The expression (this + 1) computes the address immediately after this struct, effectively pointing to the start
    /// of the data buffer.
    uint8_t * data() { return reinterpret_cast<uint8_t *>(this + 1); }
    const uint8_t * data() const { return reinterpret_cast<const uint8_t *>(this + 1); }
    void write(WriteBuffer & wb) const;
};

struct alignas(8) PostingListWriter
{
    Int32 type = -1;
    UInt32 doc_count = 0;

    PostingListChunk * blocks_head = nullptr;
    PostingListChunk ** blocks_tail = nullptr;
    UInt32 * doc_delta_buffer = nullptr;

    UInt32 first_doc_id = 0;
    UInt32 last_doc_id = 0;

    void add(UInt32 doc_id, Arena * arena, uint8_t * packed_buffer);
    void
    finish(WriteBuffer & wb, WriteBuffer & large_posting, uint8_t * packed_buffer, const MergeTreeIndexTextParams & index_params) const;
};

static_assert(sizeof(PostingListWriter) <= 40, "PostingListWriter must be less than 40 bytes");

struct LargePostingListReaderStream;
using LargePostingListReaderStreamPtr = std::shared_ptr<LargePostingListReaderStream>;

struct LargePostingBlockMeta
{
    UInt32 last_doc_id;
    UInt32 doc_count;
    UInt64 offset;

    /// Default constructor (required by std::vector, resize, etc.)
    LargePostingBlockMeta() noexcept
        : last_doc_id(0)
        , doc_count(0)
        , offset(0)
    {
    }

    /// Construct with offset only
    LargePostingBlockMeta(UInt64 offset_) noexcept // NOLINT
        : last_doc_id(0)
        , doc_count(0)
        , offset(offset_)
    {
    }

    /// Fully initialized constructor
    LargePostingBlockMeta(UInt32 last_doc_id_, UInt32 doc_count_, UInt64 offset_) noexcept
        : last_doc_id(last_doc_id_)
        , doc_count(doc_count_)
        , offset(offset_)
    {
    }

    /// Implicit conversion to offset (INTENTIONALLY implicit)
    operator UInt64() const noexcept { return offset; } // NOLINT
};

using LargePostingBlockMetas = std::vector<LargePostingBlockMeta>;

struct ReaderStreamEntry
{
    LargePostingListReaderStreamPtr stream;
    UInt32 first_doc_id;
    UInt32 doc_count;

    /// TODO(amos): Redundant for merge path: blocks are consumed sequentially
    LargePostingBlockMetas large_posting_blocks;

    ReaderStreamEntry(
        LargePostingListReaderStreamPtr stream_, UInt32 first_doc_id_, UInt32 doc_count_, LargePostingBlockMetas large_posting_blocks_);

    bool operator==(const ReaderStreamEntry & other) const { return stream.get() == other.stream.get(); }

    static std::shared_ptr<roaring::Roaring>
    materializeLargeBlockIntoBitmap(LargePostingListReaderStream & stream, UInt32 last_doc_id, UInt32 doc_count, UInt64 offset);
};

struct ReaderStreamVector
{
    std::vector<ReaderStreamEntry> entries;

    ReaderStreamVector() = default;

    ReaderStreamVector(
        LargePostingListReaderStreamPtr stream, UInt32 first_doc_id, UInt32 doc_count, LargePostingBlockMetas large_posting_blocks);

    void add(LargePostingListReaderStreamPtr stream, UInt32 first_doc_id, UInt32 doc_count, LargePostingBlockMetas large_posting_blocks);

    void merge(const ReaderStreamVector & other)
    {
        for (const auto & e : other.entries)
            add(e.stream, e.first_doc_id, e.doc_count, e.large_posting_blocks);
    }

    size_t size() const { return entries.size(); }

    bool empty() const { return entries.empty(); }

    void clear() { entries.clear(); }

    // --- support range-based for ---
    auto begin() { return entries.begin(); }
    auto end() { return entries.end(); }
    auto begin() const { return entries.begin(); }
    auto end() const { return entries.end(); }
};

struct LargePostingListWriterStream;

/// Initialized once during deserialization and reused across columns.
struct LazyPostingStream
{
    std::vector<UInt32> merged_embedded_postings;
    ReaderStreamVector streams;

    LazyPostingStream() = default;
    LazyPostingStream(const UInt32 * embedded_postings, UInt32 num_embedded_docs, ReaderStreamVector streams_ = {});
    ~LazyPostingStream();
};

using LazyPostingStreamPtr = std::unique_ptr<LazyPostingStream>;

inline static constexpr UInt64 MAX_SIZE_OF_EMBEDDED_POSTINGS = 6;

struct alignas(8) PostingListStream
{
    Int32 type = 0;
    UInt32 doc_count = 0;

    UInt32 embedded_postings[MAX_SIZE_OF_EMBEDDED_POSTINGS];
    LazyPostingStreamPtr lazy_posting_stream;

    PostingListStream() = default;
    PostingListStream(const PostingListStream &) = delete;
    PostingListStream & operator=(const PostingListStream &) = delete;

    PostingListStream(PostingListStream && other) noexcept
        : type(other.type)
        , doc_count(other.doc_count)
        , lazy_posting_stream(std::move(other.lazy_posting_stream))
    {
        if (doc_count > 0 && doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
        {
            chassert(!lazy_posting_stream);
            memcpy(embedded_postings, other.embedded_postings, doc_count * sizeof(UInt32));
        }
        other.type = 0;
        other.doc_count = 0;
    }

    PostingListStream & operator=(PostingListStream && other) noexcept
    {
        if (this != &other)
        {
            type = other.type;
            doc_count = other.doc_count;
            lazy_posting_stream = std::move(other.lazy_posting_stream);

            if (doc_count > 0 && doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
            {
                chassert(!lazy_posting_stream);
                memcpy(embedded_postings, other.embedded_postings, doc_count * sizeof(UInt32));
            }

            other.type = 0;
            other.doc_count = 0;
        }
        return *this;
    }

    void read(ReadBuffer & in, LargePostingListReaderStreamPtr stream);

    void write(WriteBuffer & wb, LargePostingListWriterStream & stream, const MergeTreeIndexTextParams & index_params) const;

    /// Collects all doc IDs into `buf`. Caller must preallocate `buf` with more than `doc_count` elements.
    void collect(UInt32 * buf) const;

    void merge(const PostingListStream & other);
};

static_assert(sizeof(PostingListStream) <= 40, "PostingListStream must be less than 40 bytes");

struct PostingListData
{
    union Storage
    {
        PostingListWriter writer;
        PostingListStream stream;
        /// TODO(amos):
        /// We should introduce another variant which holds a single stream that can do seek but cannot merge

        Storage() { }
        ~Storage() { }
    } storage;

    /// Zero initialized
    PostingListData() { new (&storage.stream) PostingListStream(); }

    explicit PostingListData(bool /* is_merge */) { new (&storage.stream) PostingListStream(); }

    PostingListData(const PostingListData & other) = delete;
    PostingListData & operator=(const PostingListData & other) = delete;

    PostingListData(PostingListData && other) noexcept
    {
        if (other.isWriter())
            new (&storage.writer) PostingListWriter(std::move(other.storage.writer));
        else if (other.isStream())
            new (&storage.stream) PostingListStream(std::move(other.storage.stream));
    }

    PostingListData & operator=(PostingListData && other) noexcept
    {
        if (this != &other)
        {
            destroy();
            if (other.isWriter())
                new (&storage.writer) PostingListWriter(std::move(other.storage.writer));
            else if (other.isStream())
                new (&storage.stream) PostingListStream(std::move(other.storage.stream));
        }
        return *this;
    }

    ~PostingListData() { destroy(); }

    void destroy()
    {
        if (isStream())
            storage.stream.~PostingListStream();
    }

    void toWriterUnsafe() { storage.writer.type = -1; }

    bool isWriter() const { return storage.writer.type == -1; }
    bool isStream() const { return storage.writer.type == 0; }

    PostingListWriter & writer() { return storage.writer; }
    PostingListStream & stream() { return storage.stream; }

    const PostingListWriter & writer() const { return storage.writer; }
    const PostingListStream & stream() const { return storage.stream; }
};

static_assert(sizeof(PostingListData) == 40, "PostingListData must be 40 bytes");

}
