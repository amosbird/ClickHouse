#include <Storages/MergeTree/ProjectionIndex/ProjectionIndexText.h>

#include <Core/Block.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTIndexDeclaration.h>
#include <Parsers/ASTProjectionDeclaration.h>
#include <Parsers/ASTProjectionSelectQuery.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/ProjectionsDescription.h>

namespace DB
{

MergeTreeIndexPtr textIndexCreator(const IndexDescription & index);

ProjectionIndexPtr ProjectionIndexText::create(const ASTProjectionDeclaration & proj)
{
    auto index_ast = std::make_shared<ASTIndexDeclaration>(proj.index->clone(), proj.type->clone(), "ProjectionIndexText");
    index_ast->granularity = ASTIndexDeclaration::DEFAULT_INDEX_GRANULARITY;
    return std::make_shared<ProjectionIndexText>(std::move(index_ast));
}

void ProjectionIndexText::fillProjectionDescription(
    ProjectionDescription & result, const IAST * /* index_expr */, const ColumnsDescription & columns, ContextPtr query_context) const
{
    result.index = std::make_shared<ProjectionIndexText>(std::static_pointer_cast<const MergeTreeIndexText>(
        textIndexCreator(IndexDescription::getIndexFromAST(index_ast, columns, /* is_implicitly_created */ true, query_context))));
}

Block ProjectionIndexText::calculate(
    const ProjectionDescription & projection_desc, const Block & block, ContextPtr context, const IColumnPermutation * perm_ptr) const
{
    return projection_desc.calculateByQuery(block, context, perm_ptr);
}

}
