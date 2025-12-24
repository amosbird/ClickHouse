#pragma once

#include <AggregateFunctions/IAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>

namespace DB
{

class AggregateFunctionPostingList final : public IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>
{
    using Data = PostingListData;

public:
    explicit AggregateFunctionPostingList(const Array & params, const MergeTreeIndexTextParams & index_params_)
        : IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>({}, params, createResultType())
        , index_params(index_params_)
    {
    }

    static DataTypePtr createResultType() { return std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>()); }

    String getName() const override { return "postingList"; }

    void add(AggregateDataPtr, const IColumn **, size_t, Arena *) const override;

    /// TODO(amos): Currently `rhs` is const because its state is allocated in an Arena and will be
    /// deallocated along with the Arena. This prevents moving its contents directly into `place`.
    /// Investigate whether we can support move-merge by either:
    ///   1) avoiding Arena allocation and managing memory with unique_ptr, or
    ///   2) designing a separate move-safe memory pool for aggregate states.
    ///
    /// Current workaround: using const_cast on std::unique_ptr to move. This works but may be unsafe; needs review to
    /// ensure correctness and avoid undefined behavior.
    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * /* arena */) const override
    {
        auto & lhs_posting_list_data = data(place);
        const auto & rhs_posting_list_data = data(rhs);
        chassert(lhs_posting_list_data.isStream());
        chassert(rhs_posting_list_data.isStream());
        lhs_posting_list_data.stream().merge(rhs_posting_list_data.stream());
    }

    void serialize(ConstAggregateDataPtr, WriteBuffer &, std::optional<size_t>) const override;

    void deserialize(AggregateDataPtr, ReadBuffer &, std::optional<size_t>, Arena *) const override;

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena * /* arena */) const override
    {
        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();
        const auto & posting_list_data = data(place);
        chassert(posting_list_data.isStream());
        const auto & posting_list = posting_list_data.stream();
        if (posting_list.doc_count == 0)
            return;

        offsets_to.push_back(offsets_to.back() + posting_list.doc_count);
        typename ColumnVector<UInt32>::Container & data_to = assert_cast<ColumnVector<UInt32> &>(arr_to.getData()).getData();
        size_t pos = data_to.size();
        data_to.resize(data_to.size() + posting_list.doc_count);
        posting_list.collect(&data_to[pos]);
    }

    bool allocatesMemoryInArena() const override { return false; }

    /// Build index parameters from data type arguments. This path is only valid when the type is created via
    /// getPostingListType() and is used exclusively for metadata, not for the columns.txt persisted in data parts.
    ///
    /// This is safe because these parameters are only needed when creating new data parts (during INSERT or MERGE),
    /// where metadata is used as the source of truth.
    MergeTreeIndexTextParams index_params;
};

DataTypePtr getPostingListType(const ASTPtr & arguments);

}
