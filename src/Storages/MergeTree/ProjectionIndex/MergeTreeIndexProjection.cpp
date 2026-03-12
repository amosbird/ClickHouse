#include <Storages/MergeTree/ProjectionIndex/MergeTreeIndexProjection.h>

#include <Columns/ColumnAggregateFunction.h>
#include <Interpreters/Context.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/IMergeTreeReader.h>
#include <Storages/MergeTree/LoadedMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeDataPartChecksum.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListState.h>
#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexText.h>
#include <Storages/MergeTree/TextIndexCache.h>
#include <Storages/ProjectionsDescription.h>

namespace ProfileEvents
{
    extern const Event TextIndexReadDictionaryBlocks;
    extern const Event TextIndexReadGranulesMicroseconds;
    extern const Event TextIndexReadPostings;
    extern const Event TextIndexUsedEmbeddedPostings;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

MergeTreeIndexGranuleProjection::MergeTreeIndexGranuleProjection(const String & projection_name_)
    : MergeTreeIndexGranuleText({}, {})
    , projection_name(projection_name_)
{
}

MergeTreeIndexGranuleProjection::~MergeTreeIndexGranuleProjection() = default;

void MergeTreeIndexGranuleProjection::deserializeBinaryWithMultipleStreams(
    MergeTreeIndexInputStreams & /* streams */, MergeTreeIndexDeserializationState & state)
{
    ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::TextIndexReadGranulesMicroseconds);

    for (const auto & [name, proj_part] : state.part.getProjectionParts())
    {
        if (name == projection_name)
        {
            projection_part = proj_part;
            break;
        }
    }

    if (!projection_part)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Projection part '{}' not found while deserializing projection index in part '{}'",
            projection_name,
            state.part.name);
    }

    if (index_id_for_caches.empty())
    {
        const auto & part_storage = state.part.getDataPartStorage();
        index_id_for_caches = fmt::format("{}:{}:{}", part_storage.getDiskName(), part_storage.getFullPath(), projection_name);
    }

    auto tokens_to_read = fillTokensFromCache(state);
    if (tokens_to_read.empty())
        return;

    DictionaryBlockBase sparse_index(projection_part->getIndex()->at(0));
    if (sparse_index.empty())
        return;

    const auto & condition_text = typeid_cast<const MergeTreeIndexConditionText &>(*state.condition);
    auto global_search_mode = condition_text.getGlobalSearchMode();
    auto tokens_cache = condition_text.tokensCache();

    /// Map each token to a mark (granule) that should contain it, using the sparse index.
    /// Group tokens by mark for batch reading.
    std::sort(tokens_to_read.begin(), tokens_to_read.end());
    std::vector<std::pair<size_t, std::vector<std::string_view>>> marks_to_read;

    for (const auto & token : tokens_to_read)
    {
        size_t pos = sparse_index.upperBound(token);

        if (pos == 0)
        {
            if (global_search_mode == TextSearchMode::All)
                return;
            continue;
        }

        size_t mark = pos - 1;

        if (pos == sparse_index.size())
        {
            if (sparse_index.tokens->getDataAt(mark) == token)
            {
                /// Special handling for the last mark:
                /// upperBound() lands past the end, but the matching granule
                /// is the one before the final mark.
                if (projection_part->index_granularity->hasFinalMark())
                    --mark;
            }
            else
            {
                if (global_search_mode == TextSearchMode::All)
                    return;
                continue;
            }
        }

        if (marks_to_read.empty() || marks_to_read.back().first != mark)
            marks_to_read.emplace_back(mark, std::vector<std::string_view>());

        marks_to_read.back().second.emplace_back(token);
    }

    if (marks_to_read.empty())
        return;

    StorageMetadataPtr metadata_ptr = projection_part->storage.getInMemoryMetadataPtr();
    StorageSnapshotPtr storage_snapshot_ptr = std::make_shared<StorageSnapshot>(projection_part->storage, metadata_ptr);
    auto alter_conversions = std::make_shared<AlterConversions>();
    auto part_info = std::make_shared<LoadedMergeTreeDataPartInfoForReader>(projection_part, alter_conversions);
    auto cols = projection_part->getColumns();
    MergeTreeReaderPtr reader = createMergeTreeReader(
        part_info,
        cols,
        storage_snapshot_ptr,
        projection_part->storage.getSettings(),
        MarkRanges{MarkRange(marks_to_read.front().first, marks_to_read.back().first + 1)},
        /*virtual_fields=*/{},
        /*uncompressed_cache=*/{},
        projection_part->storage.getContext()->getMarkCache().get(),
        nullptr,
        MergeTreeReaderSettings::createFromSettings(),
        ValueSizeMap{},
        ReadBufferFromFileBase::ProfileCallback{});

    /// Read each mark's granule via the MergeTree reader, extract token infos from the
    /// PostingListData aggregate function state, then match requested tokens by binary search.
    std::optional<size_t> prev_mark;

    for (const auto & [mark, needed_tokens] : marks_to_read)
    {
        ProfileEvents::increment(ProfileEvents::TextIndexReadDictionaryBlocks);

        const size_t rows_to_read = projection_part->index_granularity->getMarkRows(mark);
        Columns result;
        result.resize(cols.size());
        size_t rows_read = reader->readRows(
            mark,
            sparse_index.size(),
            prev_mark && *prev_mark == mark - 1,
            rows_to_read,
            /*rows_offset=*/0,
            result);

        chassert(rows_read > 0);
        prev_mark = mark;

        const auto & tokens_column = assert_cast<const ColumnString &>(*result[0]);
        const ColumnAggregateFunction & posting_column = assert_cast<const ColumnAggregateFunction &>(*result[1]);
        const auto & data = posting_column.getData();
        const size_t num_tokens = data.size();
        chassert(rows_read == num_tokens);

        /// Extract has_block_index from the aggregate function params (set during part metadata deserialization).
        const auto * agg_func = dynamic_cast<const AggregateFunctionPostingList *>(
            posting_column.getAggregateFunction().get());
        if (agg_func)
            has_block_index = agg_func->params.has_block_index;

        /// Binary search for needed tokens within this granule's token column.
        size_t search_start = 0;
        for (const auto & token : needed_tokens)
        {
            /// Find the token in the sorted tokens column.
            size_t lo = search_start;
            size_t hi = num_tokens;
            size_t found_idx = num_tokens; // sentinel: not found
            while (lo < hi)
            {
                size_t mid = lo + (hi - lo) / 2;
                auto cmp = tokens_column.getDataAt(mid);
                if (cmp < token)
                    lo = mid + 1;
                else if (token < cmp)
                    hi = mid;
                else
                {
                    found_idx = mid;
                    break;
                }
            }

            if (found_idx == num_tokens)
            {
                if (global_search_mode == TextSearchMode::All)
                {
                    remaining_tokens.clear();
                    return;
                }
                continue;
            }

            search_start = found_idx;

            /// Build TokenPostingsInfo from the PostingListData at this row.
            const auto * posting_list_data = reinterpret_cast<const PostingListData *>(data[found_idx]);
            chassert(posting_list_data->isStream());
            const auto & stream = posting_list_data->stream;

            auto info = std::make_shared<TokenPostingsInfo>();
            info->cardinality = stream.doc_count;
            if (stream.doc_count == 0)
            {
                /// Ignore empty token
            }
            else if (stream.doc_count <= MAX_SIZE_OF_EMBEDDED_POSTINGS)
            {
                info->embedded_postings = std::make_shared<PostingList>();
                info->embedded_postings->addMany(info->cardinality, stream.embedded_postings);
                info->ranges.emplace_back(stream.embedded_postings[0], stream.embedded_postings[info->cardinality - 1]);
            }
            else
            {
                chassert(stream.lazy_posting_stream);
                chassert(stream.lazy_posting_stream->merged_embedded_postings.empty());
                chassert(stream.lazy_posting_stream->streams.size() == 1);

                const auto & entry = stream.lazy_posting_stream->streams.entries.front();
                size_t num_large_blocks = entry.large_posting_blocks.size();
                chassert(num_large_blocks > 0);

                info->offsets.reserve(num_large_blocks);
                info->ranges.reserve(num_large_blocks);
                UInt32 range_begin = entry.first_doc_id;
                for (size_t b = 0; b < num_large_blocks; ++b)
                {
                    info->offsets.push_back(entry.large_posting_blocks[b]);
                    info->ranges.emplace_back(range_begin, entry.large_posting_blocks[b].last_doc_id);
                    range_begin = entry.large_posting_blocks[b].last_doc_id + 1;
                }
            }

            if (info->cardinality > 0)
            {
                String token_str(token);
                auto token_hash = TextIndexTokensCache::hash(index_id_for_caches, token_str);
                tokens_cache->set(token_hash, info);
                remaining_tokens.emplace(std::move(token_str), std::move(info));
            }
        }
    }

    if (remaining_tokens.empty())
        return;

    const String & data_path = state.part.getDataPartStorage().getFullPath();
    for (const auto & [token, token_info] : remaining_tokens)
    {
        if (token_info->embedded_postings)
        {
            ProfileEvents::increment(ProfileEvents::TextIndexUsedEmbeddedPostings);
            rare_tokens_postings.emplace(token, token_info->embedded_postings);
        }
        else if (token_info->offsets.size() == 1)
        {
            /// When the block index is present and the token has high cardinality,
            /// skip materializing the posting list into a Roaring bitmap.
            /// The lazy apply mode decodes directly from TurboPFor via `PostingListCursor`,
            /// and mark evaluation uses the range-based `hasAnyRange` check which is
            /// sufficient for common tokens (false positives are cheap — the cursor
            /// simply produces no matching doc_ids for empty ranges).
            ///
            /// For rare tokens (low cardinality), materialization is cheap and
            /// enables precise mark filtering that avoids scanning many empty marks.
            ///
            /// Threshold: skip when cardinality > 8192 (one granule worth of rows).
            /// This avoids the O(cardinality) cost of `roaring_bitmap_add_many`
            /// (e.g., ~5ms for 897K doc_ids) while keeping mark filtering for rare tokens.
            static constexpr UInt32 MATERIALIZATION_SKIP_THRESHOLD = 8192;
            if (has_block_index && token_info->cardinality > MATERIALIZATION_SKIP_THRESHOLD)
                continue;

            if (!large_posting_stream)
            {
                large_posting_stream = reader->getProjectionIndexPostingStreamPtr();
                chassert(large_posting_stream);
            }

            const auto load_postings = [&]() -> PostingListPtr
            {
                ProfileEvents::increment(ProfileEvents::TextIndexReadPostings);
                return materializeFromTokenInfo(*large_posting_stream, *token_info, 0);
            };

            auto hash = TextIndexPostingsCache::hash(data_path, projection_part->name, token_info->offsets[0].offset);
            auto p = condition_text.postingsCache()->getOrSet(hash, load_postings);
            rare_tokens_postings.emplace(token, std::move(p));
        }
    }
}

PostingListPtr MergeTreeIndexGranuleProjection::materializeFromTokenInfo(
    LargePostingListReaderStream & stream, const TokenPostingsInfo & token_info, size_t block_idx)
{
    /// For delta-decoding:
    /// - First block: 'begin' is the first doc_id (include it).
    /// - Other blocks: 'begin - 1' is the baseline to reconstruct 'begin' (exclude it).
    UInt32 last_doc_id = static_cast<UInt32>(token_info.ranges[block_idx].begin);
    if (block_idx > 0)
        --last_doc_id;

    return ReaderStreamEntry::materializeLargeBlockIntoBitmap(
        stream,
        last_doc_id,
        token_info.offsets[block_idx].block_doc_count,
        token_info.offsets[block_idx].offset,
        block_idx == 0 /* include_first_doc */);
}

namespace
{
    const IndexDescription & getIndexDescriptionOrThrow(const ProjectionDescription & projection)
    {
        if (!projection.index)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Projection index is not initialized");

        const auto * index_desc = projection.index->getIndexDescription();
        if (!index_desc)
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Projection index {} should have index description initialized. It is a bug",
                projection.index->getName());
        }

        return *index_desc;
    }
}

MergeTreeIndexProjection::MergeTreeIndexProjection(
    const ProjectionDescription & projection, std::shared_ptr<const MergeTreeIndexText> text_index_)
    : IMergeTreeIndex(getIndexDescriptionOrThrow(projection))
    , text_index(std::move(text_index_))
{
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
    if (checksums.files.contains(index.name + ".proj"))
        return {1, {{}}};

    return {0 /*unknown*/, {}};
}

MergeTreeIndexGranulePtr MergeTreeIndexProjection::createIndexGranule() const
{
    return std::make_shared<MergeTreeIndexGranuleProjection>(index.name);
}

MergeTreeIndexAggregatorPtr MergeTreeIndexProjection::createIndexAggregator() const
{
    throw Exception(ErrorCodes::LOGICAL_ERROR, "MergeTreeIndexProjection cannot create aggregator");
}

MergeTreeIndexConditionPtr MergeTreeIndexProjection::createIndexCondition(const ActionsDAG::Node * predicate, ContextPtr context) const
{
    return std::make_shared<MergeTreeIndexConditionText>(
        predicate, context, index.sample_block, text_index->tokenizer.get(), text_index->preprocessor);
}

}
