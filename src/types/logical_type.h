#pragma once

#include "types/string_t.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cdb {

// SQL-visible types.
enum class TypeId : uint8_t {
    Boolean,
    Integer, // 32-bit signed
    BigInt,  // 64-bit signed
    Double,  // IEEE-754 binary64
    Date,    // days since 1970-01-01, 32-bit signed
    Varchar,
};

// In-memory representation. Kernels dispatch on this, not on TypeId, so e.g. INTEGER and DATE
// share one instantiation.
enum class PhysicalType : uint8_t { Bool, Int32, Int64, Double, String };

size_t PhysicalTypeSize(PhysicalType type) noexcept;
const char* PhysicalTypeName(PhysicalType type) noexcept;

class LogicalType {
  public:
    constexpr LogicalType(TypeId id) noexcept : id_(id) {} // NOLINT: implicit by design

    static constexpr LogicalType Boolean() noexcept { return LogicalType(TypeId::Boolean); }
    static constexpr LogicalType Integer() noexcept { return LogicalType(TypeId::Integer); }
    static constexpr LogicalType BigInt() noexcept { return LogicalType(TypeId::BigInt); }
    static constexpr LogicalType Double() noexcept { return LogicalType(TypeId::Double); }
    static constexpr LogicalType Date() noexcept { return LogicalType(TypeId::Date); }
    static constexpr LogicalType Varchar() noexcept { return LogicalType(TypeId::Varchar); }

    constexpr TypeId id() const noexcept { return id_; }
    PhysicalType physical() const noexcept;
    size_t width() const noexcept { return PhysicalTypeSize(physical()); }

    bool IsNumeric() const noexcept;  // INTEGER, BIGINT, DOUBLE
    bool IsIntegral() const noexcept; // INTEGER, BIGINT
    bool IsFixedWidth() const noexcept { return id_ != TypeId::Varchar; }

    std::string ToString() const;

    // Case-insensitive SQL type name -> type ("INT", "INTEGER", "BIGINT", "DOUBLE", "DATE", ...).
    static std::optional<LogicalType> FromName(std::string_view name);

    friend constexpr bool operator==(LogicalType a, LogicalType b) noexcept {
        return a.id_ == b.id_;
    }
    friend constexpr bool operator!=(LogicalType a, LogicalType b) noexcept {
        return a.id_ != b.id_;
    }

  private:
    TypeId id_;
};

// Maps a C++ element type to its PhysicalType (compile-time).
template <class T> struct PhysicalTypeOf;
template <> struct PhysicalTypeOf<bool> {
    static constexpr PhysicalType value = PhysicalType::Bool;
};
template <> struct PhysicalTypeOf<int32_t> {
    static constexpr PhysicalType value = PhysicalType::Int32;
};
template <> struct PhysicalTypeOf<int64_t> {
    static constexpr PhysicalType value = PhysicalType::Int64;
};
template <> struct PhysicalTypeOf<double> {
    static constexpr PhysicalType value = PhysicalType::Double;
};

template <> struct PhysicalTypeOf<string_t> {
    static constexpr PhysicalType value = PhysicalType::String;
};

} // namespace cdb
