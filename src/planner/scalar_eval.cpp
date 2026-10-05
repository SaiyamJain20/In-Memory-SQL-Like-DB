#include "planner/scalar_eval.h"

#include "common/assert.h"
#include "common/error.h"
#include "types/cast.h"

#include <cmath>
#include <limits>

namespace cdb {

namespace {

__extension__ typedef __int128 Int128; // wide intermediates for overflow-free rounding

[[noreturn]] void Overflow(const char* what, LogicalType type, const std::string& expr) {
    throw Error(ErrorCode::Execution, std::string("Out of Range: overflow in ") + what + " of " +
                                          type.ToString() + " (" + expr + ")");
}

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xF0 && lead <= 0xF7)
        return 4;
    if (lead >= 0xE0)
        return 3;
    if (lead >= 0xC0)
        return 2;
    return 1; // stray continuation byte: treat as one character
}

// Byte offset of the start of every character, plus the end offset as the last element.
std::vector<size_t> CharOffsets(std::string_view s) {
    std::vector<size_t> offsets;
    for (size_t i = 0; i < s.size();) {
        offsets.push_back(i);
        i += std::min(Utf8Len(static_cast<unsigned char>(s[i])), s.size() - i);
    }
    offsets.push_back(s.size());
    return offsets;
}

template <class T> Value MakeNumber(T v);
template <> Value MakeNumber<int32_t>(int32_t v) {
    return Value::Integer(v);
}
template <> Value MakeNumber<int64_t>(int64_t v) {
    return Value::BigInt(v);
}

template <class T> Value CheckedArithmetic(OperatorKind op, T a, T b, LogicalType type) {
    T r{};
    const std::string shown = std::to_string(a) + " " + OperatorText(op) + " " + std::to_string(b);
    switch (op) {
    case OperatorKind::Add:
        if (__builtin_add_overflow(a, b, &r))
            Overflow("addition", type, shown);
        return MakeNumber<T>(r);
    case OperatorKind::Sub:
        if (__builtin_sub_overflow(a, b, &r))
            Overflow("subtraction", type, shown);
        return MakeNumber<T>(r);
    case OperatorKind::Mul:
        if (__builtin_mul_overflow(a, b, &r))
            Overflow("multiplication", type, shown);
        return MakeNumber<T>(r);
    case OperatorKind::Mod:
        if (b == 0)
            return Value::Null(type);
        if (b == -1)
            return MakeNumber<T>(0); // avoids the INT_MIN % -1 trap; the result is 0
        return MakeNumber<T>(static_cast<T>(a % b));
    default:
        break;
    }
    CDB_UNREACHABLE("CheckedArithmetic");
}

Value Arithmetic(OperatorKind op, const Value& a, const Value& b) {
    const TypeId ta = a.type().id(), tb = b.type().id();
    if (ta == TypeId::Date || tb == TypeId::Date) {
        auto in_range = [](int64_t days) {
            return days >= Date::FromYMD(Date::kMinYear, 1, 1).days &&
                   days <= Date::FromYMD(Date::kMaxYear, 12, 31).days;
        };
        if (ta == TypeId::Date && tb == TypeId::Date && op == OperatorKind::Sub) {
            return Value::BigInt(static_cast<int64_t>(a.GetDate().days) - b.GetDate().days);
        }
        const bool date_left = ta == TypeId::Date;
        const Value& d = date_left ? a : b;
        const Value& n = date_left ? b : a;
        CDB_CHECK(n.type().id() == TypeId::Integer || n.type().id() == TypeId::BigInt);
        const int64_t days = n.type().id() == TypeId::Integer ? n.GetInteger() : n.GetBigInt();
        int64_t result;
        if (op == OperatorKind::Add) {
            result = static_cast<int64_t>(d.GetDate().days) + days;
        } else if (op == OperatorKind::Sub && date_left) {
            result = static_cast<int64_t>(d.GetDate().days) - days;
        } else {
            CDB_UNREACHABLE("date arithmetic");
        }
        if (!in_range(result)) {
            throw Error(ErrorCode::Execution, "date out of range in arithmetic");
        }
        return Value::Date(date_t{static_cast<int32_t>(result)});
    }
    CDB_CHECK(ta == tb);
    switch (ta) {
    case TypeId::Integer:
        return CheckedArithmetic<int32_t>(op, a.GetInteger(), b.GetInteger(), a.type());
    case TypeId::BigInt:
        return CheckedArithmetic<int64_t>(op, a.GetBigInt(), b.GetBigInt(), a.type());
    case TypeId::Double: {
        const double x = a.GetDouble(), y = b.GetDouble();
        switch (op) {
        case OperatorKind::Add:
            return Value::Double(x + y);
        case OperatorKind::Sub:
            return Value::Double(x - y);
        case OperatorKind::Mul:
            return Value::Double(x * y);
        case OperatorKind::Div:
            return Value::Double(x / y); // IEEE: 1/0 = inf, 0/0 = nan
        case OperatorKind::Mod:
            return Value::Double(std::fmod(x, y)); // x % 0.0 is NaN, like DuckDB
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    CDB_UNREACHABLE("Arithmetic on unsupported types");
}

Value Negate(const Value& v) {
    switch (v.type().id()) {
    case TypeId::Integer:
        if (v.GetInteger() == std::numeric_limits<int32_t>::min()) {
            Overflow("negation", v.type(), "-" + v.ToString());
        }
        return Value::Integer(-v.GetInteger());
    case TypeId::BigInt:
        if (v.GetBigInt() == std::numeric_limits<int64_t>::min()) {
            Overflow("negation", v.type(), "-" + v.ToString());
        }
        return Value::BigInt(-v.GetBigInt());
    case TypeId::Double:
        return Value::Double(-v.GetDouble());
    default:
        break;
    }
    CDB_UNREACHABLE("Negate on non-numeric type");
}

Value Compare(OperatorKind op, const Value& a, const Value& b) {
    const int c = Value::Compare(a, b);
    bool r = false;
    switch (op) {
    case OperatorKind::Eq:
        r = c == 0;
        break;
    case OperatorKind::Ne:
        r = c != 0;
        break;
    case OperatorKind::Lt:
        r = c < 0;
        break;
    case OperatorKind::Le:
        r = c <= 0;
        break;
    case OperatorKind::Gt:
        r = c > 0;
        break;
    case OperatorKind::Ge:
        r = c >= 0;
        break;
    default:
        CDB_UNREACHABLE("Compare");
    }
    return Value::Boolean(r);
}

Value EvaluateOperator(const BoundExpr& e, std::span<const Value> row) {
    const OperatorKind op = e.op;
    const LogicalType null_type = e.type;
    if (op == OperatorKind::And || op == OperatorKind::Or) {
        const Value l = EvaluateScalar(*e.children[0], row);
        const bool is_and = op == OperatorKind::And;
        if (!l.IsNull() && l.GetBoolean() == !is_and)
            return l; // false AND _ / true OR _
        const Value r = EvaluateScalar(*e.children[1], row);
        if (!r.IsNull() && r.GetBoolean() == !is_and)
            return r;
        if (l.IsNull() || r.IsNull())
            return Value::Null(LogicalType::Boolean());
        return Value::Boolean(is_and); // both are the neutral value
    }
    if (op == OperatorKind::Not) {
        const Value v = EvaluateScalar(*e.children[0], row);
        return v.IsNull() ? Value::Null(LogicalType::Boolean()) : Value::Boolean(!v.GetBoolean());
    }
    if (op == OperatorKind::Negate) {
        const Value v = EvaluateScalar(*e.children[0], row);
        return v.IsNull() ? Value::Null(null_type) : Negate(v);
    }
    const Value a = EvaluateScalar(*e.children[0], row);
    const Value b = EvaluateScalar(*e.children[1], row);
    if (a.IsNull() || b.IsNull())
        return Value::Null(null_type);
    switch (op) {
    case OperatorKind::Add:
    case OperatorKind::Sub:
    case OperatorKind::Mul:
    case OperatorKind::Div:
    case OperatorKind::Mod:
        return Arithmetic(op, a, b);
    case OperatorKind::Concat:
        return Value::Varchar(a.GetVarchar() + b.GetVarchar());
    case OperatorKind::Eq:
    case OperatorKind::Ne:
    case OperatorKind::Lt:
    case OperatorKind::Le:
    case OperatorKind::Gt:
    case OperatorKind::Ge:
        return Compare(op, a, b);
    default:
        break;
    }
    CDB_UNREACHABLE("EvaluateOperator");
}

Value Substring(const Value& s, int64_t start, std::optional<int64_t> length) {
    const std::string& text = s.GetVarchar();
    const std::vector<size_t> offs = CharOffsets(text);
    const int64_t n = static_cast<int64_t>(offs.size()) - 1; // characters
    // 0-based begin: start 1 -> 0; start 0 -> -1; negative start counts from the end
    int64_t begin = start > 0 ? start - 1 : (start == 0 ? -1 : n + start);
    int64_t end;
    if (!length) {
        end = n;
    } else if (*length >= 0) {
        end = begin + *length;
    } else { // negative length: that many characters *before* `begin`
        end = begin;
        begin = begin + *length;
    }
    begin = std::clamp<int64_t>(begin, 0, n);
    end = std::clamp<int64_t>(end, 0, n);
    if (begin >= end)
        return Value::Varchar("");
    return Value::Varchar(
        text.substr(offs[static_cast<size_t>(begin)],
                    offs[static_cast<size_t>(end)] - offs[static_cast<size_t>(begin)]));
}

int64_t AsInt64(const Value& v) {
    return v.type().id() == TypeId::Integer ? v.GetInteger() : v.GetBigInt();
}

Value EvaluateFunction(const BoundExpr& e, std::span<const Value> row) {
    // Functions that handle NULL arguments themselves
    if (e.function == FunctionId::Coalesce) {
        for (const auto& c : e.children) {
            Value v = EvaluateScalar(*c, row);
            if (!v.IsNull())
                return v;
        }
        return Value::Null(e.type);
    }
    if (e.function == FunctionId::NullIf) {
        Value a = EvaluateScalar(*e.children[0], row);
        if (a.IsNull())
            return a;
        const Value b = EvaluateScalar(*e.children[1], row);
        if (!b.IsNull() && Value::Compare(a, b) == 0)
            return Value::Null(e.type);
        return a;
    }
    // Everything else is strict: any NULL argument gives NULL.
    std::vector<Value> args;
    args.reserve(e.children.size());
    for (const auto& c : e.children) {
        args.push_back(EvaluateScalar(*c, row));
        if (args.back().IsNull())
            return Value::Null(e.type);
    }
    switch (e.function) {
    case FunctionId::Like:
        return Value::Boolean(LikeMatch(args[0].GetVarchar(), args[1].GetVarchar()));
    case FunctionId::Year:
        return Value::BigInt(Date::Year(args[0].GetDate()));
    case FunctionId::Month:
        return Value::BigInt(Date::Month(args[0].GetDate()));
    case FunctionId::Day:
        return Value::BigInt(Date::Day(args[0].GetDate()));
    case FunctionId::DayOfWeek:
        return Value::BigInt(Date::DayOfWeek(args[0].GetDate()));
    case FunctionId::DayOfYear:
        return Value::BigInt(Date::DayOfYear(args[0].GetDate()));
    case FunctionId::Quarter:
        return Value::BigInt((Date::Month(args[0].GetDate()) - 1) / 3 + 1);
    case FunctionId::Substring:
        return Substring(args[0], AsInt64(args[1]),
                         args.size() > 2 ? std::optional<int64_t>(AsInt64(args[2])) : std::nullopt);
    case FunctionId::Length: {
        int64_t chars = 0;
        const std::string& s = args[0].GetVarchar();
        for (size_t i = 0; i < s.size(); chars++) {
            i += std::min(Utf8Len(static_cast<unsigned char>(s[i])), s.size() - i);
        }
        return Value::BigInt(chars);
    }
    case FunctionId::Upper:
    case FunctionId::Lower: {
        std::string s = args[0].GetVarchar();
        for (char& c : s) { // ASCII case mapping only; other bytes are left untouched
            if (e.function == FunctionId::Upper && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 32);
            if (e.function == FunctionId::Lower && c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + 32);
        }
        return Value::Varchar(std::move(s));
    }
    case FunctionId::Abs: {
        const Value& v = args[0];
        switch (v.type().id()) {
        case TypeId::Integer:
            if (v.GetInteger() == std::numeric_limits<int32_t>::min()) {
                Overflow("abs", v.type(), v.ToString());
            }
            return Value::Integer(std::abs(v.GetInteger()));
        case TypeId::BigInt:
            if (v.GetBigInt() == std::numeric_limits<int64_t>::min()) {
                Overflow("abs", v.type(), v.ToString());
            }
            return Value::BigInt(v.GetBigInt() < 0 ? -v.GetBigInt() : v.GetBigInt());
        default:
            return Value::Double(std::fabs(v.GetDouble()));
        }
    }
    case FunctionId::Round: {
        const int64_t digits = args.size() > 1 ? AsInt64(args[1]) : 0;
        if (args[0].type().IsIntegral()) {
            // ROUND on an integer keeps its type; only negative digits change the value
            // (half away from zero): round(1250, -2) = 1300.
            const int64_t x = AsInt64(args[0]);
            if (digits >= 0)
                return args[0];
            if (digits < -18)
                return args[0].type().id() == TypeId::Integer ? Value::Integer(0)
                                                              : Value::BigInt(0);
            Int128 p = 1;
            for (int64_t i = 0; i < -digits; i++)
                p *= 10;
            const Int128 ax = x < 0 ? -static_cast<Int128>(x) : x;
            const Int128 rounded = (ax + p / 2) / p * p;
            const Int128 result = x < 0 ? -rounded : rounded;
            if (args[0].type().id() == TypeId::Integer) {
                if (result > std::numeric_limits<int32_t>::max() ||
                    result < std::numeric_limits<int32_t>::min())
                    Overflow("round", args[0].type(), args[0].ToString());
                return Value::Integer(static_cast<int32_t>(result));
            }
            if (result > std::numeric_limits<int64_t>::max() ||
                result < std::numeric_limits<int64_t>::min())
                Overflow("round", args[0].type(), args[0].ToString());
            return Value::BigInt(static_cast<int64_t>(result));
        }
        const double x = args[0].GetDouble();
        if (digits == 0 || !std::isfinite(x))
            return Value::Double(std::round(x));
        const double clamped = static_cast<double>(std::clamp<int64_t>(digits, -308, 308));
        if (digits > 0) {
            const double factor = std::pow(10.0, clamped);
            const double scaled = x * factor;
            if (!std::isfinite(scaled))
                return Value::Double(x);
            return Value::Double(std::round(scaled) / factor);
        }
        const double factor =
            std::pow(10.0, -clamped); // negative digits: round to tens, hundreds...
        return Value::Double(std::round(x / factor) * factor);
    }
    case FunctionId::Floor:
        return Value::Double(std::floor(args[0].GetDouble()));
    case FunctionId::Ceil:
        return Value::Double(std::ceil(args[0].GetDouble()));
    case FunctionId::Coalesce:
    case FunctionId::NullIf:
        break;
    }
    CDB_UNREACHABLE("EvaluateFunction");
}

} // namespace

bool LikeMatch(std::string_view text, std::string_view pattern) {
    size_t t = 0, p = 0;
    size_t star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '%') {
            star_p = p++;
            star_t = t;
        } else if (p < pattern.size() && pattern[p] == '_') {
            t += std::min(Utf8Len(static_cast<unsigned char>(text[t])), text.size() - t);
            p++;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            p++;
            t++;
        } else if (star_p != std::string_view::npos) {
            // backtrack: let the last '%' swallow one more character
            star_t +=
                std::min(Utf8Len(static_cast<unsigned char>(text[star_t])), text.size() - star_t);
            t = star_t;
            p = star_p + 1;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
        p++;
    return p == pattern.size();
}

Value EvaluateScalar(const BoundExpr& e, std::span<const Value> row) {
    switch (e.kind) {
    case BoundKind::ColumnRef:
        CDB_CHECK(e.ordinal < row.size());
        return row[e.ordinal];
    case BoundKind::Constant:
        return *e.value;
    case BoundKind::Cast:
        return CastValue(EvaluateScalar(*e.children[0], row), e.type);
    case BoundKind::Operator:
        return EvaluateOperator(e, row);
    case BoundKind::Function:
        return EvaluateFunction(e, row);
    case BoundKind::Case: {
        for (size_t i = 0; i + 1 < e.children.size(); i += 2) {
            const Value cond = EvaluateScalar(*e.children[i], row);
            if (!cond.IsNull() && cond.GetBoolean())
                return EvaluateScalar(*e.children[i + 1], row);
        }
        return EvaluateScalar(*e.children.back(), row);
    }
    case BoundKind::InList: {
        const Value v = EvaluateScalar(*e.children[0], row);
        if (v.IsNull())
            return Value::Null(LogicalType::Boolean());
        bool saw_null = false;
        for (size_t i = 1; i < e.children.size(); i++) {
            const Value item = EvaluateScalar(*e.children[i], row);
            if (item.IsNull()) {
                saw_null = true;
            } else if (Value::Compare(v, item) == 0) {
                return Value::Boolean(!e.flag);
            }
        }
        return saw_null ? Value::Null(LogicalType::Boolean()) : Value::Boolean(e.flag);
    }
    case BoundKind::IsNull: {
        const bool is_null = EvaluateScalar(*e.children[0], row).IsNull();
        return Value::Boolean(e.flag ? !is_null : is_null);
    }
    case BoundKind::Aggregate:
        throw Error(ErrorCode::Internal, "aggregate expressions cannot be evaluated row-at-a-time");
    }
    CDB_UNREACHABLE("EvaluateScalar");
}

} // namespace cdb
