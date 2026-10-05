#pragma once

#include "types/date.h"
#include "types/logical_type.h"

#include <cstdint>
#include <string>

namespace cdb {

// A single typed scalar (possibly NULL). Used for literals, zone-map bounds, slow-path vector
// access and as the reference model in tests. Hot loops never touch Value.
class Value {
  public:
    static Value Null(LogicalType type);
    static Value Boolean(bool v);
    static Value Integer(int32_t v);
    static Value BigInt(int64_t v);
    static Value Double(double v);
    static Value Date(date_t v);
    static Value Varchar(std::string v);

    LogicalType type() const noexcept { return type_; }
    bool IsNull() const noexcept { return is_null_; }

    // Getters require a non-NULL value of the matching type.
    bool GetBoolean() const;
    int32_t GetInteger() const;
    int64_t GetBigInt() const;
    double GetDouble() const;
    date_t GetDate() const;
    const std::string& GetVarchar() const;

    // SQL-ish text: NULL -> "NULL", booleans "true"/"false", doubles in shortest round-trip form
    // with a ".0" suffix when integral (3 -> "3.0"), dates as YYYY-MM-DD, strings unquoted.
    std::string ToString() const;

    // Total order over NON-NULL values of the same type: <0, 0, >0.
    //   * numbers/dates/strings compare naturally (strings bytewise, unsigned)
    //   * DOUBLE: -0.0 == +0.0, NaN == NaN and NaN sorts after every other value (incl. +inf)
    //   * BOOLEAN: false < true
    static int Compare(const Value& a, const Value& b);

    // Structural equality: same type, same nullness, equal payload (via Compare). NULL == NULL
    // here, unlike SQL '='; SQL three-valued logic is implemented in the expression kernels.
    friend bool operator==(const Value& a, const Value& b);
    friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

  private:
    explicit Value(LogicalType type) noexcept : type_(type) {}

    LogicalType type_;
    bool is_null_ = false;
    union {
        bool b;
        int32_t i32; // INTEGER and DATE
        int64_t i64;
        double d;
    } u_{};
    std::string str_;
};

} // namespace cdb
