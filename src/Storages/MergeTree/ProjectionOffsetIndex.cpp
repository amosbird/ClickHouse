#include <Storages/MergeTree/ProjectionOffsetIndex.h>

#include <IO/HashingWriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

void ProjectionOffsetIndexWriter::write(
    const UInt64 * parent_part_offsets,
    size_t total_rows,
    const MergeTreeIndexGranularity & index_granularity,
    IDataPartStorage & storage,
    MergeTreeDataPartChecksums & checksums)
{
    if (total_rows == 0)
        return;

    /// We use 32-bit Roaring bitmaps. If any offset exceeds UInt32 range (parent part has > 4B rows),
    /// skip building the index — the reader will fall back to the slow path.
    for (size_t i = 0; i < total_rows; ++i)
    {
        if (parent_part_offsets[i] > std::numeric_limits<UInt32>::max())
            return;
    }

    size_t num_marks = index_granularity.getMarksCountWithoutFinal();

    /// Build per-granule bitmaps.
    std::vector<roaring::Roaring> bitmaps(num_marks);

    size_t current_row = 0;
    for (size_t mark = 0; mark < num_marks && current_row < total_rows; ++mark)
    {
        size_t rows_in_mark = index_granularity.getMarkRows(mark);
        size_t end_row = std::min(current_row + rows_in_mark, total_rows);

        auto & bitmap = bitmaps[mark];
        roaring::BulkContext context;
        for (size_t row = current_row; row < end_row; ++row)
            bitmap.addBulk(context, static_cast<uint32_t>(parent_part_offsets[row]));

        bitmap.runOptimize();
        bitmap.shrinkToFit();
        current_row = end_row;
    }

    /// Serialize to file.
    /// Format: [version: UInt8] [num_marks: UInt64] [sizes: UInt64 * num_marks] [bitmap_data...]
    auto out = storage.writeFile(PROJECTION_OFFSET_INDEX_FILE_NAME, 4096, {});
    HashingWriteBuffer hashing(*out);

    writeBinaryLittleEndian(PROJECTION_OFFSET_INDEX_VERSION, hashing);
    writeBinaryLittleEndian(static_cast<UInt64>(num_marks), hashing);

    /// First, compute sizes of all serialized bitmaps.
    std::vector<UInt64> bitmap_sizes(num_marks);
    for (size_t mark = 0; mark < num_marks; ++mark)
        bitmap_sizes[mark] = bitmaps[mark].getSizeInBytes();

    /// Write sizes.
    for (size_t mark = 0; mark < num_marks; ++mark)
        writeBinaryLittleEndian(bitmap_sizes[mark], hashing);

    /// Write bitmap data.
    for (size_t mark = 0; mark < num_marks; ++mark)
    {
        size_t size = bitmap_sizes[mark];
        if (size > 0)
        {
            PODArray<char> buf(size);
            bitmaps[mark].write(buf.data());
            hashing.write(buf.data(), size);
        }
    }

    hashing.finalize();

    checksums.files[PROJECTION_OFFSET_INDEX_FILE_NAME].file_size = hashing.count();
    checksums.files[PROJECTION_OFFSET_INDEX_FILE_NAME].file_hash = hashing.getHash();

    out->preFinalize();
    out->finalize();
}

ProjectionIndexBitmapPtr ProjectionOffsetIndexReader::read(
    const IDataPartStorage & storage,
    const MarkRanges & mark_ranges,
    const PartOffsetRanges & parent_ranges,
    UInt64 max_part_offset)
{
    if (!storage.existsFile(PROJECTION_OFFSET_INDEX_FILE_NAME))
        return nullptr;

    /// The on-disk bitmaps use 32-bit Roaring. If the parent part has more than 4B rows,
    /// the stored offsets would be truncated and incorrect — fall back to the slow path.
    if (max_part_offset > std::numeric_limits<UInt32>::max())
        return nullptr;

    auto in = storage.readFile(PROJECTION_OFFSET_INDEX_FILE_NAME, {}, std::nullopt);

    /// Read header.
    UInt8 version = 0;
    readBinaryLittleEndian(version, *in);
    if (version != PROJECTION_OFFSET_INDEX_VERSION)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "Unsupported projection offset index version: {}, expected: {}",
            static_cast<int>(version), static_cast<int>(PROJECTION_OFFSET_INDEX_VERSION));

    UInt64 num_marks = 0;
    readBinaryLittleEndian(num_marks, *in);

    /// Read bitmap sizes.
    std::vector<UInt64> bitmap_sizes(num_marks);
    for (UInt64 i = 0; i < num_marks; ++i)
        readBinaryLittleEndian(bitmap_sizes[i], *in);

    /// Compute byte offsets of each bitmap in the data section.
    std::vector<UInt64> bitmap_offsets(num_marks);
    UInt64 current_offset = 0;
    for (UInt64 i = 0; i < num_marks; ++i)
    {
        bitmap_offsets[i] = current_offset;
        current_offset += bitmap_sizes[i];
    }

    /// Determine which marks to read based on mark_ranges.
    /// Mark ranges are [begin, end) of mark indices.
    std::vector<bool> marks_to_read(num_marks, false);
    for (const auto & range : mark_ranges)
    {
        for (size_t mark = range.begin; mark < range.end && mark < num_marks; ++mark)
            marks_to_read[mark] = true;
    }

    /// We already checked max_part_offset <= UInt32::max above, so always use 32-bit bitmap.
    bool is_full_range = parent_ranges.isContiguousFullRange();

    auto result = ProjectionIndexBitmap::create32();

    /// Read bitmap data sequentially. We read the entire data section and pick out the bitmaps we need.
    /// This is simpler than seeking and still efficient since the file is typically small.
    UInt64 data_read_pos = 0;
    for (UInt64 mark = 0; mark < num_marks; ++mark)
    {
        UInt64 size = bitmap_sizes[mark];
        if (marks_to_read[mark] && size > 0)
        {
            /// Skip to this bitmap if we're behind.
            if (data_read_pos < bitmap_offsets[mark])
            {
                in->ignore(bitmap_offsets[mark] - data_read_pos);
                data_read_pos = bitmap_offsets[mark];
            }

            PODArray<char> buf(size);
            in->readStrict(buf.data(), size);
            data_read_pos += size;

            roaring::Roaring granule_bitmap = roaring::Roaring::read(buf.data());

            /// Add offsets from this granule's bitmap to the result, filtering by parent_ranges if needed.
            for (auto it = granule_bitmap.begin(); it != granule_bitmap.end(); ++it)
            {
                UInt64 offset = *it;
                if (is_full_range || parent_ranges.contains(offset))
                    result->add<UInt32>(static_cast<UInt32>(offset));
            }
        }
        else if (data_read_pos < bitmap_offsets[mark] + size)
        {
            /// Skip this bitmap's data.
            UInt64 to_skip = bitmap_offsets[mark] + size - data_read_pos;
            if (to_skip > 0)
            {
                in->ignore(to_skip);
                data_read_pos += to_skip;
            }
        }
    }

    return result;
}

}
