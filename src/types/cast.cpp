#include "types/cast.h"

#include "common/error.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace cdb {

namespace {

int NumericRank(LogicalType t) {
    switch (t.id()) {
    case TypeId::Integer:
        return 1;
    case TypeId::BigInt:
        return 2;
    case TypeId::Double:
        return 3;
    default:
        return 0;
    }
}

[[noreturn]] void Fail(const std::string& message) {
    throw Error(ErrorCode::Type, message);
}

std::string_view Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        e--;
    return s.substr(b, e - b);
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Removes digit-group underscores ('1_000'): each '_' must sit between two digits. Returns false
// for a misplaced underscore.
bool StripUnderscores(std::string_view s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '_') {
            const bool ok = i > 0 && i + 1 < s.size() &&
                            std::isxdigit(static_cast<unsigned char>(s[i - 1])) &&
                            std::isxdigit(static_cast<unsigned char>(s[i + 1]));
            if (!ok)
                return false;
            continue;
        }
        out += s[i];
    }
    return true;
}

// [+-]digits or [+-]0xHEX (underscore separators allowed) -> value, overflow-checked
std::optional<int64_t> ParseInteger(std::string_view raw) {
    std::string cleaned;
    if (!StripUnderscores(raw, cleaned))
        return std::nullopt;
    std::string_view s = cleaned;
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }
    unsigned base = 10;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i == s.size())
        return std::nullopt;
    uint64_t acc = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63) : (uint64_t{1} << 63) - 1;
    for (; i < s.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = 10 + c - 'a';
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = 10 + c - 'A';
        else
            return std::nullopt;
        if (digit >= base)
            return std::nullopt;
        if (acc > (limit - digit) / base)
            return std::nullopt; // would exceed the limit
        acc = acc * base + digit;
    }
    return negative ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
}

// Strict decimal / scientific syntax or inf/infinity/nan words (case-insensitive).
std::optional<double> ParseDouble(std::string_view raw) {
    std::string cleaned;
    if (!StripUnderscores(raw, cleaned))
        return std::nullopt;
    const std::string_view s = cleaned;
    if (s.empty())
        return std::nullopt;
    const std::string lower = Lower(s);
    std::string_view body = lower;
    size_t sign_len = 0;
    if (body[0] == '+' || body[0] == '-')
        sign_len = 1;
    const std::string_view word = body.substr(sign_len);
    if (word == "inf" || word == "infinity") {
        return body[0] == '-' ? -std::numeric_limits<double>::infinity()
                              : std::numeric_limits<double>::infinity();
    }
    if (word == "nan")
        return std::numeric_limits<double>::quiet_NaN();

    size_t i = sign_len;
    size_t digits = 0;
    while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i])))
        i++, digits++;
    if (i < body.size() && body[i] == '.') {
        i++;
        while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i])))
            i++, digits++;
    }
    if (digits == 0)
        return std::nullopt;
    if (i < body.size() && body[i] == 'e') {
        i++;
        if (i < body.size() && (body[i] == '+' || body[i] == '-'))
            i++;
        size_t exp_digits = 0;
        while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i])))
            i++, exp_digits++;
        if (exp_digits == 0)
            return std::nullopt;
    }
    if (i != body.size())
        return std::nullopt;
    const std::string text(body);
    return std::strtod(text.c_str(), nullptr); // out-of-range literals ('1e999') become infinity
}

int64_t RoundToInt64(double d, LogicalType target, const std::string& shown) {
    if (std::isnan(d) || std::isinf(d)) {
        Fail("Cannot cast " + shown + " to " + target.ToString());
    }
    const double r = std::nearbyint(d); // half to even, like DuckDB's DOUBLE -> integer cast
    // 2^63 is exactly representable; anything >= it (or < -2^63) is out of range
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        Fail("Value " + shown + " is out of range for " + target.ToString());
    }
    return static_cast<int64_t>(r);
}

Value MakeInteger(int64_t v, const std::string& shown) {
    if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
        Fail("Value " + shown + " is out of range for INTEGER");
    }
    return Value::Integer(static_cast<int32_t>(v));
}

Value FromVarchar(const std::string& raw, LogicalType target) {
    const std::string_view s = Trim(raw);
    auto bad = [&]() -> Value {
        Fail("Could not convert string '" + raw + "' to " + target.ToString());
    };
    switch (target.id()) {
    case TypeId::Varchar:
        return Value::Varchar(raw);
    case TypeId::Integer:
    case TypeId::BigInt: {
        std::optional<int64_t> v = ParseInteger(s);
        if (!v) {
            std::optional<double> d = ParseDouble(s); // '1.5' -> 2
            if (!d || std::isnan(*d) || std::isinf(*d))
                return bad();
            const double r = std::round(*d);
            if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
                return bad();
            v = static_cast<int64_t>(r);
        }
        return target.id() == TypeId::BigInt ? Value::BigInt(*v) : MakeInteger(*v, raw);
    }
    case TypeId::Double: {
        auto d = ParseDouble(s);
        return d ? Value::Double(*d) : bad();
    }
    case TypeId::Date: {
        // Like DuckDB, the special words clamp to the ends of the supported range.
        const std::string word = Lower(s);
        if (word == "inf" || word == "+inf" || word == "infinity" || word == "+infinity")
            return Value::Date(Date::FromYMD(Date::kMaxYear, 12, 31));
        if (word == "-inf" || word == "-infinity")
            return Value::Date(Date::FromYMD(Date::kMinYear, 1, 1));
        auto d = Date::FromString(s);
        return d ? Value::Date(*d) : bad();
    }
    case TypeId::Boolean: {
        const std::string l = Lower(s);
        if (l == "true" || l == "t" || l == "yes" || l == "y" || l == "1")
            return Value::Boolean(true);
        if (l == "false" || l == "f" || l == "no" || l == "n" || l == "0")
            return Value::Boolean(false);
        return bad();
    }
    }
    return bad();
}

} // namespace

bool CanCastImplicitly(LogicalType from, LogicalType to) noexcept {
    if (from == to)
        return true;
    const int a = NumericRank(from), b = NumericRank(to);
    return a != 0 && b != 0 && a < b;
}

bool CanCastExplicitly(LogicalType from, LogicalType to) noexcept {
    if (from == to || from.id() == TypeId::Varchar || to.id() == TypeId::Varchar)
        return true;
    switch (from.id()) {
    case TypeId::Boolean:
        return to.id() == TypeId::Integer || to.id() == TypeId::BigInt || to.id() == TypeId::Double;
    case TypeId::Integer:
    case TypeId::BigInt:
    case TypeId::Double:
        return to.id() != TypeId::Date;
    case TypeId::Date:
        return false; // only VARCHAR, handled above
    case TypeId::Varchar:
        return true;
    }
    return false;
}

std::optional<LogicalType> CommonSuperType(LogicalType a, LogicalType b) noexcept {
    if (a == b)
        return a;
    const int ra = NumericRank(a), rb = NumericRank(b);
    if (ra != 0 && rb != 0)
        return ra > rb ? a : b;
    return std::nullopt;
}

Value CastValue(const Value& v, LogicalType target) {
    if (v.type() == target)
        return v;
    if (v.IsNull())
        return Value::Null(target);
    if (!CanCastExplicitly(v.type(), target)) {
        Fail("Cannot cast " + v.type().ToString() + " to " + target.ToString());
    }
    const std::string shown = v.ToString();
    switch (v.type().id()) {
    case TypeId::Boolean:
        switch (target.id()) {
        case TypeId::Integer:
            return Value::Integer(v.GetBoolean() ? 1 : 0);
        case TypeId::BigInt:
            return Value::BigInt(v.GetBoolean() ? 1 : 0);
        case TypeId::Double:
            return Value::Double(v.GetBoolean() ? 1.0 : 0.0);
        default:
            return Value::Varchar(shown);
        }
    case TypeId::Integer: {
        const int32_t x = v.GetInteger();
        switch (target.id()) {
        case TypeId::BigInt:
            return Value::BigInt(x);
        case TypeId::Double:
            return Value::Double(static_cast<double>(x));
        case TypeId::Boolean:
            return Value::Boolean(x != 0);
        default:
            return Value::Varchar(shown);
        }
    }
    case TypeId::BigInt: {
        const int64_t x = v.GetBigInt();
        switch (target.id()) {
        case TypeId::Integer:
            return MakeInteger(x, shown);
        case TypeId::Double:
            return Value::Double(static_cast<double>(x));
        case TypeId::Boolean:
            return Value::Boolean(x != 0);
        default:
            return Value::Varchar(shown);
        }
    }
    case TypeId::Double: {
        const double x = v.GetDouble();
        switch (target.id()) {
        case TypeId::Integer:
            return MakeInteger(RoundToInt64(x, target, shown), shown);
        case TypeId::BigInt:
            return Value::BigInt(RoundToInt64(x, target, shown));
        case TypeId::Boolean:
            return Value::Boolean(x != 0.0);
        default:
            return Value::Varchar(shown);
        }
    }
    case TypeId::Date:
        return Value::Varchar(shown);
    case TypeId::Varchar:
        return FromVarchar(v.GetVarchar(), target);
    }
    Fail("Cannot cast " + v.type().ToString() + " to " + target.ToString());
}

date_t AddInterval(date_t date, int64_t amount, const std::string& unit) {
    auto range_error = [&]() {
        return Error(ErrorCode::Execution, "date out of range in interval arithmetic");
    };
    if (unit == "day" || unit == "week") {
        const int64_t days = unit == "week" ? amount * 7 : amount;
        const int64_t result = static_cast<int64_t>(date.days) + days;
        const date_t lo = Date::FromYMD(Date::kMinYear, 1, 1),
                     hi = Date::FromYMD(Date::kMaxYear, 12, 31);
        if (days > std::numeric_limits<int32_t>::max() ||
            days < std::numeric_limits<int32_t>::min() || result < lo.days || result > hi.days) {
            throw range_error();
        }
        return date_t{static_cast<int32_t>(result)};
    }
    if (unit == "month" || unit == "year") {
        const int64_t months = unit == "year" ? amount * 12 : amount;
        int y, m, d;
        Date::ToYMD(date, y, m, d);
        const int64_t total = static_cast<int64_t>(y) * 12 + (m - 1) + months;
        if (total < Date::kMinYear * 12LL || total > Date::kMaxYear * 12LL + 11)
            throw range_error();
        const int ny = static_cast<int>(total / 12);
        const int nm = static_cast<int>(total % 12) + 1;
        const int nd = std::min(d, Date::DaysInMonth(ny, nm)); // clamp to month end
        return Date::FromYMD(ny, nm, nd);
    }
    throw Error(ErrorCode::NotImplemented,
                "interval unit '" + unit + "' is not supported for DATE arithmetic");
}

} // namespace cdb
