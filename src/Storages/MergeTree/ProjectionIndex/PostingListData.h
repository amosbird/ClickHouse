#pragma once

#include <base/defines.h>
#include <base/types.h>

#include <set>
#include <roaring/roaring.hh>

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

struct PostingListSet
{
    bool full() const { return m_size >= 8; }

    const UInt32 * begin() const { return container; }
    const UInt32 * end() const { return container + m_size; }

    const UInt32 * find(UInt32 value) const
    {
        const UInt32 * it = std::lower_bound(begin(), end(), value);
        if (it != end() && *it == value)
            return it;
        return end();
    }

    /// Return true if full
    bool insert(UInt32 value)
    {
        UInt32 * it = std::lower_bound(container, container + m_size, value);

        if (it != end() && *it == value)
            return false;

        if (full())
            return true;

        std::move_backward(it, container + m_size, container + m_size + 1);

        *it = value;
        ++m_size;

        return false;
    }

    bool merge(const PostingListSet & other)
    {
        if (other.m_size == 0)
            return true;

        if (m_size == 0)
        {
            std::copy(other.container, other.container + other.m_size, container);
            m_size = other.m_size;
            return true;
        }

        UInt32 i = m_size;
        UInt32 j = other.m_size;
        UInt32 merged_size = 0;
        while (i > 0 && j > 0)
        {
            if (container[i - 1] == other.container[j - 1])
            {
                --i;
                --j;
                ++merged_size;
            }
            else if (container[i - 1] > other.container[j - 1])
            {
                --i;
                ++merged_size;
            }
            else
            {
                --j;
                ++merged_size;
            }
        }

        merged_size += i + j;
        if (merged_size > 8)
            return false;

        i = m_size;
        j = other.m_size;
        UInt32 k = merged_size;

        while (i > 0 && j > 0)
        {
            if (container[i - 1] == other.container[j - 1])
            {
                container[--k] = container[--i];
                --j;
            }
            else if (container[i - 1] > other.container[j - 1])
            {
                container[--k] = container[--i];
            }
            else
            {
                container[--k] = other.container[--j];
            }
        }

        while (i > 0)
            container[--k] = container[--i];

        while (j > 0)
            container[--k] = other.container[--j];

        m_size = merged_size;
        return true;
    }

    void intersect(const PostingListSet & other)
    {
        if (m_size == 0 || other.m_size == 0)
        {
            m_size = 0;
            return;
        }

        UInt32 i = 0;
        UInt32 j = 0;
        UInt32 k = 0;

        while (i < m_size && j < other.m_size)
        {
            if (container[i] == other.container[j])
            {
                if (k < i)
                    container[k] = container[i];
                ++i;
                ++j;
                ++k;
            }
            else if (container[i] < other.container[j])
            {
                ++i;
            }
            else
            {
                ++j;
            }
        }

        m_size = k;
    }


    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }

    /// This struct is designed to have the same size (40 bytes) as roaring_array_t
    /// from the Roaring Bitmap library, which has the following structure:
    ///
    /// typedef struct roaring_array_s {
    ///     int32_t size;
    ///     int32_t allocation_size;
    ///     ROARING_CONTAINER_T **containers;  // Use container_t in non-API files!
    ///     uint16_t *keys;
    ///     uint8_t *typecodes;
    ///     uint8_t flags;
    /// } roaring_array_t;
    ///
    /// In our implementation:
    /// - 'type' (4 bytes) is always 0, indicating this is a PostingListSet
    /// - 'container' (32 bytes) stores up to 8 UInt32 values
    /// - 'm_size' (4 bytes) tracks the number of elements in use
    ///
    /// Total size: 4 + 32 + 4 = 40 bytes
    Int32 type = 0;
    UInt32 m_size = 0;
    UInt32 container[8];
}; // sizeof == 40

using PostingListBitmap = roaring::Roaring; // sizeof == 40

struct alignas(8) PostingListInMemory
{
    union M
    {
        PostingListSet set;
        PostingListBitmap bitmap;

        M()
            : set()
        {
        }

        ~M()
        {
            if (set.type != 0)
                bitmap.~Roaring();
        }
    } m;

    PostingListInMemory() { new (&m.set) PostingListSet(); }

    PostingListInMemory(const PostingListInMemory & other)
    {
        if (other.m.set.type != 0)
            new (&m.bitmap) roaring::Roaring(other.m.bitmap);
        else
            new (&m.set) PostingListSet(other.m.set);
    }

    PostingListInMemory & operator=(const PostingListInMemory & other)
    {
        if (this != &other)
        {
            this->~PostingListInMemory();

            if (other.m.set.type != 0)
                new (&m.bitmap) roaring::Roaring(other.m.bitmap);
            else
                new (&m.set) PostingListSet(other.m.set);
        }
        return *this;
    }

    PostingListInMemory(PostingListInMemory && other) noexcept
    {
        if (other.m.set.type != 0)
            new (&m.bitmap) PostingListBitmap(std::move(other.m.bitmap));
        else
            new (&m.set) PostingListSet(std::move(other.m.set));
    }

    PostingListInMemory & operator=(PostingListInMemory && other) noexcept
    {
        if (this != &other)
        {
            this->~PostingListInMemory();

            if (other.m.set.type != 0)
                new (&m.bitmap) PostingListBitmap(std::move(other.m.bitmap));
            else
                new (&m.set) PostingListSet(std::move(other.m.set));
        }
        return *this;
    }

    bool empty() const { return m.set.type == 0 && m.set.empty(); }

    bool isBitmap() const { return m.set.type != 0; }

    size_t size() const { return m.set.type == 0 ? m.set.m_size : m.bitmap.cardinality(); }

    size_t cardinality() const { return size(); }

    UInt32 minimum() const
    {
        if (m.set.type == 0)
        {
            if (m.set.empty())
                return 0;
            return m.set.container[0];
        }
        else
        {
            return m.bitmap.minimum();
        }
    }

    UInt32 maximum() const
    {
        if (m.set.type == 0)
        {
            if (m.set.empty())
                return 0;
            return m.set.container[m.set.m_size - 1];
        }
        else
        {
            return m.bitmap.maximum();
        }
    }

    size_t getSizeInBytes() const
    {
        if (m.set.type == 0)
            return 40;
        return 40 + m.bitmap.getSizeInBytes();
    }

    void add(UInt32 doc)
    {
        if (m.set.type == 0)
        {
            if (m.set.insert(doc))
            {
                PostingListBitmap bitmap(m.set.m_size, m.set.container);
                new (&m.bitmap) PostingListBitmap(std::move(bitmap));
            }
            else
            {
                return;
            }
        }

        chassert(m.set.type != 0);
        m.bitmap.add(doc);
    }

    void addMany(size_t num, UInt32 * docs)
    {
        if (num > 8)
        {
            if (m.set.type == 0)
            {
                if (m.set.m_size > 0)
                {
                    PostingListBitmap bitmap(m.set.m_size, m.set.container);
                    bitmap.addMany(num, docs);
                    new (&m.bitmap) PostingListBitmap(std::move(bitmap));
                }
                else
                {
                    new (&m.bitmap) PostingListBitmap(num, docs);
                }
            }
            else
            {
                m.bitmap.addMany(num, docs);
            }
        }
        else
        {
            if (m.set.type != 0)
            {
                m.bitmap.addMany(num, docs);
            }
            else
            {
                UInt32 i = 0;
                for (; i < num; ++i)
                {
                    if (m.set.insert(docs[i]))
                    {
                        PostingListBitmap bitmap(m.set.m_size, m.set.container);
                        new (&m.bitmap) PostingListBitmap(std::move(bitmap));
                        break;
                    }
                }

                if (i < num)
                    m.bitmap.addMany(num - i, docs + i);
            }
        }
    }

    void addRangeClosed(UInt32 min, UInt32 max);
    void addRange(UInt32 min, UInt32 max);

    void merge(const PostingListInMemory & other)
    {
        if (other.empty())
            return;

        if (this->empty())
        {
            *this = other;
            return;
        }

        /// Fast path: both are small sets
        if (!this->isBitmap() && !other.isBitmap())
        {
            if (m.set.merge(other.m.set))
                return;

            /// overflow: upgrade to bitmap
            PostingListBitmap bitmap(m.set.m_size, m.set.container);
            bitmap.addMany(other.m.set.m_size, other.m.set.container);
            new (&m.bitmap) PostingListBitmap(std::move(bitmap));
            return;
        }

        /// General path: at least one side is bitmap

        PostingListBitmap result;

        if (isBitmap())
            result = std::move(m.bitmap);
        else
            result = PostingListBitmap(m.set.m_size, m.set.container);

        if (other.isBitmap())
            result |= other.m.bitmap;
        else
            result.addMany(other.m.set.m_size, other.m.set.container);

        new (&m.bitmap) PostingListBitmap(std::move(result));
    }

    PostingListInMemory & operator|=(const PostingListInMemory & other)
    {
        merge(other);
        return *this;
    }

    friend PostingListInMemory operator|(PostingListInMemory lhs, const PostingListInMemory & rhs)
    {
        lhs |= rhs;
        return lhs;
    }

    void intersect(const PostingListInMemory & other)
    {
        if (this->empty())
            return;

        if (other.empty())
        {
            *this = {};
            return;
        }

        if (!this->isBitmap() && !other.isBitmap())
        {
            m.set.intersect(other.m.set);
        }
        else
        {
            PostingListBitmap result;

            if (isBitmap())
                result = std::move(m.bitmap);
            else
                result = PostingListBitmap(m.set.m_size, m.set.container);

            if (other.isBitmap())
                result &= other.m.bitmap;
            else
                result &= PostingListBitmap(other.m.set.m_size, other.m.set.container);

            if (result.cardinality() <= 8)
            {
                UInt32 i = 0;
                for (auto x : result)
                    m.set.container[i++] = x;
                m.set.type = 0;
                m.set.m_size = i;
            }
            else
            {
                new (&m.bitmap) PostingListBitmap(std::move(result));
            }
        }
    }

    PostingListInMemory & operator&=(const PostingListInMemory & other)
    {
        intersect(other);
        return *this;
    }

    friend PostingListInMemory operator&(PostingListInMemory lhs, const PostingListInMemory & rhs)
    {
        lhs &= rhs;
        return lhs;
    }

    void toUint32Array(UInt32 * out) const
    {
        if (empty())
            return;

        if (isBitmap())
        {
            m.bitmap.toUint32Array(out);
        }
        else
        {
            /// PostingListSet: already sorted UInt32 array
            std::memcpy(out, m.set.container, m.set.m_size * sizeof(UInt32));
        }
    }

    void generateFilter(UInt8 * result, UInt32 start, size_t len) const
    {
        if (empty())
            return;

        if (m.set.type == 0)
        {
            for (UInt32 i = 0; i < m.set.m_size; ++i)
            {
                UInt32 doc_id = m.set.container[i];
                if (doc_id >= start && doc_id < start + len)
                    result[doc_id - start] = 1;
            }
        }
        else
        {
            PostingListBitmap::const_iterator it = m.bitmap.begin();
            PostingListBitmap::const_iterator end = m.bitmap.end();

            it.equalorlarger(start);

            while (it != end && *it < start + len)
            {
                result[*it - start] = 1;
                ++it;
            }
        }
    }

    void write(WriteBuffer & out, UInt32 * doc_delta_buffer, uint8_t * packed_buffer) const;

    void
    read(ReadBuffer & in, UInt32 * doc_buffer, uint8_t * packed_buffer, const MergedPartOffsets * merged_part_offsets, size_t part_index);
};

static_assert(sizeof(PostingListInMemory) == 40, "PostingListInMemory must be 40 bytes");

class MergeTreeReaderStreamSingleColumnWholePart;

struct SharedPtrAddressComparator
{
    using is_transparent = void;

    template <typename T, typename U>
    bool operator()(const std::shared_ptr<T> & lhs, const std::shared_ptr<U> & rhs) const
    {
        return lhs.get() < rhs.get();
    }

    template <typename T, typename U>
    bool operator()(const std::shared_ptr<T> & lhs, const U * rhs) const
    {
        return lhs.get() < rhs;
    }

    template <typename T, typename U>
    bool operator()(const T * lhs, const std::shared_ptr<U> & rhs) const
    {
        return lhs < rhs.get();
    }
};

struct LargePostingListReaderStream;
using LargePostingListReaderStreamPtr = std::shared_ptr<LargePostingListReaderStream>;

struct LargePostingListWriterStream;

/// Initialized once during deserialization prefix and reused across columns.
/// Owns mutable read state and is not thread-safe.
struct LazyPostingStream
{
    UInt32 doc_buffer_pos = 0;
    UInt32 doc_buffer_up_to = 0;
    UInt32 left_count = 0;

    UInt32 last_doc_id;

    UInt32 num_large_blocks = 0;
    UInt64 * large_block_offsets = nullptr;
    UInt32 * large_block_last_doc_ids = nullptr;

    UInt64 large_posting_offset = 0;
    std::set<LargePostingListReaderStreamPtr, SharedPtrAddressComparator> streams;

    /// Lives for the entire merge process; a raw pointer is sufficient.
    const MergedPartOffsets * merged_part_offsets;

    /// Index of the source part being merged.
    size_t part_index;

    LazyPostingStream(
        UInt32 last_doc_id_, LargePostingListReaderStreamPtr stream_, const MergedPartOffsets * merged_part_offsets_, size_t part_index_);

    ~LazyPostingStream();

    /// Return false if EOS
    bool get(UInt32 & doc);

    // void restart(UInt32 * embedded_docs, UInt32 num_embedded_docs, UInt32 doc_count)
    // {
    //     doc_buffer_pos = 0;
    //     doc_buffer_up_to = num_embedded_docs;
    //     memcpy(doc_buffer, embedded_docs, doc_buffer_up_to);
    //     left_count = doc_count;
    // }
};

struct alignas(8) PostingListStream
{
    Int32 type = -2;
    UInt32 doc_count = 0;
    PostingListBitmap * embedded_postings = nullptr;
    LazyPostingStream * lazy_posting_stream = nullptr;

    PostingListStream() = default;
    PostingListStream(const PostingListStream &) = delete;
    PostingListStream & operator=(const PostingListStream &) = delete;

    PostingListStream(PostingListStream && other) noexcept
        : type(other.type)
        , doc_count(other.doc_count)
        , embedded_postings(other.embedded_postings)
        , lazy_posting_stream(other.lazy_posting_stream)
    {
        other.type = -2;
        other.doc_count = 0;
        other.embedded_postings = nullptr;
        other.lazy_posting_stream = nullptr;
    }

    PostingListStream & operator=(PostingListStream && other) noexcept
    {
        if (this != &other)
        {
            type = other.type;
            doc_count = other.doc_count;
            embedded_postings = other.embedded_postings;
            lazy_posting_stream = other.lazy_posting_stream;

            other.type = -2;
            other.doc_count = 0;
            other.embedded_postings = nullptr;
            other.lazy_posting_stream = nullptr;
        }
        return *this;
    }

    ~PostingListStream()
    {
        if (embedded_postings)
            embedded_postings->~PostingListBitmap();
        if (lazy_posting_stream)
            lazy_posting_stream->~LazyPostingStream();
    }

    void read(
        ReadBuffer & in,
        LargePostingListReaderStreamPtr stream,
        const MergedPartOffsets * merged_part_offsets,
        size_t part_index,
        Arena * arena);

    void write(WriteBuffer & wb, LargePostingListWriterStream & stream, const MergeTreeIndexTextParams & index_params) const;
};

static_assert(sizeof(PostingListStream) <= 40, "PostingListStream must be less than 40 bytes");

struct PostingListData
{
    union Storage
    {
        PostingListWriter writer;
        PostingListStream stream;
        PostingListInMemory posting;

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
        else
            new (&storage.posting) PostingListInMemory(std::move(other.storage.posting));
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
            else
                new (&storage.posting) PostingListInMemory(std::move(other.storage.posting));
        }
        return *this;
    }

    ~PostingListData() { destroy(); }

    void destroy()
    {
        if (storage.writer.type > 0)
            storage.posting.~PostingListInMemory();
        else if (storage.writer.type == -2)
            storage.stream.~PostingListStream();
    }

    void toWriterUnsafe() { storage.writer.type = -1; }
    void toStreamUnsafe() { storage.writer.type = -2; }

    bool isWriter() const { return storage.writer.type == -1; }
    bool isStream() const { return storage.writer.type == -2; }
    bool isPosting() const { return storage.writer.type >= 0; }

    PostingListWriter & writer() { return storage.writer; }
    PostingListStream & stream() { return storage.stream; }
    PostingListInMemory & posting() { return storage.posting; }

    const PostingListWriter & writer() const { return storage.writer; }
    const PostingListStream & stream() const { return storage.stream; }
    const PostingListInMemory & posting() const { return storage.posting; }
};

static_assert(sizeof(PostingListData) == 40, "PostingListData must be 40 bytes");

}
