#pragma once

#include <base/types.h>

namespace DB
{

class MergedPartOffsets;

struct ProjectionIndexDeserializationContext
{
    const MergedPartOffsets * merged_part_offsets = nullptr;
    UInt64 part_index = 0;
    UInt64 row_start = 0;
    UInt64 row_end = 0;
};

}
