#include "types/logical_type.h"

#include "common/assert.h"
#include "types/string_t.h"

#include <algorithm>
#include <cctype>

namespace cdb {

size_t PhysicalTypeSize(PhysicalType type) noexcept {
    switch (type) {
    case PhysicalType::Bool:
        return sizeof(bool);
    case PhysicalType::Int32:
        return sizeof(int32_t);
    case PhysicalType::Int64:
        return sizeof(int64_t);
    case PhysicalType::Double:
        return sizeof(double);
    case PhysicalType::String:
        return sizeof(string_t);
    }
    CDB_UNREACHABLE("PhysicalTypeSize");
}

const char* PhysicalTypeName(PhysicalType type) noexcept {
    switch (type) {
    case PhysicalType::Bool:
        return "BOOL";
    case PhysicalType::Int32:
        return "INT32";
    case PhysicalType::Int64:
        return "INT64";
    case PhysicalType::Double:
        return "DOUBLE";
    case PhysicalType::String:
        return "STRING";
    }
    CDB_UNREACHABLE("PhysicalTypeName");
}

PhysicalType LogicalType::physical() const noexcept {
    switch (id_) {
    case TypeId::Boolean:
        return PhysicalType::Bool;
    case TypeId::Integer:
        return PhysicalType::Int32;
    case TypeId::BigInt:
        return PhysicalType::Int64;
    case TypeId::Double:
        return PhysicalType::Double;
    case TypeId::Date:
        return PhysicalType::Int32;
    case TypeId::Varchar:
        return PhysicalType::String;
    }
    CDB_UNREACHABLE("LogicalType::physical");
}

bool LogicalType::IsNumeric() const noexcept {
    return id_ == TypeId::Integer || id_ == TypeId::BigInt || id_ == TypeId::Double;
}

bool LogicalType::IsIntegral() const noexcept {
    return id_ == TypeId::Integer || id_ == TypeId::BigInt;
}

std::string LogicalType::ToString() const {
    switch (id_) {
    case TypeId::Boolean:
        return "BOOLEAN";
    case TypeId::Integer:
        return "INTEGER";
    case TypeId::BigInt:
        return "BIGINT";
    case TypeId::Double:
        return "DOUBLE";
    case TypeId::Date:
        return "DATE";
    case TypeId::Varchar:
        return "VARCHAR";
    }
    CDB_UNREACHABLE("LogicalType::ToString");
}

std::optional<LogicalType> LogicalType::FromName(std::string_view name) {
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper == "BOOLEAN" || upper == "BOOL")
        return Boolean();
    if (upper == "INTEGER" || upper == "INT" || upper == "INT4")
        return Integer();
    if (upper == "BIGINT" || upper == "INT8")
        return BigInt();
    if (upper == "DOUBLE" || upper == "FLOAT8")
        return Double();
    if (upper == "DATE")
        return Date();
    if (upper == "VARCHAR" || upper == "TEXT" || upper == "STRING")
        return Varchar();
    return std::nullopt;
}

} // namespace cdb
