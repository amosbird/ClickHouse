#pragma once

#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context_fwd.h>
#include <Parsers/IAST_fwd.h>
#include <base/types.h>
#include <boost/core/noncopyable.hpp>
#include <Common/PODArray_fwd.h>

namespace DB
{

class ColumnsDescription;

struct ProjectionDescription;

using IColumnPermutation = PaddedPODArray<size_t>;

class ASTProjectionDeclaration;

/// Base interface for projection index implementations.
class IProjectionIndex
{
public:
    virtual ~IProjectionIndex();

    virtual String getName() const = 0;

    virtual void fillProjectionDescription(
        ProjectionDescription & result, const IAST * index_expr, const ColumnsDescription & columns, ContextPtr query_context) const
        = 0;

    virtual Block calculate(
        const ProjectionDescription & projection_desc,
        const Block & block,
        UInt64 starting_offset,
        ContextPtr context,
        const IColumnPermutation * perm_ptr) const
        = 0;

    /// Rewrite a filter DAG to use projection columns instead of original table columns.
    /// For array projection indices, this transforms expressions like `arrayElement(map_col, 'key')`
    /// into references to the expanded projection columns (e.g. `map_col_key`, `map_col_value`).
    /// Returns std::nullopt if the filter cannot be rewritten for this projection.
    virtual std::optional<ActionsDAG> rewriteFilterDAG(
        const ActionsDAG & filter_dag,
        const ActionsDAG::Node * filter_node,
        const ProjectionDescription & projection) const
    {
        UNUSED(filter_dag, filter_node, projection);
        return std::nullopt;
    }
};

using ProjectionIndexPtr = std::shared_ptr<IProjectionIndex>;

class ProjectionIndexFactory : private boost::noncopyable
{
public:
    static ProjectionIndexFactory & instance();

    using Creator = std::function<ProjectionIndexPtr(const ASTProjectionDeclaration & proj)>;

    ProjectionIndexPtr get(const ASTProjectionDeclaration & proj) const;

    template <typename ProjectionIndex>
    void registerProjectionIndex()
    {
        creators.emplace(ProjectionIndex::name, ProjectionIndex::create);
    }

protected:
    ProjectionIndexFactory();

private:
    using Creators = std::unordered_map<std::string, Creator>;
    Creators creators;
};

}
