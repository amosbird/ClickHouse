#pragma once

#include <DataTypes/Serializations/ISerialization.h>

namespace DB
{

constexpr auto PROJECTION_INDEX_LARGE_POSTING_SUFFIX = ".lpst";

class MergedPartOffsets;

struct ProjectionIndexDeserializationContext
{
    const MergedPartOffsets * merged_part_offsets = nullptr;
    UInt64 part_index = 0;
    UInt64 row_start = 0;
    UInt64 row_end = 0;
};

struct ProjectionIndexSerializationContext
{
    ISerialization::OutputStreamGetter large_posting_getter;
};

}
