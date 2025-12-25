#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexText.h>

#include <Columns/ColumnAggregateFunction.h>
#include <Core/Block.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <Interpreters/ITokenExtractor.h>
#include <Interpreters/sortBlock.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTIndexDeclaration.h>
#include <Parsers/ASTProjectionDeclaration.h>
#include <Parsers/ASTProjectionSelectQuery.h>
#include <Storages/KeyDescription.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeIndexTextPreprocessor.h>
#include <Storages/MergeTree/ProjectionIndex/PostingListState.h>
#include <Storages/ProjectionsDescription.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/quoteString.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NO_SUCH_COLUMN_IN_TABLE;
    extern const int NOT_IMPLEMENTED;
}

MergeTreeIndexPtr textIndexCreator(const IndexDescription & index);

ProjectionIndexPtr ProjectionIndexText::create(const ASTProjectionDeclaration & proj)
{
    auto index_ast = std::make_shared<ASTIndexDeclaration>(proj.index->clone(), proj.type->clone(), "ProjectionIndexText");
    index_ast->granularity = ASTIndexDeclaration::DEFAULT_INDEX_GRANULARITY;

    const ASTIdentifier * col_name_ast = proj.index->as<const ASTIdentifier>();
    if (col_name_ast == nullptr)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Projection index expr must be identifier");

    return std::make_shared<ProjectionIndexText>(std::move(index_ast), col_name_ast->name());
}

void ProjectionIndexText::fillProjectionDescription(
    ProjectionDescription & result, const IAST * /* index_expr */, const ColumnsDescription & columns, ContextPtr query_context) const
{
    chassert(result.index.get() == this);
    chassert(!text_index);
    if (!columns.has(col_name))
        throw Exception(ErrorCodes::NO_SUCH_COLUMN_IN_TABLE, "There is no column {} to build index", backQuoteIfNeed(col_name));

    const auto & index_col = columns.get(col_name);
    DataTypePtr type = index_col.type;
    if (const DataTypeArray * type_array = typeid_cast<const DataTypeArray *>(type.get()))
        type = type_array->getNestedType();

    if (const DataTypeLowCardinality * type_low_cardinality = typeid_cast<const DataTypeLowCardinality *>(type.get()))
        type = type_low_cardinality->getDictionaryType();

    if (const DataTypeNullable * type_nullable = typeid_cast<const DataTypeNullable *>(type.get()))
        type = type_nullable->getNestedType();

    ColumnsDescription index_columns{{index_col.name, type}};
    static_cast<ProjectionIndexText &>(*result.index).index_description
        = IndexDescription::getIndexFromAST(index_ast, index_columns, /* is_implicitly_created */ true, query_context);
    static_cast<ProjectionIndexText &>(*result.index).text_index
        = std::static_pointer_cast<const MergeTreeIndexText>(textIndexCreator(index_description));

    result.required_columns = {col_name, "_part_offset"};
    result.with_parent_part_offset = true;
    StorageInMemoryMetadata metadata;
    metadata.partition_key = KeyDescription::buildEmptyKey();

    result.type = ProjectionDescription::Type::Aggregate;
    result.sample_block_for_keys.insert({ColumnString::create(), std::make_shared<DataTypeString>(), "term"});
    auto posting_list_type = getPostingListType(index_ast->as<ASTIndexDeclaration>()->getType()->arguments);
    result.sample_block
        = {result.sample_block_for_keys.getByPosition(0), {posting_list_type->createColumn(), posting_list_type, "posting_list"}};

    ColumnsDescription projection_columns(result.sample_block.getNamesAndTypesList());
    projection_columns.modify(
        "term", [&](ColumnDescription & column) { column.codec = makeASTFunction("CODEC", std::make_shared<ASTIdentifier>("ZSTD")); });

    projection_columns.modify(
        "posting_list",
        [&](ColumnDescription & column) { column.codec = makeASTFunction("CODEC", std::make_shared<ASTIdentifier>("ZSTD")); });

    auto term_ident = std::make_shared<ASTIdentifier>("term");
    metadata.sorting_key = KeyDescription::getSortingKeyFromAST(term_ident, projection_columns, query_context, {});
    metadata.primary_key = KeyDescription::getKeyFromAST(term_ident, projection_columns, query_context);
    metadata.primary_key.definition_ast = nullptr;
    metadata.setColumns(std::move(projection_columns));

    result.metadata = std::make_shared<StorageInMemoryMetadata>(metadata);
}

const IndexDescription & ProjectionIndexText::getIndexDescription() const
{
    return index_description;
}

namespace
{

template <typename Input>
Block tokenize(
    const ITokenExtractor & extractor,
    const Input & input_data_column,
    const ColumnUInt64::Container & offsets,
    Block sample,
    const IColumn::Offsets * array_offsets,
    const ColumnUInt8::Container * null_map,
    const IColumn * index_column,
    const IColumn::Permutation * perm_ptr)
{
    alignas(16) uint8_t packed_buffer[128 * 4];
    size_t rows = array_offsets ? array_offsets->size() : (index_column ? index_column->size() : input_data_column.size());
    auto terms = ColumnString::create();
    UInt32 cur_terms_id = 0;
    StringHashMap<UInt32> terms_id;
    StringHashMap<UInt32>::LookupResult it;

    const auto & posting_list_type = sample.getByPosition(1).type;
    auto posting_list_col = posting_list_type->createColumn();
    ColumnAggregateFunction & posting_list = assert_cast<ColumnAggregateFunction &>(*posting_list_col);
    Arena & arena = posting_list.createOrGetArena();

    auto work = [&]<bool has_null, typename Index>(const Index * index)
    {
        PostingListData * current_posting_list = nullptr;
        std::string_view data;
        for (size_t r = 0; r < rows; ++r)
        {
            size_t pr = r;
            if (perm_ptr)
                pr = (*perm_ptr)[r];

            size_t left = pr;
            size_t right = pr + 1;

            if (array_offsets)
            {
                left = (*array_offsets)[pr - 1];
                right = (*array_offsets)[pr];
            }

            for (size_t i = left; i < right; ++i)
            {
                if constexpr (has_null)
                {
                    if ((*null_map)[i])
                        continue;
                }

                if constexpr (std::is_same_v<Index, void>)
                    data = input_data_column.getDataAt(i);
                else
                    data = input_data_column.getDataAt(index->getUInt(i));

                if (data.empty())
                    continue;

                forEachTokenPadded(
                    extractor,
                    data.data(),
                    data.size(),
                    [&](const char * token_start, size_t token_length)
                    {
                        std::string_view term(token_start, token_length);
                        bool inserted;
                        terms_id.emplace(term, it, inserted);
                        if (inserted)
                        {
                            it->getMapped() = cur_terms_id++;
                            terms->insertData(term.data(), term.size());
                            posting_list.insertDefault();
                            current_posting_list = reinterpret_cast<PostingListData *>(posting_list.getData().back());
                            current_posting_list->toWriterUnsafe();
                        }
                        else
                        {
                            current_posting_list = reinterpret_cast<PostingListData *>(posting_list.getData()[it->getMapped()]);
                        }

                        current_posting_list->writer().add(static_cast<UInt32>(offsets[pr]), &arena, packed_buffer);
                        return false;
                    });
            }
        }
    };

    if (null_map)
        work.template operator()<true, void>(nullptr);
    else if (!index_column)
        work.template operator()<false, void>(nullptr);
    else if (const auto * uint8 = checkAndGetColumn<ColumnUInt8>(index_column))
        work.template operator()<false>(uint8);
    else if (const auto * uint16 = checkAndGetColumn<ColumnUInt16>(index_column))
        work.template operator()<false>(uint16);
    else if (const auto * uint32 = checkAndGetColumn<ColumnUInt32>(index_column))
        work.template operator()<false>(uint32);
    else if (const auto * uint64 = checkAndGetColumn<ColumnUInt64>(index_column))
        work.template operator()<false>(uint64);
    else
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected size of index type for low cardinality column");

    terms_id.clearAndShrink();

    /// Construct empty projection part to filter empty column.
    /// TODO(amos): For non-string columns, we need non-default placeholder.
    if (terms->empty())
    {
        terms->insertDefault();
        posting_list_col->insertDefault();
    }

    sample.getByPosition(0).column = std::move(terms);
    sample.getByPosition(1).column = std::move(posting_list_col);

    return sample;
}

}

Block ProjectionIndexText::calculate(
    const ProjectionDescription & projection_desc, const Block & block, ContextPtr /* context */, const IColumnPermutation * perm_ptr) const
{
    OpenTelemetry::SpanHolder span("ProjectionIndexText::calculate");

    // MergeTreeIndexTextParams params;
    // std::unique_ptr<ITokenExtractor> token_extractor;
    // MergeTreeIndexTextPreprocessorPtr preprocessor;

    /// TODO: handle RowExistsColumn
    /// Respect the _row_exists column.
    // if (block.has(RowExistsColumn::name))
    // {
    //     query_ast_copy = query_ast->clone();
    //     auto * select_row_exists = query_ast_copy->as<ASTSelectQuery>();
    //     if (!select_row_exists)
    //         throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot get ASTSelectQuery when adding _row_exists = 1. It's a bug");

    //     select_row_exists->setExpression(
    //         ASTSelectQuery::Expression::WHERE,
    //         makeASTOperator("equals", std::make_shared<ASTIdentifier>(RowExistsColumn::name), std::make_shared<ASTLiteral>(1)));
    // }

    /// Create "_part_offset" column when needed for projection with parent part offsets
    Block source_block = block;

    /// Add "_part_offset" column if not present (needed for insertions but not mutations - materialize projections)
    if (!source_block.has("_part_offset"))
    {
        auto uint64 = std::make_shared<DataTypeUInt64>();
        auto column = uint64->createColumn();
        auto & offset = assert_cast<ColumnUInt64 &>(*column).getData();
        offset.resize_exact(block.rows());
        if (perm_ptr)
        {
            for (size_t i = 0; i < block.rows(); ++i)
                offset[(*perm_ptr)[i]] = i;
        }
        else
        {
            iota(offset.data(), offset.size(), UInt64(0));
        }

        source_block.insert({std::move(column), std::move(uint64), "_part_offset"});
    }

    auto doc_column = source_block.getByName(col_name).column;
    const auto & offsets = assert_cast<const ColumnUInt64 &>(*source_block.getByName("_part_offset").column).getData();
    const IColumn::Offsets * array_offsets = nullptr;
    const ColumnUInt8::Container * null_map = nullptr;
    const IColumn * index_column = nullptr;

    if (const auto * array = checkAndGetColumn<ColumnArray>(doc_column.get()))
    {
        array_offsets = &array->getOffsets();
        doc_column = array->getDataPtr();
    }

    if (const auto * nullable = checkAndGetColumn<ColumnNullable>(doc_column.get()))
    {
        null_map = &nullable->getNullMapData();
        doc_column = nullable->getNestedColumnPtr();
    }
    else if (const auto * low_card = checkAndGetColumn<ColumnLowCardinality>(doc_column.get()))
    {
        doc_column = low_card->getDictionary().getNestedNotNullableColumn();
        index_column = &low_card->getIndexes();
    }

    auto agg = text_index->preprocessor;
    ColumnWithTypeAndName doc_column_with_type_and_name(doc_column, std::make_shared<DataTypeString>(), col_name);
    auto [processed_column, offset] = text_index->preprocessor->processColumn(doc_column_with_type_and_name, 0, doc_column->size());

    const auto * column_string = checkAndGetColumn<ColumnString>(processed_column.get());
    const auto * column_fixed_string = checkAndGetColumn<ColumnFixedString>(processed_column.get());
    if (!column_string && !column_fixed_string)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Inverted index projection only accepts string columns for now");

    Block tokenized_block;
    if (column_string)
    {
        tokenized_block = tokenize(
            *text_index->token_extractor,
            *column_string,
            offsets,
            projection_desc.sample_block,
            array_offsets,
            null_map,
            index_column,
            perm_ptr);
    }
    else
    {
        tokenized_block = tokenize(
            *text_index->token_extractor,
            *column_fixed_string,
            offsets,
            projection_desc.sample_block,
            array_offsets,
            null_map,
            index_column,
            perm_ptr);
    }

    sortBlock(tokenized_block, sort_description);
    return tokenized_block;
}

}
