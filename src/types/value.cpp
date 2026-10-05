#include "types/value.h"

#include "common/assert.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace cdb {

Value Value::Null(LogicalType type) {
    Value v(type);
    v.is_null_ = true;
    return v;
}

Value Value::Boolean(bool x) {
    Value v(LogicalType::Boolean());
    v.u_.b = x;
    return v;
}

Value Value::Integer(int32_t x) {
    Value v(LogicalType::Integer());
    v.u_.i32 = x;
    return v;
}

Value Value::BigInt(int64_t x) {
    Value v(LogicalType::BigInt());
    v.u_.i64 = x;
    return v;
}

Value Value::Double(double x) {
    Value v(LogicalType::Double());
    v.u_.d = x;
    return v;
}

Value Value::Date(date_t x) {
    Value v(LogicalType::Date());
    v.u_.i32 = x.days;
    return v;
}

Value Value::Varchar(std::string x) {
    Value v(LogicalType::Varchar());
    v.str_ = std::move(x);
    return v;
}

bool Value::GetBoolean() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::Boolean);
    return u_.b;
}

int32_t Value::GetInteger() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::Integer);
    return u_.i32;
}

int64_t Value::GetBigInt() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::BigInt);
    return u_.i64;
}

double Value::GetDouble() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::Double);
    return u_.d;
}

date_t Value::GetDate() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::Date);
    return date_t{u_.i32};
}

const std::string& Value::GetVarchar() const {
    CDB_CHECK(!is_null_ && type_.id() == TypeId::Varchar);
    return str_;
}

namespace {

std::string DoubleToString(double d) {
    if (std::isnan(d)) {
        return "nan";
    }
    if (std::isinf(d)) {
        return d < 0 ? "-inf" : "inf";
    }
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), d);
    CDB_CHECK(res.ec == std::errc());
    std::string s(buf, res.ptr);
    if (s.find_first_of(".e") == std::string::npos) {
        s += ".0";
    }
    return s;
}

template <class T> int ThreeWay(T a, T b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

} // namespace

std::string Value::ToString() const {
    if (is_null_) {
        return "NULL";
    }
    switch (type_.id()) {
    case TypeId::Boolean:
        return u_.b ? "true" : "false";
    case TypeId::Integer:
        return std::to_string(u_.i32);
    case TypeId::BigInt:
        return std::to_string(u_.i64);
    case TypeId::Double:
        return DoubleToString(u_.d);
    case TypeId::Date:
        return Date::ToString(date_t{u_.i32});
    case TypeId::Varchar:
        return str_;
    }
    CDB_UNREACHABLE("Value::ToString");
}

int Value::Compare(const Value& a, const Value& b) {
    CDB_CHECK(!a.is_null_ && !b.is_null_ && a.type_ == b.type_);
    switch (a.type_.id()) {
    case TypeId::Boolean:
        return ThreeWay(a.u_.b, b.u_.b);
    case TypeId::Integer:
    case TypeId::Date:
        return ThreeWay(a.u_.i32, b.u_.i32);
    case TypeId::BigInt:
        return ThreeWay(a.u_.i64, b.u_.i64);
    case TypeId::Double: {
        const bool an = std::isnan(a.u_.d);
        const bool bn = std::isnan(b.u_.d);
        if (an || bn) {
            return an == bn ? 0 : (an ? 1 : -1);
        }
        return ThreeWay(a.u_.d, b.u_.d); // -0.0 == 0.0 under IEEE comparison
    }
    case TypeId::Varchar: {
        // std::string::compare uses char_traits<char>::compare, which compares as unsigned char.
        const int c = a.str_.compare(b.str_);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    }
    CDB_UNREACHABLE("Value::Compare");
}

bool operator==(const Value& a, const Value& b) {
    if (a.type_ != b.type_ || a.is_null_ != b.is_null_) {
        return false;
    }
    if (a.is_null_) {
        return true;
    }
    return Value::Compare(a, b) == 0;
}

} // namespace cdb
