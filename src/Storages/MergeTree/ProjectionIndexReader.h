#pragma once

#include <Core/Settings.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <Storages/MergeTree/MergeTreeReadPoolProjectionIndex.h>
#include <Storages/MergeTree/MergeTreeSelectAlgorithms.h>
#include <Storages/MergeTree/MergeTreeSelectProcessor.h>
#include <Storages/MergeTree/MergeTreeSource.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageSnapshot.h>

#include <shared_mutex>

namespace DB
{

namespace Setting
{
    extern const SettingsBool allow_asynchronous_read_from_io_pool_for_merge_tree;
    extern const SettingsBool apply_deleted_mask;
    extern const SettingsBool checksum_on_read;
    extern const SettingsBool enable_multiple_prewhere_read_steps;
    extern const SettingsFloat max_streams_to_max_threads_ratio;
    extern const SettingsUInt64 max_streams_for_merge_tree_reading;
    extern const SettingsUInt64 merge_tree_max_bytes_to_use_cache;
    extern const SettingsUInt64 merge_tree_max_rows_to_use_cache;
    extern const SettingsUInt64 preferred_block_size_bytes;
    extern const SettingsUInt64 preferred_max_column_in_block_size_bytes;
    extern const SettingsBool use_uncompressed_cache;
    extern const SettingsBool query_plan_merge_filters;
    extern const SettingsBool use_query_condition_cache;
    extern const SettingsBool query_condition_cache_store_conditions_as_plaintext;
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool merge_tree_use_deserialization_prefixes_cache;
    extern const SettingsBool merge_tree_use_prefixes_deserialization_thread_pool;
}

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

struct ProjectionIndexReader
{
    ProjectionIndexReader(
        const StorageSnapshotPtr & storage_snapshot,
        const MergeTreeData & merge_tree_,
        const PrewhereInfoPtr & prewhere_info_,
        ProjectionDescriptionRawPtr projection_,
        ContextPtr context,
        size_t query_terms_)
        : cache_mutex(std::make_unique<std::shared_mutex>())
        , merge_tree(merge_tree_)
        , prewhere_info(prewhere_info_)
        , projection(projection_)
        , query_terms(query_terms_)
    {
    }

    void read(QueryPlan & query_plan, DataPartPtr part) const
    {
        MergeTreeSelectAlgorithmPtr algorithm = std::make_unique<MergeTreeProjectionIndexSelectAlgorithm>(part);
        auto processor = std::make_unique<MergeTreeSelectProcessor>(
            pool, std::move(algorithm), prewhere_info, nullptr, actions_settings, reader_settings);
        Pipe pipe(std::make_shared<MergeTreeSource>(std::move(processor)));
        auto read_from_pipe = std::make_unique<ReadFromPreparedSource>(std::move(pipe));
        read_from_pipe->setStepDescription(fmt::format("Read from ProjectionIndex({}:{})", projection->name, part->name));
        query_plan.addStep(std::move(read_from_pipe));
    }

    struct CacheHolder
    {
        CacheHolder(std::promise<ColumnPtr> * promise_, std::shared_future<ColumnPtr> future_)
            : promise(promise_)
            , future(std::move(future_))
        {
        }

        std::promise<ColumnPtr> * promise;
        std::shared_future<ColumnPtr> future;
    };

    struct PostingListHolder
    {
        std::promise<ColumnPtr> promise;
        std::shared_future<ColumnPtr> future = promise.get_future().share();
    };

    CacheHolder getHolder(const DataPartPtr & part) const
    {
        std::lock_guard lock(*cache_mutex);
        auto it = cache.find(part.get());
        if (it == cache.end())
        {
            it = cache.emplace(part.get(), PostingListHolder{}).first;
            return CacheHolder(&it->second.promise, it->second.future);
        }
        else
        {
            return CacheHolder(nullptr, it->second.future);
        }
    }

    template <typename Func>
    void read(DataPartPtr part, Func && func) const
    {
        /// TODO(amos): Implement adaptive index reading vs full scan based on mark range count
        auto holder = getHolder(part);
        if (holder.promise)
        {
            try
            {
                MergeTreeSelectAlgorithmPtr algorithm = std::make_unique<MergeTreeProjectionIndexSelectAlgorithm>(part);
                auto processor = std::make_unique<MergeTreeSelectProcessor>(
                    pool, std::move(algorithm), prewhere_info, nullptr, actions_settings, reader_settings);

                ColumnPtr result;
                while (true)
                {
                    auto chunk = processor->read();
                    if (chunk.chunk)
                    {
                        if (chunk.chunk.getNumRows() > 0)
                        {
                            if (!func(chunk.chunk, result, query_terms))
                                break; /// Cancelled
                        }
                    }

                    if (chunk.is_finished)
                    {
                        Chunk dummy;
                        func(dummy, result, query_terms);
                        break;
                    }
                }

                holder.promise->set_value(std::move(result));
            }
            catch (...)
            {
                holder.promise->set_value(nullptr);
                throw;
            }
        }
        else
        {
            Chunk dummy;
            ColumnPtr result = holder.future.get();
            if (result)
                func(dummy, result, query_terms);
        }
    }

    mutable std::unordered_map<const IMergeTreeDataPart *, PostingListHolder> cache;
    mutable std::unique_ptr<std::shared_mutex> cache_mutex;

    const MergeTreeData & merge_tree;
    PrewhereInfoPtr prewhere_info;
    ProjectionDescriptionRawPtr projection;
    size_t query_terms;
    ExpressionActionsSettings actions_settings;
    MergeTreeReaderSettings reader_settings;
    MergeTreeReadTask::BlockSizeParams block_size;
    std::shared_ptr<MergeTreeReadPoolProjectionIndex> pool;
};

struct ProjectionIndexReaderMap : public std::unordered_map<String, ProjectionIndexReader>
{
    explicit ProjectionIndexReaderMap(StorageMetadataPtr metadata_snapshot_)
        : metadata_snapshot(std::move(metadata_snapshot_))
    {
    }
    StorageMetadataPtr metadata_snapshot;
};

}
