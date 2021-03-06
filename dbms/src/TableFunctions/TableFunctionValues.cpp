#include <Common/typeid_cast.h>
#include <Common/Exception.h>

#include <Core/Block.h>
#include <Storages/StorageValues.h>
#include <DataTypes/DataTypeTuple.h>

#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>

#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionValues.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/parseColumnsListForTableFunction.h>

#include <Interpreters/convertFieldToType.h>
#include <Interpreters/evaluateConstantExpression.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

static void parseAndInsertValues(MutableColumns & res_columns, const ASTs & args, const Block & sample_block, const Context & context)
{
    if (res_columns.size() == 1) /// Parsing arguments as Fields
    {
        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto & [value_field, value_type_ptr] = evaluateConstantExpression(args[i], context);

            Field value = convertFieldToType(value_field, *sample_block.getByPosition(0).type, value_type_ptr.get());
            res_columns[0]->insert(value);
        }
    }
    else /// Parsing arguments as Tuples
    {
        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto & [value_field, value_type_ptr] = evaluateConstantExpression(args[i], context);
            const DataTypes & value_types_tuple = typeid_cast<const DataTypeTuple *>(value_type_ptr.get())->getElements();
            const TupleBackend & value_tuple = value_field.safeGet<Tuple>().toUnderType();

            if (value_tuple.size() != sample_block.columns())
                throw Exception("Values size should match with number of columns", ErrorCodes::LOGICAL_ERROR);

            for (size_t j = 0; j < value_tuple.size(); ++j)
            {
                Field value = convertFieldToType(value_tuple[j], *sample_block.getByPosition(j).type, value_types_tuple[j].get());
                res_columns[j]->insert(value);
            }
        }
    }
}

StoragePtr TableFunctionValues::executeImpl(const ASTPtr & ast_function, const Context & context, const std::string & table_name) const
{
    ASTs & args_func = ast_function->children;

    if (args_func.size() != 1)
        throw Exception("Table function '" + getName() + "' must have arguments.", ErrorCodes::LOGICAL_ERROR);

    ASTs & args = args_func.at(0)->children;

    if (args.size() < 2)
        throw Exception("Table function '" + getName() + "' requires 2 or more arguments: structure and values.",
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    /// Parsing first argument as table structure and creating a sample block
    std::string structure = args[0]->as<ASTLiteral &>().value.safeGet<String>();

    ColumnsDescription columns = parseColumnsListFromString(structure, context);

    Block sample_block;
    for (const auto & name_type : columns.getOrdinary())
        sample_block.insert({ name_type.type->createColumn(), name_type.type, name_type.name });

    MutableColumns res_columns = sample_block.cloneEmptyColumns();

    /// Parsing other arguments as values and inserting them into columns
    parseAndInsertValues(res_columns, args, sample_block, context);

    Block res_block = sample_block.cloneWithColumns(std::move(res_columns));

    auto res = StorageValues::create(getDatabaseName(), table_name, columns, res_block);
    res->startup();
    return res;
}

void registerTableFunctionValues(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionValues>(TableFunctionFactory::CaseInsensitive);
}

}
