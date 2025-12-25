#include <Storages/MergeTree/ProjectionIndex/MergeTreeIndexProjection.h>

#include <Columns/ColumnAggregateFunction.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/IMergeTreeReader.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeDataPartChecksum.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>
#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexText.h>
#include <Storages/MergeTree/TextIndexCache.h>
#include <Storages/ProjectionsDescription.h>

namespace ProfileEvents
{
extern const Event TextIndexReadPostings;
}

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
extern const int INCORRECT_QUERY;
extern const int INCORRECT_NUMBER_OF_COLUMNS;
extern const int CORRUPTED_DATA;
}

MergeTreeIndexGranuleProjection::MergeTreeIndexGranuleProjection(const ProjectionDescription & projection_)
    : MergeTreeIndexGranuleText({})
    , projection(projection_)
{
}

MergeTreeIndexGranuleProjection::~MergeTreeIndexGranuleProjection() = default;

void MergeTreeIndexGranuleProjection::deserializeBinaryWithMultipleStreams(
    MergeTreeIndexInputStreams & /* streams */, MergeTreeIndexDeserializationState & state)
{
    MergeTreeDataPartPtr part;
    for (const auto & [name, projection_part] : state.part.getProjectionParts())
    {
        if (name == projection.name)
        {
            part = projection_part;
            break;
        }
    }

    if (!part)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Projection part '{}' not found while deserializing projection index in part '{}'",
            projection.name,
            state.part.name);
    }

    DictionaryBlockBase sparse_index(part->getIndex()->at(0));
    if (sparse_index.empty())
        return;

    const auto & condition_text = typeid_cast<const MergeTreeIndexConditionText &>(*state.condition);
    auto global_search_mode = condition_text.getGlobalSearchMode();
    const auto & all_search_tokens = condition_text.getAllSearchTokens();
    std::map<size_t, std::vector<std::string_view>> mark_to_tokens;

    for (const auto & token : all_search_tokens)
    {
        size_t mark = sparse_index.upperBound(token);

        if (mark == 0 || mark == sparse_index.size())
        {
            if (global_search_mode == TextSearchMode::All)
                return;
            continue;
        }

        --mark;
        mark_to_tokens[mark].emplace_back(token);
    }

    if (mark_to_tokens.empty())
        return;

    StorageMetadataPtr metadata_ptr = part->storage.getInMemoryMetadataPtr();
    StorageSnapshotPtr storage_snapshot_ptr = std::make_shared<StorageSnapshot>(part->storage, metadata_ptr);
    auto alter_conversions = std::make_shared<AlterConversions>();
    auto part_info = std::make_shared<LoadedMergeTreeDataPartInfoForReader>(part, alter_conversions);
    auto cols = part->getColumns();
    MergeTreeReaderPtr reader = createMergeTreeReader(
        part_info,
        cols,
        storage_snapshot_ptr,
        part->storage.getSettings(),
        MarkRanges{MarkRange(mark_to_tokens.begin()->first, mark_to_tokens.rbegin()->first + 1)},
        /*virtual_fields=*/{},
        /*uncompressed_cache=*/{},
        part->storage.getContext()->getMarkCache().get(),
        nullptr,
        MergeTreeReaderSettings::createFromSettings(),
        ValueSizeMap{},
        ReadBufferFromFileBase::ProfileCallback{});

    /// TODO(amos): here we shoud get LargePostingListReaderStream from reader so that it can be used later to fill in rare_tokens_postings

    std::optional<size_t> prev_mark;
    const auto get_dictionary_block = [&](size_t mark)
    {
        const auto load_dictionary_block = [&] -> TextIndexDictionaryBlockCacheEntryPtr
        {
            const size_t rows_to_read = part->index_granularity->getMarkRows(mark);

            Columns result;
            result.resize(cols.size());
            size_t rows_read = reader->readRows(
                mark,
                part->getMarksCount(),
                prev_mark && *prev_mark == mark - 1,
                rows_to_read,
                /*rows_offset=*/0,
                result);

            chassert(rows_read > 0);
            prev_mark = mark;

            assert_cast<const ColumnString &>(*result[0]);
            const ColumnAggregateFunction & posting_column = assert_cast<const ColumnAggregateFunction &>(*result[1]);
            const auto & data = posting_column.getData();
            const size_t rows = data.size();
            std::vector<TokenPostingsInfo> token_infos;
            token_infos.reserve(rows);

            for (size_t i = 0; i < rows; ++i)
            {
                const auto * posting_list_data = reinterpret_cast<const PostingListData *>(data[i]);
                chassert(posting_list_data->isStream());
                const auto & stream = posting_list_data->stream();

                TokenPostingsInfo info;
                // info.header =
                info.cardinality = stream.doc_count;
                if (stream.doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
                {
                    info.embedded_postings = std::make_shared<PostingList>();
                    info.embedded_postings->addMany(info.cardinality, stream.embedded_postings);
                }
                else
                {
                    chassert(stream.lazy_posting_stream);
                    chassert(stream.lazy_posting_stream->merged_embedded_postings.empty());
                    chassert(stream.lazy_posting_stream->streams.size() == 1);

                    const auto & entry = stream.lazy_posting_stream->streams.entries.front();
                    size_t num_large_blocks = entry.large_posting_blocks.size();
                    chassert(num_large_blocks > 0);

                    info.offsets.reserve(num_large_blocks - 1);
                    info.ranges.reserve(num_large_blocks - 1);
                    UInt32 last_doc_id = entry.large_posting_blocks[0].last_doc_id;
                    for (size_t b = 1; b < num_large_blocks; ++b)
                    {
                        info.offsets.push_back(entry.large_posting_blocks[b].offset);
                        info.ranges.emplace_back(last_doc_id, entry.large_posting_blocks[b].last_doc_id);
                        last_doc_id = entry.large_posting_blocks[b].last_doc_id;
                    }
                }

                /// TODO(amos): here we should extend info to store LargePostingBlockMeta so that posting list stream can be decoded
                token_infos.emplace_back(std::move(info));
            }

            return std::make_shared<TextIndexDictionaryBlockCacheEntry>(DictionaryBlock(std::move(result[0]), std::move(token_infos)));
        };

        auto hash = TextIndexDictionaryBlockCache::hash(state.part.getDataPartStorage().getFullPath(), part->name, mark);
        return condition_text.dictionaryBlockCache()->getOrSet(hash, load_dictionary_block);
    };

    for (const auto & [mark, tokens] : mark_to_tokens)
    {
        const auto dictionary_block = get_dictionary_block(mark);

        for (const auto & token : tokens)
        {
            auto * token_info = dictionary_block->getTokenInfo(token);

            if (token_info)
            {
                remaining_tokens.emplace(token, *token_info);
            }
            else if (global_search_mode == TextSearchMode::All)
            {
                remaining_tokens.clear();
                return;
            }
        }
    }
}

// PostingListPtr MergeTreeIndexGranuleProjection::readPostingsBlock(
//     MergeTreeIndexReaderStream & stream, MergeTreeIndexDeserializationState & state, const TokenPostingsInfo & token_info, size_t block_idx)
// {
//     auto * data_buffer = stream.getDataBuffer();

//     const String & data_path = state.part.getDataPartStorage().getFullPath();
//     const String & index_name = state.index.getFileName();
//     const auto & condition_text = assert_cast<const MergeTreeIndexConditionText &>(*state.condition);

//     const auto load_postings = [&]() -> PostingListPtr
//     {
//         ProfileEvents::increment(ProfileEvents::TextIndexReadPostings);
//         stream.seekToMark({token_info.offsets[block_idx], 0});
//         return PostingsSerialization::deserialize(*data_buffer, token_info.header, token_info.cardinality);
//     };

//     auto hash = TextIndexPostingsCache::hash(data_path, index_name, token_info.offsets[block_idx]);
//     return condition_text.postingsCache()->getOrSet(hash, load_postings);
// }

MergeTreeIndexProjection::MergeTreeIndexProjection(const ProjectionDescription & projection_)
    : IMergeTreeIndex(
          [&]
          {
              if (!projection_.index)
                  throw Exception(ErrorCodes::LOGICAL_ERROR, "Projection index is not initialized");
              return projection_.index->getIndexDescription();
          }())
    , projection(projection_)
{
    if (const auto * projection_text_index = dynamic_cast<const ProjectionIndexText *>(projection_.index.get()))
    {
        text_index = projection_text_index->getTextIndex();
        if (!text_index)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Text projection index is not initialized");
    }
    else
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "MergeTreeIndexProjection expects a text projection index");
    }
}

MergeTreeIndexSubstreams MergeTreeIndexProjection::getSubstreams() const
{
    throw Exception(ErrorCodes::LOGICAL_ERROR, "MergeTreeIndexProjection cannot get substreams");
}

MergeTreeIndexFormat
MergeTreeIndexProjection::getDeserializedFormat(const MergeTreeDataPartChecksums & checksums, const std::string & /* path_prefix */) const
{
    /// Projection index intentionally does not return any index streams here. It relies on the MergeTree part reader
    /// for deserialization instead.
    if (checksums.files.contains(projection.name))
        return {1, {{}}};

    return {0 /*unknown*/, {}};
}

MergeTreeIndexGranulePtr MergeTreeIndexProjection::createIndexGranule() const
{
    return std::make_shared<MergeTreeIndexGranuleProjection>(projection);
}

MergeTreeIndexAggregatorPtr MergeTreeIndexProjection::createIndexAggregator() const
{
    throw Exception(ErrorCodes::LOGICAL_ERROR, "MergeTreeIndexProjection cannot create aggregator");
}

MergeTreeIndexConditionPtr MergeTreeIndexProjection::createIndexCondition(const ActionsDAG::Node * predicate, ContextPtr context) const
{
    return std::make_shared<MergeTreeIndexConditionText>(
        predicate, context, index.sample_block, text_index->token_extractor.get(), text_index->preprocessor);
}

}
