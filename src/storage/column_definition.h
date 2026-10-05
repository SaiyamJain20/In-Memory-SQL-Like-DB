#pragma once

#include "types/logical_type.h"

#include <string>

namespace cdb {

struct ColumnDefinition {
    std::string name;
    LogicalType type;
};

} // namespace cdb
