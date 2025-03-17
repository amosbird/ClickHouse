#include <Columns/ColumnArray.h>

#include <Formats/FormatSettings.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/Serializations/SerializationArray.h>

#include <Parsers/IAST.h>

#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>
#include <Parsers/ASTLiteral.h>

#include <Core/NamesAndTypes.h>
#include <Columns/ColumnConst.h>

#include <IO/WriteHelpers.h>
#include <IO/Operators.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int UNEXPECTED_AST_STRUCTURE;
}
using FieldType = Array;


DataTypeArray::DataTypeArray(const DataTypePtr & nested_, size_t n_)
    : nested{nested_}, n(n_)
{
}


MutableColumnPtr DataTypeArray::createColumn() const
{
    return ColumnArray::create(nested->createColumn(), ColumnArray::ColumnOffsets::create(), n);
}

Field DataTypeArray::getDefault() const
{
    return Array();
}

bool DataTypeArray::equals(const IDataType & rhs) const
{
    const auto & other = static_cast<const DataTypeArray &>(rhs);
    return typeid(rhs) == typeid(*this) && nested->equals(*other.nested) && n == other.n;
}

SerializationPtr DataTypeArray::doGetDefaultSerialization() const
{
    return std::make_shared<SerializationArray>(nested->getDefaultSerialization());
}

size_t DataTypeArray::getNumberOfDimensions() const
{
    const DataTypeArray * nested_array = typeid_cast<const DataTypeArray *>(nested.get());
    if (!nested_array)
        return 1;
    return 1 + nested_array->getNumberOfDimensions();   /// Every modern C++ compiler optimizes tail recursion.
}

String DataTypeArray::doGetPrettyName(size_t indent) const
{
    WriteBufferFromOwnString s;
    s << "Array(" << nested->getPrettyName(indent);
    if (n > 0)
        s << ", " << n;
    s << ')';
    return s.str();
}

void DataTypeArray::forEachChild(const ChildCallback & callback) const
{
    callback(*nested);
    nested->forEachChild(callback);
}

std::unique_ptr<ISerialization::SubstreamData> DataTypeArray::getDynamicSubcolumnData(std::string_view subcolumn_name, const DB::IDataType::SubstreamData & data, bool throw_if_null) const
{
    auto nested_type = assert_cast<const DataTypeArray &>(*data.type).nested;
    auto nested_data = std::make_unique<ISerialization::SubstreamData>(nested_type->getDefaultSerialization());
    nested_data->type = nested_type;
    nested_data->column = data.column ? assert_cast<const ColumnArray &>(*data.column).getDataPtr() : nullptr;

    auto nested_subcolumn_data = DB::IDataType::getSubcolumnData(subcolumn_name, *nested_data, throw_if_null);
    if (!nested_subcolumn_data)
        return nullptr;

    auto creator = SerializationArray::SubcolumnCreator(data.column ? assert_cast<const ColumnArray &>(*data.column).getOffsetsPtr() : nullptr);
    auto res = std::make_unique<ISerialization::SubstreamData>();
    res->serialization = creator.create(nested_subcolumn_data->serialization, nested_subcolumn_data->type);
    res->type = creator.create(nested_subcolumn_data->type);
    if (data.column)
        res->column = creator.create(nested_subcolumn_data->column);

    return res;
}

static DataTypePtr create(const ASTPtr & arguments)
{
    if (!arguments || arguments->children.size() > 2)
    {
        throw Exception(
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Array data type family requires 1-2 arguments: element type and optional array size");
    }

    size_t n = 0;

    if (arguments->children.size() == 2)
    {
        const auto * argument = arguments->children[1]->as<ASTLiteral>();
        if (!argument || argument->value.getType() != Field::Types::UInt64 || argument->value.safeGet<UInt64>() == 0)
            throw Exception(ErrorCodes::UNEXPECTED_AST_STRUCTURE,
                            "Array data type family must have a number (positive integer) as its second argument");

        n = argument->value.safeGet<UInt64>();
    }

    return std::make_shared<DataTypeArray>(DataTypeFactory::instance().get(arguments->children[0]), n);
}


void registerDataTypeArray(DataTypeFactory & factory)
{
    factory.registerDataType("Array", create);
}

}
