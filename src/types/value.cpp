#include "types/value.h"

#include "common/assert.h"

#include <algorithm>
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
    if (d == 0.0) {
        return std::signbit(d) ? "-0.0" : "0.0";
    }
    // Shortest round-trip digits in scientific form: [-]d[.ddd]e[+-]XX
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::scientific);
    CDB_CHECK(res.ec == std::errc());
    std::string sci(buf, res.ptr);
    std::string sign;
    if (sci[0] == '-') {
        sign = "-";
        sci.erase(0, 1);
    }
    const size_t e_pos = sci.find('e');
    std::string digits = sci.substr(0, e_pos);
    const int exponent = std::stoi(sci.substr(e_pos + 1));
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end()); // "d.ddd" -> "dddd"

    // Like Python's repr (and DuckDB): positional notation for 1e-4 <= |d| < 1e16, otherwise
    // scientific with at least two exponent digits.
    if (exponent >= -4 && exponent < 16) {
        std::string out;
        if (exponent >= 0) {
            const size_t int_len = static_cast<size_t>(exponent) + 1;
            out = digits.size() <= int_len
                      ? digits + std::string(int_len - digits.size(), '0') + ".0"
                      : digits.substr(0, int_len) + "." + digits.substr(int_len);
        } else {
            out = "0." + std::string(static_cast<size_t>(-exponent - 1), '0') + digits;
        }
        return sign + out;
    }
    std::string mantissa = digits.substr(0, 1);
    if (digits.size() > 1)
        mantissa += "." + digits.substr(1);
    const int abs_exp = exponent < 0 ? -exponent : exponent;
    return sign + mantissa + "e" + (exponent < 0 ? "-" : "+") + (abs_exp < 10 ? "0" : "") +
           std::to_string(abs_exp);
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
