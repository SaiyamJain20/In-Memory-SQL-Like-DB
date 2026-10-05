#pragma once

#include "types/logical_type.h"

#include <string>

namespace cdb {

struct ColumnDefinition {
    std::string name;
    LogicalType type;
    // NOT NULL is enforced by Table::Append (violations throw Error(Execution)).
    bool not_null = false;
};

} // namespace cdb
