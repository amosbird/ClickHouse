#pragma once

#include <AggregateFunctions/IAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeCustom.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListData.h>

namespace DB
{

/// On-disk posting list format version.
/// Bumped when the on-disk layout changes incompatibly.  Readers must reject unknown versions.
static constexpr size_t POSTING_LIST_FORMAT_VERSION_BASIC = 0;
static constexpr size_t POSTING_LIST_FORMAT_VERSION_CURRENT = POSTING_LIST_FORMAT_VERSION_BASIC;

/// Projection-index-specific parameters that extend upstream's MergeTreeIndexTextParams.
/// Inherits all text index params (dictionary_block_size, posting_list_block_size, etc.) and adds
/// projection-only fields without polluting upstream's struct.
struct PostingListParams : MergeTreeIndexTextParams
{
    /// On-disk format version number.
    size_t format_version = POSTING_LIST_FORMAT_VERSION_CURRENT;

    PostingListParams() = default;

    PostingListParams(const MergeTreeIndexTextParams & text_params, size_t format_version_)
        : MergeTreeIndexTextParams(text_params)
        , format_version(format_version_)
    {
    }

    /// Validates projection-specific fields only; base class fields are validated by textIndexValidator.
    void validate() const;
};

class AggregateFunctionPostingList final : public IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>
{
    using Data = PostingListData;

public:
    explicit AggregateFunctionPostingList(const PostingListParams & params_)
        : IAggregateFunctionDataHelper<PostingListData, AggregateFunctionPostingList>({}, {}, createResultType())
        , params(params_)
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
    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * /* arena */) const override;

    void serialize(ConstAggregateDataPtr, WriteBuffer &, std::optional<size_t>) const override;

    void deserialize(AggregateDataPtr, ReadBuffer &, std::optional<size_t>, Arena *) const override;

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena * /* arena */) const override;

    bool allocatesMemoryInArena() const override { return false; }

    /// Returns the data type without non-persistent parameters to resolve type mismatches caused by DataTypePostingList
    /// having runtime arguments not present in stored data.
    // DataTypePtr getNormalizedStateType() const override;

    PostingListParams params;
};

struct DataTypePostingList : public IDataTypeCustomName
{
    DataTypePostingList(const Array & parameters_, size_t format_version_)
        : parameters(parameters_)
        , format_version(format_version_)
    {
    }

    String getName() const override;

    const Array parameters;
    size_t format_version;
};

/// Construct PostingList DataType from index definition using the specified posting list params. This is used
/// in metadata construction paths (projection definition, merge, and output parts) to control the on-disk format.
/// The caller provides already-parsed PostingListParams to avoid redundant re-parsing of the AST arguments.
/// The AST arguments are only used to build the metadata Array for serialization (DataTypePostingList::getName()).
DataTypePtr createPostingListType(const ASTPtr & text_index_definition, const PostingListParams & posting_list_params);

/// Reconstruct PostingList DataType from on-disk part metadata. The format version is read
/// from the stored first field; old parts with version 0 are still supported.
DataTypePtr createPostingListTypeFromPartMetadata(const ASTPtr & parsed_fields);

}
