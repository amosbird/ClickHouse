#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexArray.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Block.h>
#include <Core/SortDescription.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsLogical.h>
#include <Functions/IFunctionAdaptors.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/sortBlock.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Storages/KeyDescription.h>
#include <Storages/ProjectionsDescription.h>
#include <Storages/StorageInMemoryMetadata.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_QUERY;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

/// Generate _parent_part_offset column: for each expanded row, the row index of the parent row in the original block.
ColumnPtr generateParentOffsets(
    const ColumnArray::Offsets & array_offsets,
    size_t num_rows,
    size_t total_elements,
    UInt64 starting_offset,
    const IColumnPermutation * perm_ptr)
{
    auto uint64_type = std::make_shared<DataTypeUInt64>();
    auto col = uint64_type->createColumn();
    auto & data = assert_cast<ColumnUInt64 &>(*col).getData();
    data.resize_exact(total_elements);

    size_t pos = 0;
    for (size_t row = 0; row < num_rows; ++row)
    {
        UInt64 parent_offset;
        if (perm_ptr)
        {
            chassert(starting_offset == 0);
            parent_offset = (*perm_ptr)[row];
        }
        else
        {
            parent_offset = starting_offset + row;
        }

        size_t array_size = (row == 0) ? array_offsets[0] : (array_offsets[row] - array_offsets[row - 1]);
        for (size_t j = 0; j < array_size; ++j)
            data[pos++] = parent_offset;
    }
    chassert(pos == total_elements);
    return col;
}

/// Find all `arrayElement(column_name, const_key)` nodes in the DAG subtree rooted at `filter_node`.
/// Returns a list of (arrayElement_node, const_key_value) pairs.
struct ArrayElementMatch
{
    const ActionsDAG::Node * node;
    Field const_key;
};

void findArrayElementNodes(
    const ActionsDAG::Node * node,
    const String & column_name,
    std::vector<ArrayElementMatch> & matches,
    std::unordered_set<const ActionsDAG::Node *> & visited)
{
    if (!visited.insert(node).second)
        return;

    if (node->type == ActionsDAG::ActionType::FUNCTION
        && node->function_base
        && node->function_base->getName() == "arrayElement"
        && node->children.size() == 2)
    {
        const auto * map_arg = node->children[0];
        const auto * key_arg = node->children[1];

        if (map_arg->type == ActionsDAG::ActionType::INPUT
            && map_arg->result_name == column_name
            && key_arg->type == ActionsDAG::ActionType::COLUMN
            && key_arg->column)
        {
            matches.push_back({node, (*key_arg->column)[0]});
        }
    }

    for (const auto * child : node->children)
        findArrayElementNodes(child, column_name, matches, visited);
}

/// Find `has(column_name, const_value)`, `mapContains(column_name, const_key)`,
/// or `has(column_name_key, const_key)` nodes.
/// The last form is produced by `FunctionToSubcolumnsPass` which rewrites
/// `mapContains(map, key)` → `has(map_key, key)`.
struct HasMatch
{
    const ActionsDAG::Node * node;
    String function_name;
    Field const_value;
    /// The name that actually matched (could be column_name or column_name + "_key")
    String matched_column;
};

void findHasNodes(
    const ActionsDAG::Node * node,
    const String & column_name,
    std::vector<HasMatch> & matches,
    std::unordered_set<const ActionsDAG::Node *> & visited)
{
    if (!visited.insert(node).second)
        return;

    if (node->type == ActionsDAG::ActionType::FUNCTION
        && node->function_base
        && node->children.size() == 2)
    {
        const auto & func_name = node->function_base->getName();
        if (func_name == "has" || func_name == "mapContains" || func_name == "mapContainsKey")
        {
            const auto * col_arg = node->children[0];
            const auto * val_arg = node->children[1];

            if (col_arg->type == ActionsDAG::ActionType::INPUT
                && val_arg->type == ActionsDAG::ActionType::COLUMN
                && val_arg->column)
            {
                /// Normalize function name: mapContainsKey is an alias for mapContains
                auto normalized_name = (func_name == "mapContainsKey") ? String("mapContains") : func_name;

                /// Match `has(column_name, ...)` or `mapContains(column_name, ...)`
                /// or `mapContainsKey(column_name, ...)`
                if (col_arg->result_name == column_name)
                {
                    matches.push_back({node, normalized_name, (*val_arg->column)[0], column_name});
                }
                /// Match `has(column_name_key, ...)` — produced by FunctionToSubcolumnsPass
                /// from `mapContains(column_name, ...)`
                else if (col_arg->result_name == column_name + "_key")
                {
                    matches.push_back({node, "mapContains", (*val_arg->column)[0], col_arg->result_name});
                }
            }
        }
    }

    for (const auto * child : node->children)
        findHasNodes(child, column_name, matches, visited);
}

/// Evaluate the full filter predicate on an empty (default-valued) column.
/// If the predicate returns true for a default/empty Map or Array, we cannot use the projection
/// because `arrayElement` returns the default value for missing keys.
bool evaluateOnDefault(const ActionsDAG::Node * filter_node, const String & column_name, const DataTypePtr & column_type)
{
    auto subdag = ActionsDAG::cloneSubDAG({filter_node}, true);
    auto required_columns = subdag.getRequiredColumns();

    /// Build a block with the required column set to its default value
    Block block;
    for (const auto & col : required_columns)
    {
        if (col.name == column_name)
            block.insert({column_type->createColumnConstWithDefaultValue(1), column_type, col.name});
        else
            block.insert({col.type->createColumnConstWithDefaultValue(1), col.type, col.name});
    }

    ExpressionActions actions(std::move(subdag));
    actions.execute(block);

    const auto & output_name = filter_node->result_name;
    if (!block.has(output_name))
        return false;

    const auto & result_column = block.getByName(output_name).column;
    return result_column->getBool(0);
}

/// Add an `input_node = const_value` equality node to the DAG.
const ActionsDAG::Node * addEqualsCondition(
    ActionsDAG & dag,
    const ActionsDAG::Node * input_node,
    const String & input_name,
    const DataTypePtr & input_type,
    const Field & const_value)
{
    ColumnWithTypeAndName const_col;
    const_col.type = input_type;
    const_col.column = input_type->createColumnConst(1, const_value);
    const_col.name = input_type->getName() + "_" + const_value.dump();
    const auto * const_node = &dag.addColumn(std::move(const_col));

    auto equals_resolver = FunctionFactory::instance().get("equals", nullptr);
    return &dag.addFunction(equals_resolver, {input_node, const_node}, input_name + " = " + const_value.dump());
}

} // anonymous namespace

void ProjectionIndexArray::fillProjectionDescription(
    ProjectionDescription & result,
    const IAST * index_expr,
    const ColumnsDescription & columns,
    ContextPtr query_context) const
{
    auto column_name = index_expr->getColumnName();
    if (!columns.has(column_name))
        throw Exception(ErrorCodes::INCORRECT_QUERY, "Column '{}' not found in table columns", column_name);

    auto column_desc = columns.get(column_name);
    auto column_type = column_desc.type;

    auto uint64_type = std::make_shared<DataTypeUInt64>();

    /// Build the ORDER BY AST for the projection metadata.
    ASTPtr order_expression;

    if (const auto * map_type = typeid_cast<const DataTypeMap *>(column_type.get()))
    {
        /// Map(K, V) -> expanded columns: (col_key K, col_value V, _parent_part_offset UInt64)
        /// Sorted by (col_key, col_value)
        auto key_name = column_name + "_key";
        auto value_name = column_name + "_value";

        result.sample_block.insert({map_type->getKeyType()->createColumn(), map_type->getKeyType(), key_name});
        result.sample_block.insert({map_type->getValueType()->createColumn(), map_type->getValueType(), value_name});
        result.sample_block.insert({uint64_type->createColumn(), uint64_type, "_parent_part_offset"});

        result.with_parent_part_offset = true;
        result.required_columns = {column_name};
        result.key_size = 2;

        /// ORDER BY tuple(col_key, col_value)
        auto function_node = make_intrusive<ASTFunction>();
        function_node->name = "tuple";
        function_node->arguments = make_intrusive<ASTExpressionList>();
        function_node->arguments->children.push_back(make_intrusive<ASTIdentifier>(key_name));
        function_node->arguments->children.push_back(make_intrusive<ASTIdentifier>(value_name));
        function_node->children.push_back(function_node->arguments);
        order_expression = function_node;
    }
    else if (const auto * array_type = typeid_cast<const DataTypeArray *>(column_type.get()))
    {
        /// Array(T) -> expanded columns: (col_element T, _parent_part_offset UInt64)
        /// Sorted by col_element
        auto element_type = array_type->getNestedType();

        result.sample_block.insert({element_type->createColumn(), element_type, column_name});
        result.sample_block.insert({uint64_type->createColumn(), uint64_type, "_parent_part_offset"});

        result.with_parent_part_offset = true;
        result.required_columns = {column_name};
        result.key_size = 1;

        /// ORDER BY col_name
        order_expression = make_intrusive<ASTIdentifier>(column_name);
    }
    else
    {
        throw Exception(
            ErrorCodes::INCORRECT_QUERY,
            "Projection index type 'array' requires Array or Map column, got '{}'",
            column_type->getName());
    }

    /// Build StorageInMemoryMetadata for the projection.
    /// This is required by `MergeTreeData::checkProperties` which recursively validates
    /// projection metadata (sorting key, primary key, etc.).
    StorageInMemoryMetadata metadata;
    metadata.partition_key = KeyDescription::buildEmptyKey();

    auto projection_columns = ColumnsDescription(result.sample_block.getNamesAndTypesList());
    metadata.sorting_key = KeyDescription::getSortingKeyFromAST(order_expression, projection_columns, query_context, {});
    metadata.primary_key = KeyDescription::getKeyFromAST(order_expression, projection_columns, query_context);
    metadata.primary_key.definition_ast = nullptr;

    metadata.setColumns(std::move(projection_columns));
    result.metadata = std::make_shared<StorageInMemoryMetadata>(metadata);
}

Block ProjectionIndexArray::calculate(
    const ProjectionDescription & projection_desc,
    const Block & block,
    UInt64 starting_offset,
    ContextPtr /* context */,
    const IColumnPermutation * perm_ptr) const
{
    if (block.rows() == 0)
        return projection_desc.sample_block.cloneEmpty();

    chassert(projection_desc.required_columns.size() == 1);
    const auto & column_name = projection_desc.required_columns[0];
    const auto & source_column = block.getByName(column_name);

    auto uint64_type = std::make_shared<DataTypeUInt64>();
    Block result;

    if (const auto * map_type = typeid_cast<const DataTypeMap *>(source_column.type.get()))
    {
        /// Map internally is ColumnMap -> ColumnArray(ColumnTuple(keys, values))
        const auto & column_map = assert_cast<const ColumnMap &>(*source_column.column);
        const auto & nested_array = column_map.getNestedColumn();
        const auto & offsets = nested_array.getOffsets();
        const auto & nested_data = column_map.getNestedData();
        const auto & key_col = nested_data.getColumn(0);
        const auto & value_col = nested_data.getColumn(1);

        size_t total_elements = nested_array.getData().size();

        auto parent_offset_col = generateParentOffsets(offsets, block.rows(), total_elements, starting_offset, perm_ptr);

        auto key_name = column_name + "_key";
        auto value_name = column_name + "_value";

        result.insert({key_col.cloneResized(total_elements), map_type->getKeyType(), key_name});
        result.insert({value_col.cloneResized(total_elements), map_type->getValueType(), value_name});
        result.insert({std::move(parent_offset_col), uint64_type, "_parent_part_offset"});

        /// Sort by (key, value)
        SortDescription sort_desc;
        sort_desc.emplace_back(key_name, 1, 1);
        sort_desc.emplace_back(value_name, 1, 1);
        sortBlock(result, sort_desc);
    }
    else if (const auto * array_type = typeid_cast<const DataTypeArray *>(source_column.type.get()))
    {
        const auto & column_array = assert_cast<const ColumnArray &>(*source_column.column);
        const auto & offsets = column_array.getOffsets();
        const auto & data_col = column_array.getData();

        size_t total_elements = data_col.size();

        auto parent_offset_col = generateParentOffsets(offsets, block.rows(), total_elements, starting_offset, perm_ptr);

        auto element_type = array_type->getNestedType();
        result.insert({data_col.cloneResized(total_elements), element_type, column_name});
        result.insert({std::move(parent_offset_col), uint64_type, "_parent_part_offset"});

        /// Sort by element column
        SortDescription sort_desc;
        sort_desc.emplace_back(column_name, 1, 1);
        sortBlock(result, sort_desc);
    }
    else
    {
        throw Exception(
            ErrorCodes::ILLEGAL_COLUMN,
            "ProjectionIndexArray::calculate: expected Array or Map column, got '{}'",
            source_column.type->getName());
    }

    return result;
}

std::optional<ActionsDAG> ProjectionIndexArray::rewriteFilterDAG(
    const ActionsDAG & /*filter_dag*/,
    const ActionsDAG::Node * filter_node,
    const ProjectionDescription & projection) const
{
    chassert(projection.required_columns.size() == 1);
    const auto & column_name = projection.required_columns[0];

    /// Determine if this projection is for a Map or Array column by checking sample_block columns.
    bool is_map = projection.sample_block.has(column_name + "_key");

    if (is_map)
        return rewriteFilterDAGForMap(filter_node, projection, column_name);
    else
        return rewriteFilterDAGForArray(filter_node, projection, column_name);
}

std::optional<ActionsDAG> ProjectionIndexArray::rewriteFilterDAGForMap(
    const ActionsDAG::Node * filter_node,
    const ProjectionDescription & projection,
    const String & column_name) const
{
    /// For Map projections, we need to find `arrayElement(map_col, 'const_key')` patterns
    /// and replace them with references to the projection's `map_col_value` column,
    /// adding `AND map_col_key = 'const_key'`.

    std::vector<ArrayElementMatch> ae_matches;
    std::unordered_set<const ActionsDAG::Node *> visited;
    findArrayElementNodes(filter_node, column_name, ae_matches, visited);

    std::vector<HasMatch> has_matches;
    visited.clear();
    findHasNodes(filter_node, column_name, has_matches, visited);

    if (ae_matches.empty() && has_matches.empty())
        return std::nullopt;

    /// Collect all distinct keys referenced by arrayElement and has/mapContains matches.
    /// If there are multiple different keys (e.g. `labels['env'] = 'prod' AND labels['region'] = 'eu'`),
    /// we cannot rewrite: the projection expands one row per Map entry, so a single expanded row
    /// has only ONE key. Requiring `labels_key = 'env' AND labels_key = 'region'` simultaneously
    /// is impossible and would incorrectly filter out all data.
    std::optional<Field> single_key;
    bool multiple_keys = false;
    auto check_key = [&](const Field & key)
    {
        if (!single_key)
            single_key = key;
        else if (*single_key != key)
            multiple_keys = true;
    };
    for (const auto & match : ae_matches)
        check_key(match.const_key);
    for (const auto & match : has_matches)
        check_key(match.const_value);
    if (multiple_keys)
        return std::nullopt;

    /// Determine the original column type from the projection's sample_block
    auto key_type = projection.sample_block.getByName(column_name + "_key").type;
    auto value_type = projection.sample_block.getByName(column_name + "_value").type;

    /// Construct the original Map type for the default-value safety check
    auto map_type = std::make_shared<DataTypeMap>(key_type, value_type);

    /// Safety check: evaluate predicate on empty Map; if it returns true, skip projection
    if (evaluateOnDefault(filter_node, column_name, map_type))
        return std::nullopt;

    /// Clone the filter DAG subtree and perform replacements
    ActionsDAG::NodeMapping copy_map;
    auto result_dag = ActionsDAG::cloneSubDAG({filter_node}, copy_map, false);

    auto key_name = column_name + "_key";
    auto value_name = column_name + "_value";

    /// Add projection column inputs to the new DAG
    const ActionsDAG::Node * key_input = nullptr;
    const ActionsDAG::Node * value_input = nullptr;

    /// Collect key equality conditions to AND together with the filter
    std::vector<const ActionsDAG::Node *> key_conditions;

    /// Process arrayElement matches: replace `arrayElement(map, key)` -> `value_col`
    /// and add `key_col = key` condition
    for (const auto & match : ae_matches)
    {
        auto it = copy_map.find(match.node);
        if (it == copy_map.end())
            return std::nullopt;

        /// Lazily create input nodes
        if (!value_input)
            value_input = &result_dag.addInput(value_name, value_type);
        if (!key_input)
            key_input = &result_dag.addInput(key_name, key_type);

        /// Replace arrayElement node with value_input by replacing in parent's children
        /// We find the copied node in result_dag and rewire parents to point to value_input
        auto * mutable_node = const_cast<ActionsDAG::Node *>(it->second);
        /// Replace the node's contents to act as an alias to value_input
        mutable_node->type = ActionsDAG::ActionType::ALIAS;
        mutable_node->children = {value_input};
        mutable_node->function_base = nullptr;
        mutable_node->function = nullptr;
        /// Keep result_name and result_type — they should match (value_type == arrayElement return type)

        /// Create `key_col = const_key` condition
        key_conditions.push_back(addEqualsCondition(result_dag, key_input, key_name, key_type, match.const_key));
    }

    /// Process has/mapContains matches:
    /// `has(map, key)` -> `key_col = key` (has on Map is semantically mapContains)
    /// `mapContains(map, key)` -> `key_col = key`
    for (const auto & match : has_matches)
    {
        auto it = copy_map.find(match.node);
        if (it == copy_map.end())
            return std::nullopt;

        if (!key_input)
            key_input = &result_dag.addInput(key_name, key_type);

        if (match.function_name == "mapContains" || match.function_name == "has")
        {
            /// Replace `mapContains(map, key)` or `has(map, key)` with `key_col = key`
            const auto * key_eq_node = addEqualsCondition(result_dag, key_input, key_name, key_type, match.const_value);

            /// Replace the node with the equals node
            auto * mutable_node = const_cast<ActionsDAG::Node *>(it->second);
            mutable_node->type = ActionsDAG::ActionType::ALIAS;
            mutable_node->children = {key_eq_node};
            mutable_node->function_base = nullptr;
            mutable_node->function = nullptr;
        }
    }

    /// Now AND together the filter with key conditions
    if (!key_conditions.empty())
    {
        const auto * current_filter = copy_map[filter_node];

        /// Build the conjunction: current_filter AND key_cond_1 AND key_cond_2 ...
        std::vector<const ActionsDAG::Node *> and_args;
        and_args.push_back(current_filter);
        for (const auto * cond : key_conditions)
            and_args.push_back(cond);

        FunctionOverloadResolverPtr and_resolver = std::make_unique<FunctionToOverloadResolverAdaptor>(std::make_shared<FunctionAnd>());
        const auto * and_node = &result_dag.addFunction(and_resolver, std::move(and_args), {});
        result_dag.getOutputs() = {and_node};
    }
    else
    {
        result_dag.getOutputs() = {copy_map[filter_node]};
    }

    /// Remove unused inputs (e.g. stale `labels_key` of type `Array(String)` from the
    /// original DAG, now replaced by `labels_key` of type `String` from the projection).
    result_dag.removeUnusedActions();

    return result_dag;
}

std::optional<ActionsDAG> ProjectionIndexArray::rewriteFilterDAGForArray(
    const ActionsDAG::Node * filter_node,
    const ProjectionDescription & projection,
    const String & column_name) const
{
    /// For Array projections, we need to find `has(array_col, const_value)` patterns
    /// and replace them with `array_col_element = const_value`.

    std::vector<HasMatch> has_matches;
    std::unordered_set<const ActionsDAG::Node *> visited;
    findHasNodes(filter_node, column_name, has_matches, visited);

    if (has_matches.empty())
        return std::nullopt;

    /// If there are multiple different values (e.g. `has(tags, 'fast') AND has(tags, 'reliable')`),
    /// we cannot use the projection because each expanded row contains only one element,
    /// so the AND would produce zero matching rows — resulting in data loss.
    {
        std::optional<Field> single_value;
        bool multiple_values = false;
        for (const auto & match : has_matches)
        {
            if (!single_value)
                single_value = match.const_value;
            else if (*single_value != match.const_value)
                multiple_values = true;
        }
        if (multiple_values)
            return std::nullopt;
    }

    /// Safety check: evaluate predicate on empty Array; if it returns true, skip projection
    auto element_type = projection.sample_block.getByName(column_name).type;
    auto array_type = std::make_shared<DataTypeArray>(element_type);

    if (evaluateOnDefault(filter_node, column_name, array_type))
        return std::nullopt;

    /// Clone the filter DAG subtree and perform replacements
    ActionsDAG::NodeMapping copy_map;
    auto result_dag = ActionsDAG::cloneSubDAG({filter_node}, copy_map, false);

    const ActionsDAG::Node * element_input = nullptr;

    for (const auto & match : has_matches)
    {
        if (match.function_name != "has")
            continue; /// Only `has` makes sense for Array columns

        auto it = copy_map.find(match.node);
        if (it == copy_map.end())
            return std::nullopt;

        if (!element_input)
            element_input = &result_dag.addInput(column_name, element_type);

        /// Replace `has(array, value)` with `element_col = value`
        const auto * eq_node = addEqualsCondition(result_dag, element_input, column_name, element_type, match.const_value);

        /// Replace the has node with the equals node
        auto * mutable_node = const_cast<ActionsDAG::Node *>(it->second);
        mutable_node->type = ActionsDAG::ActionType::ALIAS;
        mutable_node->children = {eq_node};
        mutable_node->function_base = nullptr;
        mutable_node->function = nullptr;
    }

    result_dag.getOutputs() = {copy_map[filter_node]};
    result_dag.removeUnusedActions();
    return result_dag;
}

}
