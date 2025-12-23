#pragma once

#include <base/defines.h>
#include <base/types.h>

#include <roaring/roaring.hh>

namespace DB
{

class Arena;
class ReadBuffer;
class WriteBuffer;
class MergedPartOffsets;

struct alignas(16) PostingListChunk
{
    PostingListChunk * next = nullptr;
    size_t len;

    explicit PostingListChunk(size_t len_) noexcept
        : len(len_)
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
    void finish(WriteBuffer & wb, uint8_t * packed_buffer) const;
};

static_assert(sizeof(PostingListWriter) == 40, "PostingListWriter must be 40 bytes");

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

    void addMany(UInt32 * docs, size_t num)
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

    void merge(const PostingListInMemory & other)
    {
        if (this->empty())
        {
            /// TODO(amos): Is this copy necessary?
            *this = other;
            return;
        }

        if (other.empty())
            return;

        if (!this->isBitmap() && !other.isBitmap())
        {
            if (!m.set.merge(other.m.set))
            {
                PostingListBitmap bitmap(m.set.m_size, m.set.container);
                bitmap.addMany(other.m.set.m_size, other.m.set.container);
                new (&m.bitmap) PostingListBitmap(std::move(bitmap));
            }
        }
        else
        {
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

struct RowsRange;

struct alignas(8) PostingListReader
{
    Int32 type = -2;
    UInt32 doc_count = 0;

    UInt64 * offsets = nullptr;
    RowsRange * ranges = nullptr;
    PostingListInMemory * embedded_postings = nullptr;
};

struct PostingListData
{
    union Storage
    {
        PostingListWriter writer;
        PostingListInMemory posting;

        Storage() { }
        ~Storage() { }
    } storage;

    /// Zero initialized
    PostingListData() { new (&storage.posting) PostingListInMemory(); }

    explicit PostingListData(bool /* is_merge */) { new (&storage.posting) PostingListInMemory(); }

    PostingListData(const PostingListData & other)
    {
        if (other.isWriter())
            new (&storage.writer) PostingListWriter(other.storage.writer);
        else
            new (&storage.posting) PostingListInMemory(other.storage.posting);
    }

    PostingListData & operator=(const PostingListData & other)
    {
        if (this != &other)
        {
            destroy();
            if (other.isWriter())
                new (&storage.writer) PostingListWriter(other.storage.writer);
            else
                new (&storage.posting) PostingListInMemory(other.storage.posting);
        }
        return *this;
    }

    PostingListData(PostingListData && other) noexcept
    {
        if (other.isWriter())
            new (&storage.writer) PostingListWriter(std::move(other.storage.writer));
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
    }

    void toWriterUnsafe() { storage.writer.type = -1; }

    bool isWriter() const { return storage.writer.type == -1; }

    PostingListWriter & writer() { return storage.writer; }
    PostingListInMemory & posting() { return storage.posting; }

    const PostingListWriter & writer() const { return storage.writer; }
    const PostingListInMemory & posting() const { return storage.posting; }
};

static_assert(sizeof(PostingListData) == 40, "PostingListData must be 40 bytes");

}
