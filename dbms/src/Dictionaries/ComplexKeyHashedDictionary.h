#pragma once

#include <atomic>
#include <memory>
#include <variant>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnString.h>
#include <Common/Arena.h>
#include <Common/HashTable/HashMap.h>
#include <Core/Block.h>
#include <common/StringRef.h>
#include <ext/range.h>
#include "DictionaryStructure.h"
#include "IDictionary.h"
#include "IDictionarySource.h"


namespace DB
{
using BlockPtr = std::shared_ptr<Block>;

class ComplexKeyHashedDictionary final : public IDictionaryBase
{
public:
    ComplexKeyHashedDictionary(
        const std::string & name_,
        const DictionaryStructure & dict_struct_,
        DictionarySourcePtr source_ptr_,
        const DictionaryLifetime dict_lifetime_,
        bool require_nonempty_,
        BlockPtr saved_block_ = nullptr);

    std::string getKeyDescription() const { return key_description; }

    std::string getName() const override { return name; }

    std::string getTypeName() const override { return "ComplexKeyHashed"; }

    size_t getBytesAllocated() const override { return bytes_allocated; }

    size_t getQueryCount() const override { return query_count.load(std::memory_order_relaxed); }

    double getHitRate() const override { return 1.0; }

    size_t getElementCount() const override { return element_count; }

    double getLoadFactor() const override { return static_cast<double>(element_count) / bucket_count; }

    bool isCached() const override { return false; }

    std::shared_ptr<const IExternalLoadable> clone() const override
    {
        return std::make_shared<ComplexKeyHashedDictionary>(name, dict_struct, source_ptr->clone(), dict_lifetime, require_nonempty, saved_block);
    }

    const IDictionarySource * getSource() const override { return source_ptr.get(); }

    const DictionaryLifetime & getLifetime() const override { return dict_lifetime; }

    const DictionaryStructure & getStructure() const override { return dict_struct; }

    bool isInjective(const std::string & attribute_name) const override
    {
        return dict_struct.attributes[&getAttribute(attribute_name) - attributes.data()].injective;
    }

    template <typename T>
    using ResultArrayType = std::conditional_t<IsDecimalNumber<T>, DecimalPaddedPODArray<T>, PaddedPODArray<T>>;

#define DECLARE(TYPE) \
    void get##TYPE( \
        const std::string & attribute_name, const Columns & key_columns, const DataTypes & key_types, ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void getString(const std::string & attribute_name, const Columns & key_columns, const DataTypes & key_types, ColumnString * out) const;

#define DECLARE(TYPE) \
    void get##TYPE( \
        const std::string & attribute_name, \
        const Columns & key_columns, \
        const DataTypes & key_types, \
        const PaddedPODArray<TYPE> & def, \
        ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void getString(
        const std::string & attribute_name,
        const Columns & key_columns,
        const DataTypes & key_types,
        const ColumnString * const def,
        ColumnString * const out) const;

#define DECLARE(TYPE) \
    void get##TYPE( \
        const std::string & attribute_name, \
        const Columns & key_columns, \
        const DataTypes & key_types, \
        const TYPE def, \
        ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void getString(
        const std::string & attribute_name,
        const Columns & key_columns,
        const DataTypes & key_types,
        const String & def,
        ColumnString * const out) const;

    void has(const Columns & key_columns, const DataTypes & key_types, PaddedPODArray<UInt8> & out) const;

    BlockInputStreamPtr getBlockInputStream(const Names & column_names, size_t max_block_size) const override;

private:
    template <typename Value>
    using ContainerType = HashMapWithSavedHash<StringRef, Value, StringRefHash>;

    struct Attribute final
    {
        AttributeUnderlyingType type;
        std::variant<
            UInt8,
            UInt16,
            UInt32,
            UInt64,
            UInt128,
            Int8,
            Int16,
            Int32,
            Int64,
            Decimal32,
            Decimal64,
            Decimal128,
            Float32,
            Float64,
            String>
            null_values;
        std::variant<
            ContainerType<UInt8>,
            ContainerType<UInt16>,
            ContainerType<UInt32>,
            ContainerType<UInt64>,
            ContainerType<UInt128>,
            ContainerType<Int8>,
            ContainerType<Int16>,
            ContainerType<Int32>,
            ContainerType<Int64>,
            ContainerType<Decimal32>,
            ContainerType<Decimal64>,
            ContainerType<Decimal128>,
            ContainerType<Float32>,
            ContainerType<Float64>,
            ContainerType<StringRef>>
            maps;
        std::unique_ptr<Arena> string_arena;
    };

    void createAttributes();

    void blockToAttributes(const Block & block);

    void updateData();

    void loadData();

    template <typename T>
    void addAttributeSize(const Attribute & attribute);

    void calculateBytesAllocated();

    template <typename T>
    void createAttributeImpl(Attribute & attribute, const Field & null_value);

    Attribute createAttributeWithType(const AttributeUnderlyingType type, const Field & null_value);

    template <typename AttributeType, typename OutputType, typename ValueSetter, typename DefaultGetter>
    void
    getItemsImpl(const Attribute & attribute, const Columns & key_columns, ValueSetter && set_value, DefaultGetter && get_default) const;

    template <typename T>
    bool setAttributeValueImpl(Attribute & attribute, const StringRef key, const T value);

    bool setAttributeValue(Attribute & attribute, const StringRef key, const Field & value);

    const Attribute & getAttribute(const std::string & attribute_name) const;

    static StringRef placeKeysInPool(const size_t row, const Columns & key_columns, StringRefs & keys, Arena & pool);

    template <typename T>
    void has(const Attribute & attribute, const Columns & key_columns, PaddedPODArray<UInt8> & out) const;

    std::vector<StringRef> getKeys() const;

    template <typename T>
    std::vector<StringRef> getKeys(const Attribute & attribute) const;

    const std::string name;
    const DictionaryStructure dict_struct;
    const DictionarySourcePtr source_ptr;
    const DictionaryLifetime dict_lifetime;
    const bool require_nonempty;
    const std::string key_description{dict_struct.getKeyDescription()};

    std::map<std::string, size_t> attribute_index_by_name;
    std::vector<Attribute> attributes;
    Arena keys_pool;

    size_t bytes_allocated = 0;
    size_t element_count = 0;
    size_t bucket_count = 0;
    mutable std::atomic<size_t> query_count{0};

    BlockPtr saved_block;
};

}
