#pragma once

#include <Core/Field.h>
#include <Core/SortDescription.h>
#include <Storages/IndicesDescription.h>
#include <Storages/MergeTree/ProjectionIndex/IProjectionIndex.h>

namespace DB
{

class MergeTreeIndexText;

class ProjectionIndexText : public IProjectionIndex
{
public:
    static constexpr auto name = "text";

    static ProjectionIndexPtr create(const ASTProjectionDeclaration & proj);

    explicit ProjectionIndexText(ASTPtr index_ast_, String col_name_)
        : index_ast(std::move(index_ast_))
        , col_name(std::move(col_name_))
    {
        sort_description.push_back(SortColumnDescription("term"));
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

    const IndexDescription & getIndexDescription() const override;

    std::shared_ptr<const MergeTreeIndexText> getTextIndex() const { return text_index; }

private:
    ASTPtr index_ast;
    String col_name;
    SortDescription sort_description;
    IndexDescription index_description;
    std::shared_ptr<const MergeTreeIndexText> text_index;
};

}
