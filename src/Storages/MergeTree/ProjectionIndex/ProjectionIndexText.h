#pragma once

#include <Core/Field.h>
#include <Storages/MergeTree/ProjectionIndex/IProjectionIndex.h>

namespace DB
{

class MergeTreeIndexText;

class ProjectionIndexText : public IProjectionIndex
{
public:
    static constexpr auto name = "text";

    static ProjectionIndexPtr create(const ASTProjectionDeclaration & proj);

    explicit ProjectionIndexText(ASTPtr index_ast_)
        : index_ast(std::move(index_ast_))
    {
    }

    explicit ProjectionIndexText(std::shared_ptr<const MergeTreeIndexText> text_index_)
        : text_index(std::move(text_index_))
    {
    }

    String getName() const override { return name; }

    void fillProjectionDescription(
        ProjectionDescription & result,
        const IAST * index_expr,
        const ColumnsDescription & columns,
        ContextPtr query_context) const override;

    Block
    calculate(const ProjectionDescription & projection_desc, const Block & block, ContextPtr context, const IColumnPermutation * perm_ptr)
        const override;

private:
    ASTPtr index_ast;
    std::shared_ptr<const MergeTreeIndexText> text_index;
};

}
