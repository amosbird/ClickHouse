#include <DataTypes/DataTypeFactory.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListState.h>

#include <Columns/ColumnAggregateFunction.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeCustom.h>
#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexDeserializationContext.h>
#include <Common/Arena.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

void AggregateFunctionPostingList::add(AggregateDataPtr, const IColumn **, size_t, Arena *) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot add in memory posting list for the moment");
}
void AggregateFunctionPostingList::serialize(ConstAggregateDataPtr, WriteBuffer &, std::optional<size_t>) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot serialize in memory posting list for the moment");
}

void AggregateFunctionPostingList::deserialize(AggregateDataPtr, ReadBuffer &, std::optional<size_t>, Arena *) const
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot deserialize in memory posting list for the moment");
}


class DataTypePostingList : public IDataTypeCustomName
{
public:
    String getName() const override { return "PostingList"; }
};

/// Serialization for PostingList values stored in dictionary blocks of the projection-based text index.
///
/// This serialization is responsible only for encoding and decoding the dictionary-level representation of posting
/// lists (e.g. headers, cardinality, and references to large posting list streams). Large posting lists are written to
/// and read from a separate index data stream owned by the projection index.
class SerializationPostingList final : public SimpleTextSerialization
{
private:
    AggregateFunctionPostingList function;

public:
    [[noreturn]] static void throwNoSerialization()
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Serialization is not implemented for type PostingList");
    }

    void serializeBinary(const Field &, WriteBuffer &, const FormatSettings &) const override { throwNoSerialization(); }
    void deserializeBinary(Field &, ReadBuffer &, const FormatSettings &) const override { throwNoSerialization(); }
    void serializeBinary(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override { throwNoSerialization(); }
    void deserializeBinary(IColumn &, ReadBuffer &, const FormatSettings &) const override { throwNoSerialization(); }
    void serializeText(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override { throwNoSerialization(); }
    void deserializeText(IColumn &, ReadBuffer &, const FormatSettings &, bool) const override { throwNoSerialization(); }
    void serializeBinaryBulk(const IColumn &, WriteBuffer &, size_t, size_t) const override { throwNoSerialization(); }
    void deserializeBinaryBulk(IColumn &, ReadBuffer &, size_t, size_t, double) const override { throwNoSerialization(); }

    /// TODO(amos): Introduce a dedicated state to represent large posting lists
    /// stored in a separate index data stream.
    ///
    /// The stream is owned by the projection index and is independent of column
    /// streams and part formats (compact or wide). It is written sequentially
    /// in append-only mode, without marks or column-level compression.
    ///
    /// Dictionary blocks store offsets into this stream, which are used during
    /// reads to seek directly to the corresponding posting lists, enabling
    /// deferred and selective loading of large postings.
    // void serializeBinaryBulkStatePrefix(
    //     const IColumn & /*column*/, SerializeBinaryBulkSettings & /*settings*/, SerializeBinaryBulkStatePtr & /*state*/) const override;

    /// TODO(amos): Evaluate whether this serialization should participate in enumerateStreams() to declare index-specific
    /// side streams (e.g. the large posting list data stream), or whether such streams should be managed exclusively by the
    /// projection index writer/reader.
    ///
    /// Note that these streams are not column substreams and are independent
    /// of compact or wide part layouts.
    // void enumerateStreams(EnumerateStreamsSettings & settings, const StreamCallback & callback, const SubstreamData & data) const override;

    void serializeBinaryBulkWithMultipleStreams(
        const IColumn & column,
        size_t offset,
        size_t limit,
        SerializeBinaryBulkSettings & settings,
        SerializeBinaryBulkStatePtr & /* state */) const override
    {
        /// TODO(amos): move this to state
        alignas(16) UInt32 doc_buffer[128];
        alignas(16) uint8_t packed_buffer[128 * 4];

        settings.path.push_back(Substream::Regular);
        if (WriteBuffer * stream = settings.getter(settings.path))
        {
            const ColumnAggregateFunction & real_column = typeid_cast<const ColumnAggregateFunction &>(column);
            const ColumnAggregateFunction::Container & vec = real_column.getData();

            size_t end = vec.size();
            if (limit)
                end = std::min(end, offset + limit);

            chassert(end > 0);

            /// Invariant: posting list data in this range is homogeneous
            if (reinterpret_cast<const PostingListData *>(vec[0])->isWriter())
            {
                for (size_t i = offset; i < end; ++i)
                {
                    const auto * posting_list_data = reinterpret_cast<const PostingListData *>(vec[i]);
                    chassert(posting_list_data->isWriter());
                    posting_list_data->writer().finish(*stream, packed_buffer);
                }
            }
            else
            {
                for (size_t i = offset; i < end; ++i)
                {
                    const auto * posting_list_data = reinterpret_cast<const PostingListData *>(vec[i]);
                    chassert(!posting_list_data->isWriter());
                    posting_list_data->posting().write(*stream, doc_buffer, packed_buffer);
                }
            }
        }
        settings.path.pop_back();
    }

    /// TODO(amos): Initialize deserialization state with index-specific resources, such as handles to the large posting
    /// list data stream.
    // void deserializeBinaryBulkStatePrefix(
    //     DeserializeBinaryBulkSettings & settings,
    //     DeserializeBinaryBulkStatePtr & state,
    //     SubstreamsDeserializeStatesCache * cache) const override;

    void deserializeBinaryBulkWithMultipleStreams(
        ColumnPtr & column,
        size_t rows_offset,
        size_t limit,
        DeserializeBinaryBulkSettings & settings,
        DeserializeBinaryBulkStatePtr & /* state */,
        SubstreamsCache * cache) const override
    {
        /// TODO(amos): move this to state
        alignas(16) UInt32 doc_buffer[128];
        alignas(16) uint8_t packed_buffer[128 * 4];

        settings.path.push_back(Substream::Regular);

        if (insertDataFromSubstreamsCacheIfAny(cache, settings, column))
        {
            /// Data was inserted from substreams cache.
        }
        else if (ReadBuffer * stream = settings.getter(settings.path))
        {
            size_t prev_size = column->size();
            auto mutable_column = column->assumeMutable();

            if (rows_offset)
            {
                throw Exception(
                    ErrorCodes::NOT_IMPLEMENTED,
                    "SerializationPostingList does not support cases where rows_offset {} is non-zero",
                    rows_offset);
            }

            ColumnAggregateFunction & real_column = typeid_cast<ColumnAggregateFunction &>(*mutable_column);
            ColumnAggregateFunction::Container & vec = real_column.getData();

            Arena & arena = real_column.createOrGetArena();
            vec.reserve(vec.size() + limit);

            size_t size_of_state = function.sizeOfData();
            size_t align_of_state = function.alignOfData();

            /// Adjust the size of state to make all states aligned in vector.
            size_t total_size_of_state = (size_of_state + align_of_state - 1) / align_of_state * align_of_state;
            char * place = arena.alignedAlloc(total_size_of_state * limit, align_of_state);

            for (size_t i = 0; i < limit; ++i)
            {
                if (stream->eof())
                    break;

                new (place) PostingListData(false /* is_merge */);

                try
                {
                    auto & posting_list_data = reinterpret_cast<PostingListData &>(*place);
                    chassert(!posting_list_data.isWriter());
                    chassert(settings.projection_index_context);
                    posting_list_data.posting().read(
                        *stream,
                        doc_buffer,
                        packed_buffer,
                        settings.projection_index_context->merged_part_offsets,
                        settings.projection_index_context->part_index);
                }
                catch (...)
                {
                    function.destroy(place);
                    throw;
                }

                vec.push_back(place);
                place += total_size_of_state;
            }

            column = std::move(mutable_column);
            addColumnWithNumReadRowsToSubstreamsCache(cache, settings.path, column, column->size() - prev_size);
        }

        settings.path.pop_back();
    }
};

DataTypePtr getPostingListType()
{
    DataTypePtr posting_list_type
        = std::make_shared<DataTypeAggregateFunction>(std::make_shared<AggregateFunctionPostingList>(), DataTypes{}, Array{});
    posting_list_type->setCustomization(
        std::make_unique<DataTypeCustomDesc>(std::make_unique<DataTypePostingList>(), std::make_shared<SerializationPostingList>()));
    return posting_list_type;
}

static std::pair<DataTypePtr, DataTypeCustomDescPtr> create(const ASTPtr & /* arguments */)
{
    return {
        std::make_shared<DataTypeAggregateFunction>(std::make_shared<AggregateFunctionPostingList>(), DataTypes{}, Array{}),
        std::make_unique<DataTypeCustomDesc>(std::make_unique<DataTypePostingList>(), std::make_shared<SerializationPostingList>())};
}

void registerDataTypePostingList(DataTypeFactory & factory)
{
    factory.registerDataTypeCustom("PostingList", create);
}

}
