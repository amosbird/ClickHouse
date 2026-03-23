#pragma once

#include <Storages/MergeTree/MergeTreeIndexReadResultPool.h>
#include <Storages/MergeTree/IDataPartStorage.h>
#include <Storages/MergeTree/MergeTreeDataPartChecksum.h>
#include <Storages/MergeTree/MarkRange.h>
#include <Storages/MergeTree/RangesInDataPart.h>

#include <roaring/roaring.hh>

namespace DB
{

/// Pre-built per-granule index of `_parent_part_offset` values for projection parts.
///
/// During write, for each mark (granule) in the projection part, we build a Roaring bitmap
/// containing the `_parent_part_offset` values of all rows in that granule. These bitmaps
/// are serialized to a single file alongside the projection part data.
///
/// At query time, instead of reading the `_parent_part_offset` column and applying PREWHERE
/// to build a bitmap (the "slow path"), we directly load the pre-built bitmaps for candidate
/// marks selected by KeyCondition, and union them into the result bitmap.
///
/// File format (version 1):
///   [version: UInt8]
///   [num_marks: UInt64]
///   [bitmap_size_0: UInt64] [bitmap_size_1: UInt64] ... [bitmap_size_{num_marks-1}: UInt64]
///   [bitmap_0_bytes] [bitmap_1_bytes] ...
///
/// This is analogous to the text index's "direct read" mode but for projection indexes.

static constexpr auto PROJECTION_OFFSET_INDEX_FILE_NAME = "parent_offset.idx";
static constexpr UInt8 PROJECTION_OFFSET_INDEX_VERSION = 1;

class ProjectionOffsetIndexWriter
{
public:
    /// Build the per-granule bitmap index from the `_parent_part_offset` column data.
    ///
    /// @param parent_part_offsets  The `_parent_part_offset` column data (after permutation/sort).
    ///                             Must be in the same row order as the written projection part.
    /// @param total_rows           Total number of rows written.
    /// @param index_granularity    The index granularity of the projection part.
    /// @param storage              The projection part storage to write the index file to.
    /// @param checksums            Checksums to update with the new file.
    static void write(
        const UInt64 * parent_part_offsets,
        size_t total_rows,
        const MergeTreeIndexGranularity & index_granularity,
        IDataPartStorage & storage,
        MergeTreeDataPartChecksums & checksums);
};

class ProjectionOffsetIndexReader
{
public:
    /// Read the projection offset index and build a combined bitmap for the given mark ranges.
    ///
    /// @param storage           The projection part storage.
    /// @param mark_ranges       Mark ranges selected by KeyCondition (from projection part's primary key analysis).
    /// @param parent_ranges     The parent part offset ranges to intersect with.
    /// @param max_part_offset   Maximum part offset, used to choose 32-bit vs 64-bit bitmap.
    /// @return                  A ProjectionIndexBitmap with the union of all matching offsets,
    ///                          or nullptr if the index file does not exist.
    static ProjectionIndexBitmapPtr read(
        const IDataPartStorage & storage,
        const MarkRanges & mark_ranges,
        const PartOffsetRanges & parent_ranges,
        UInt64 max_part_offset);
};

}
