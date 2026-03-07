#pragma once

#include <Storages/MergeTree/ProjectionIndex/IProjectionIndex.h>

namespace DB
{

/// Projection index that expands Array/Map columns via ARRAY JOIN,
/// creating an inverted index mapping expanded elements back to parent rows
/// via `_parent_part_offset`.
///
/// For `Array(T)`: produces `(col_element, _parent_part_offset)` sorted by `col_element`.
/// For `Map(K,V)`: produces `(col_key, col_value, _parent_part_offset)` sorted by `(col_key, col_value)`.
class ProjectionIndexArray : public IProjectionIndex
{
public:
    static constexpr auto name = "array";

    static ProjectionIndexPtr create(const ASTProjectionDeclaration & /* proj */)
    {
        return std::make_shared<ProjectionIndexArray>();
    }

    String getName() const override { return name; }

    void fillProjectionDescription(
        ProjectionDescription & result,
        const IAST * index_expr,
        const ColumnsDescription & columns,
        ContextPtr query_context) const override;

    Block calculate(
        const ProjectionDescription & projection_desc,
        const Block & block,
        UInt64 starting_offset,
        ContextPtr context,
        const IColumnPermutation * perm_ptr) const override;

    std::optional<ActionsDAG> rewriteFilterDAG(
        const ActionsDAG & filter_dag,
        const ActionsDAG::Node * filter_node,
        const ProjectionDescription & projection) const override;

private:
    std::optional<ActionsDAG> rewriteFilterDAGForMap(
        const ActionsDAG::Node * filter_node,
        const ProjectionDescription & projection,
        const String & column_name) const;

    std::optional<ActionsDAG> rewriteFilterDAGForArray(
        const ActionsDAG::Node * filter_node,
        const ProjectionDescription & projection,
        const String & column_name) const;
};

}
