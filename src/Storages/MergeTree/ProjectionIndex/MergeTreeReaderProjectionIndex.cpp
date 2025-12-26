#include <Storages/MergeTree/ProjectionIndex/MergeTreeReaderProjectionIndex.h>

#include <Storages/MergeTree/ProjectionIndex/MergeTreeIndexProjection.h>

namespace DB
{

MergeTreeReaderProjectionIndex::MergeTreeReaderProjectionIndex(
    const IMergeTreeReader * main_reader_, MergeTreeIndexWithCondition index_, NamesAndTypesList columns_, bool can_skip_mark_)
    : MergeTreeReaderTextIndex(main_reader_, index_, columns_, can_skip_mark_)
{
    auto data_part = getDataPart();
    auto index_format = index.index->getDeserializedFormat(data_part->checksums, index.index->getFileName());
    chassert(index_format);

    MergeTreeIndexDeserializationState state{
        .version = index_format.version,
        .condition = index.condition.get(),
        .part = *data_part,
        .index = *index.index,
    };

    deserialization_state = std::make_unique<MergeTreeIndexDeserializationState>(std::move(state));
}

PostingListPtr MergeTreeReaderProjectionIndex::readPostingsBlockForToken(
    std::string_view /* token */, const TokenPostingsInfo & token_info, size_t block_idx)
{
    chassert(granule);
    auto & granule_projection = assert_cast<MergeTreeIndexGranuleProjection &>(*granule);
    chassert(granule_projection.large_posting_stream);
    UInt32 last_doc_id = token_info.ranges[block_idx].begin;
    if (last_doc_id > 0)
        --last_doc_id;
    return ReaderStreamEntry::materializeLargeBlockIntoBitmap(
        *granule_projection.large_posting_stream,
        last_doc_id,
        token_info.offsets[block_idx].doc_count,
        token_info.offsets[block_idx].offset);
}

}
