#pragma once

#include <AggregateFunctions/IAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>

namespace DB
{

class AggregateFunctionPostingList final : public IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>
{
    using Data = PostingListData;

public:
    explicit AggregateFunctionPostingList()
        : IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>({}, {}, createResultType())
    {
    }

    static DataTypePtr createResultType() { return std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt32>()); }

    String getName() const override { return "postingList"; }

    void add(AggregateDataPtr, const IColumn **, size_t, Arena *) const override;

    /// TODO(amos): should we use arena to allocate memory inside roaring?
    /// TODO(amos): we should also use this to merge streams
    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * /* arena */) const override
    {
        auto & lhs_posting_list_data = data(place);
        const auto & rhs_posting_list_data = data(rhs);
        chassert(!lhs_posting_list_data.isWriter());
        chassert(!rhs_posting_list_data.isWriter());
        lhs_posting_list_data.posting().merge(rhs_posting_list_data.posting());
    }

    void serialize(ConstAggregateDataPtr, WriteBuffer &, std::optional<size_t>) const override;

    void deserialize(AggregateDataPtr, ReadBuffer &, std::optional<size_t>, Arena *) const override;

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena * /* arena */) const override
    {
        ColumnArray & arr_to = assert_cast<ColumnArray &>(to);
        ColumnArray::Offsets & offsets_to = arr_to.getOffsets();
        const auto & posting_list_data = data(place);
        chassert(!posting_list_data.isWriter());
        const auto & posting_list = posting_list_data.posting();
        if (posting_list.isBitmap())
        {
            size_t num_docs = posting_list.m.bitmap.cardinality();
            offsets_to.push_back(offsets_to.back() + num_docs);
            typename ColumnVector<UInt32>::Container & data_to = assert_cast<ColumnVector<UInt32> &>(arr_to.getData()).getData();
            size_t pos = data_to.size();
            data_to.resize(data_to.size() + num_docs);
            for (UInt32 doc : posting_list.m.bitmap)
                data_to[pos++] = doc;
        }
        else
        {
            size_t num_docs = posting_list.m.set.m_size;
            offsets_to.push_back(offsets_to.back() + num_docs);
            typename ColumnVector<UInt32>::Container & data_to = assert_cast<ColumnVector<UInt32> &>(arr_to.getData()).getData();
            data_to.insert(posting_list.m.set.begin(), posting_list.m.set.end());
        }
    }

    bool allocatesMemoryInArena() const override { return false; }
};

}
