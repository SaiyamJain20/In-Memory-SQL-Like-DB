#include "execution/expression_executor.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/string_ops.h"
#include "planner/scalar_eval.h"
#include "types/cast.h"
#include "types/date.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <type_traits>

namespace cdb {

// ====================================================================================
// Per-expression-node state
// ====================================================================================

// A compiled constant LIKE pattern: the common shapes ('abc', 'abc%', '%abc', '%abc%', '%') are
// answered with plain byte comparisons; everything else uses the general matcher.
struct LikePattern {
    enum class Kind { All, Exact, Prefix, Suffix, Contains, General };
    Kind kind = Kind::General;
    std::string text;    // the literal part for the fast shapes
    std::string pattern; // the full pattern, for Kind::General

    explicit LikePattern(const std::string& p) : pattern(p) {
        if (p.find('_') != std::string::npos)
            return; // '_' needs the general matcher
        const bool lead = !p.empty() && p.front() == '%';
        const bool trail = p.size() > 1 && p.back() == '%';
        std::string core = p.substr(lead ? 1 : 0, p.size() - (lead ? 1 : 0) - (trail ? 1 : 0));
        if (core.find('%') != std::string::npos)
            return; // an interior '%'
        // A leading '%' lets the match start at any character boundary. Byte-wise search is only
        // equivalent when the literal starts with an ASCII byte (always a boundary, see
        // Utf8CharLength); otherwise leave it to the general matcher.
        if (lead && !core.empty() && static_cast<unsigned char>(core[0]) >= 0x80)
            return;
        if (p == "%" || (lead && p.size() == 1)) {
            kind = Kind::All;
        } else if (lead && trail) {
            kind = Kind::Contains;
        } else if (lead) {
            kind = Kind::Suffix;
        } else if (trail) {
            kind = Kind::Prefix;
        } else {
            kind = Kind::Exact;
        }
        text = std::move(core);
    }

    bool Match(std::string_view s) const {
        switch (kind) {
        case Kind::All:
            return true;
        case Kind::Exact:
            return s == text;
        case Kind::Prefix:
            return s.size() >= text.size() && s.compare(0, text.size(), text) == 0;
        case Kind::Suffix:
            return s.size() >= text.size() &&
                   s.compare(s.size() - text.size(), text.size(), text) == 0;
        case Kind::Contains:
            return s.find(text) != std::string_view::npos;
        case Kind::General:
            return LikeMatch(s, pattern);
        }
        return false;
    }
};

struct ExprNode {
    const BoundExpr* expr;
    std::vector<std::unique_ptr<ExprNode>> children;
    Vector buffer;                   // this node's result (or a sliced view of an input column)
    std::optional<LikePattern> like; // set for LIKE with a constant, non-NULL pattern

    explicit ExprNode(const BoundExpr& e) : expr(&e), buffer(e.type) {
        for (const auto& c : e.children)
            children.push_back(std::make_unique<ExprNode>(*c));
        if (e.kind == BoundKind::Constant) {
            buffer.SetConstant(*e.value);
        } else if (e.kind == BoundKind::Function && e.function == FunctionId::Like &&
                   e.children[1]->kind == BoundKind::Constant && !e.children[1]->value->IsNull()) {
            like.emplace(e.children[1]->value->GetVarchar());
        }
    }
};

namespace {

// ====================================================================================
// Views and inputs
// ====================================================================================

struct View {
    UnifiedFormat u;
    explicit View(const Vector& v) { v.ToUnified(u); }
    template <class T> const T* data() const { return u.Data<T>(); }
    bool AllValid() const noexcept { return u.validity->AllValid(); }
    bool Valid(idx_t i) const noexcept { return u.validity->IsValid(u.sel[i]); }
    sel_t Slot(idx_t i) const noexcept { return u.sel[i]; }
};

// The rows being evaluated: logical row i is row `sel ? (*sel)[i] : i` of `chunk`.
struct Input {
    const DataChunk* chunk;
    const SelectionVector* sel;
    idx_t count;
};

// Rows `rows[0..n)` (positions within `in`) as a new Input; `storage` keeps the composed selection.
Input Subset(const Input& in, const sel_t* rows, idx_t n, SelectionVector& storage) {
    storage = SelectionVector(std::max<idx_t>(n, 1));
    for (idx_t j = 0; j < n; j++) {
        storage.Set(j, in.sel ? (*in.sel)[rows[j]] : rows[j]);
    }
    return Input{in.chunk, &storage, n};
}

template <class F> decltype(auto) DispatchType(PhysicalType t, F&& f) {
    switch (t) {
    case PhysicalType::Bool:
        return f(bool{});
    case PhysicalType::Int32:
        return f(int32_t{});
    case PhysicalType::Int64:
        return f(int64_t{});
    case PhysicalType::Double:
        return f(double{});
    case PhysicalType::String:
        return f(string_t{});
    }
    CDB_UNREACHABLE("DispatchType");
}

[[noreturn]] void Internal(const std::string& what) {
    throw Error(ErrorCode::Internal, what);
}

// ====================================================================================
// Comparison (total order for doubles, like Value::Compare)
// ====================================================================================

template <class T> struct LessThan {
    static bool Apply(const T& a, const T& b) { return a < b; }
};
template <> struct LessThan<double> {
    static bool Apply(double a, double b) { return a < b || (!std::isnan(a) && std::isnan(b)); }
};
template <> struct LessThan<string_t> {
    static bool Apply(const string_t& a, const string_t& b) { return string_t::Compare(a, b) < 0; }
};
template <class T> struct Equals {
    static bool Apply(const T& a, const T& b) { return a == b; }
};
template <> struct Equals<double> {
    static bool Apply(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }
};

template <class T> struct CmpEq {
    static bool Apply(const T& a, const T& b) { return Equals<T>::Apply(a, b); }
};
template <class T> struct CmpNe {
    static bool Apply(const T& a, const T& b) { return !Equals<T>::Apply(a, b); }
};
template <class T> struct CmpLt {
    static bool Apply(const T& a, const T& b) { return LessThan<T>::Apply(a, b); }
};
template <class T> struct CmpLe {
    static bool Apply(const T& a, const T& b) { return !LessThan<T>::Apply(b, a); }
};
template <class T> struct CmpGt {
    static bool Apply(const T& a, const T& b) { return LessThan<T>::Apply(b, a); }
};
template <class T> struct CmpGe {
    static bool Apply(const T& a, const T& b) { return !LessThan<T>::Apply(a, b); }
};

template <class T, class CMP>
void CompareExecute(const Vector& lv, const Vector& rv, Vector& out, idx_t count) {
    const View l(lv), r(rv);
    const T* ld = l.data<T>();
    const T* rd = r.data<T>();
    bool* od = out.FlatData<bool>();
    if (l.AllValid() && r.AllValid()) {
        for (idx_t i = 0; i < count; i++) {
            od[i] = CMP::Apply(ld[l.u.sel[i]], rd[r.u.sel[i]]);
        }
        return;
    }
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i)) {
            ov.SetInvalid(i);
            od[i] = false;
        } else {
            od[i] = CMP::Apply(ld[l.u.sel[i]], rd[r.u.sel[i]]);
        }
    }
}

// Writes the indices of the rows where the comparison is TRUE; NULLs never match.
template <class T, class CMP>
idx_t CompareSelect(const Vector& lv, const Vector& rv, idx_t count, sel_t* out) {
    const View l(lv), r(rv);
    const T* ld = l.data<T>();
    const T* rd = r.data<T>();
    idx_t n = 0;
    if (l.AllValid() && r.AllValid()) {
        for (idx_t i = 0; i < count; i++) {
            const bool match = CMP::Apply(ld[l.u.sel[i]], rd[r.u.sel[i]]);
            out[n] = static_cast<sel_t>(i);
            n += match; // branch-free append
        }
        return n;
    }
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i))
            continue;
        const bool match = CMP::Apply(ld[l.u.sel[i]], rd[r.u.sel[i]]);
        out[n] = static_cast<sel_t>(i);
        n += match;
    }
    return n;
}

template <class T>
void CompareExecuteT(OperatorKind op, const Vector& l, const Vector& r, Vector& out, idx_t n) {
    switch (op) {
    case OperatorKind::Eq:
        return CompareExecute<T, CmpEq<T>>(l, r, out, n);
    case OperatorKind::Ne:
        return CompareExecute<T, CmpNe<T>>(l, r, out, n);
    case OperatorKind::Lt:
        return CompareExecute<T, CmpLt<T>>(l, r, out, n);
    case OperatorKind::Le:
        return CompareExecute<T, CmpLe<T>>(l, r, out, n);
    case OperatorKind::Gt:
        return CompareExecute<T, CmpGt<T>>(l, r, out, n);
    case OperatorKind::Ge:
        return CompareExecute<T, CmpGe<T>>(l, r, out, n);
    default:
        Internal("not a comparison");
    }
}

template <class T>
idx_t CompareSelectT(OperatorKind op, const Vector& l, const Vector& r, idx_t n, sel_t* out) {
    switch (op) {
    case OperatorKind::Eq:
        return CompareSelect<T, CmpEq<T>>(l, r, n, out);
    case OperatorKind::Ne:
        return CompareSelect<T, CmpNe<T>>(l, r, n, out);
    case OperatorKind::Lt:
        return CompareSelect<T, CmpLt<T>>(l, r, n, out);
    case OperatorKind::Le:
        return CompareSelect<T, CmpLe<T>>(l, r, n, out);
    case OperatorKind::Gt:
        return CompareSelect<T, CmpGt<T>>(l, r, n, out);
    case OperatorKind::Ge:
        return CompareSelect<T, CmpGe<T>>(l, r, n, out);
    default:
        Internal("not a comparison");
    }
}

bool IsComparison(OperatorKind op) {
    return op == OperatorKind::Eq || op == OperatorKind::Ne || op == OperatorKind::Lt ||
           op == OperatorKind::Le || op == OperatorKind::Gt || op == OperatorKind::Ge;
}

// NOT (a < b) is (a >= b) for a total order, which lets `NOT <comparison>` stay a selection.
OperatorKind Negated(OperatorKind op) {
    switch (op) {
    case OperatorKind::Eq:
        return OperatorKind::Ne;
    case OperatorKind::Ne:
        return OperatorKind::Eq;
    case OperatorKind::Lt:
        return OperatorKind::Ge;
    case OperatorKind::Le:
        return OperatorKind::Gt;
    case OperatorKind::Gt:
        return OperatorKind::Le;
    case OperatorKind::Ge:
        return OperatorKind::Lt;
    default:
        Internal("not a comparison");
    }
}

// ====================================================================================
// Arithmetic
// ====================================================================================

struct AddOp {
    static constexpr const char* kName = "addition";
    static constexpr char kSym = '+';
    template <class T> static bool Apply(T a, T b, T& out) {
        if constexpr (std::is_floating_point_v<T>) {
            out = a + b;
            return false;
        } else {
            return __builtin_add_overflow(a, b, &out);
        }
    }
};
struct SubOp {
    static constexpr const char* kName = "subtraction";
    static constexpr char kSym = '-';
    template <class T> static bool Apply(T a, T b, T& out) {
        if constexpr (std::is_floating_point_v<T>) {
            out = a - b;
            return false;
        } else {
            return __builtin_sub_overflow(a, b, &out);
        }
    }
};
struct MulOp {
    static constexpr const char* kName = "multiplication";
    static constexpr char kSym = '*';
    template <class T> static bool Apply(T a, T b, T& out) {
        if constexpr (std::is_floating_point_v<T>) {
            out = a * b;
            return false;
        } else {
            return __builtin_mul_overflow(a, b, &out);
        }
    }
};

template <class T> std::string Show(T v) {
    return std::to_string(v);
}

template <class T, class OP>
[[noreturn]] void ReportOverflow(const View& l, const View& r, idx_t count, LogicalType type) {
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i))
            continue;
        const T a = l.data<T>()[l.u.sel[i]], b = r.data<T>()[r.u.sel[i]];
        T tmp;
        if (OP::template Apply<T>(a, b, tmp)) {
            throw Error(ErrorCode::Execution, std::string("Out of Range: overflow in ") +
                                                  OP::kName + " of " + type.ToString() + " (" +
                                                  Show(a) + " " + OP::kSym + " " + Show(b) + ")");
        }
    }
    Internal("overflow reported but not found");
}

template <class T, class OP>
void ArithmeticKernel(const Vector& lv, const Vector& rv, Vector& out, idx_t count) {
    const View l(lv), r(rv);
    const T* ld = l.data<T>();
    const T* rd = r.data<T>();
    T* od = out.FlatData<T>();
    bool overflow = false;
    if (l.AllValid() && r.AllValid()) {
        for (idx_t i = 0; i < count; i++) {
            overflow |= OP::template Apply<T>(ld[l.u.sel[i]], rd[r.u.sel[i]], od[i]);
        }
    } else {
        ValidityMask& ov = out.Validity();
        for (idx_t i = 0; i < count; i++) {
            if (!l.Valid(i) || !r.Valid(i)) {
                ov.SetInvalid(i);
                continue;
            }
            overflow |= OP::template Apply<T>(ld[l.u.sel[i]], rd[r.u.sel[i]], od[i]);
        }
    }
    if (overflow)
        ReportOverflow<T, OP>(l, r, count, lv.type());
}

template <class OP>
void ArithmeticByType(PhysicalType t, const Vector& l, const Vector& r, Vector& out, idx_t n) {
    switch (t) {
    case PhysicalType::Int32:
        return ArithmeticKernel<int32_t, OP>(l, r, out, n);
    case PhysicalType::Int64:
        return ArithmeticKernel<int64_t, OP>(l, r, out, n);
    case PhysicalType::Double:
        return ArithmeticKernel<double, OP>(l, r, out, n);
    default:
        Internal("arithmetic on a non-numeric type");
    }
}

void DivideKernel(const Vector& lv, const Vector& rv, Vector& out, idx_t count) {
    const View l(lv), r(rv);
    const double* ld = l.data<double>();
    const double* rd = r.data<double>();
    double* od = out.FlatData<double>();
    ValidityMask& ov = out.Validity();
    const bool all_valid = l.AllValid() && r.AllValid();
    for (idx_t i = 0; i < count; i++) {
        if (!all_valid && (!l.Valid(i) || !r.Valid(i))) {
            ov.SetInvalid(i);
            continue;
        }
        od[i] = ld[l.u.sel[i]] / rd[r.u.sel[i]]; // IEEE: 1/0 = inf, 0/0 = nan
    }
}

template <class T> void ModuloKernel(const Vector& lv, const Vector& rv, Vector& out, idx_t count) {
    const View l(lv), r(rv);
    const T* ld = l.data<T>();
    const T* rd = r.data<T>();
    T* od = out.FlatData<T>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        const T a = ld[l.u.sel[i]], b = rd[r.u.sel[i]];
        if constexpr (std::is_floating_point_v<T>) {
            od[i] = std::fmod(a, b); // x % 0.0 is NaN
        } else if (b == 0) {
            ov.SetInvalid(i); // integer modulo by zero is NULL
        } else if (b == -1) {
            od[i] = 0; // avoids the INT_MIN % -1 trap
        } else {
            od[i] = static_cast<T>(a % b);
        }
    }
}

template <class T> void NegateKernel(const Vector& cv, Vector& out, idx_t count) {
    const View c(cv);
    const T* cd = c.data<T>();
    T* od = out.FlatData<T>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!c.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        const T v = cd[c.u.sel[i]];
        if constexpr (std::is_floating_point_v<T>) {
            od[i] = -v;
        } else {
            if (v == std::numeric_limits<T>::min()) {
                throw Error(ErrorCode::Execution, "Out of Range: overflow in negation of " +
                                                      cv.type().ToString() + " (-" + Show(v) + ")");
            }
            od[i] = -v;
        }
    }
}

void DateArithmeticKernel(OperatorKind op, const Vector& lv, const Vector& rv, Vector& out,
                          idx_t count) {
    const View l(lv), r(rv);
    const bool l_date = lv.type().id() == TypeId::Date, r_date = rv.type().id() == TypeId::Date;
    ValidityMask& ov = out.Validity();
    const int64_t lo = Date::FromYMD(Date::kMinYear, 1, 1).days;
    const int64_t hi = Date::FromYMD(Date::kMaxYear, 12, 31).days;
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        const int64_t a = l.data<int32_t>()[l.u.sel[i]];
        const int64_t b = r.data<int32_t>()[r.u.sel[i]];
        if (l_date && r_date) { // DATE - DATE -> BIGINT days
            out.FlatData<int64_t>()[i] = a - b;
            continue;
        }
        const int64_t days =
            op == OperatorKind::Add ? a + b : a - b; // DATE +- INTEGER, INTEGER + DATE
        if (days < lo || days > hi) {
            throw Error(ErrorCode::Execution, "date out of range in arithmetic");
        }
        out.FlatData<int32_t>()[i] = static_cast<int32_t>(days);
    }
}

void ConcatKernel(const Vector& lv, const Vector& rv, Vector& out, idx_t count) {
    const View l(lv), r(rv);
    const string_t* ld = l.data<string_t>();
    const string_t* rd = r.data<string_t>();
    string_t* od = out.FlatData<string_t>();
    ValidityMask& ov = out.Validity();
    std::string tmp;
    for (idx_t i = 0; i < count; i++) {
        if (!l.Valid(i) || !r.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        const std::string_view a = ld[l.u.sel[i]].view(), b = rd[r.u.sel[i]].view();
        tmp.assign(a);
        tmp.append(b);
        od[i] = out.AddString(tmp);
    }
}

// ====================================================================================
// Boolean logic, IS NULL, IN
// ====================================================================================

// Three-valued states: 0 = FALSE, 1 = TRUE, 2 = NULL.
inline int BoolState(bool valid, bool value) noexcept {
    return !valid ? 2 : (value ? 1 : 0);
}
inline int And3(int a, int b) noexcept {
    if (a == 0 || b == 0)
        return 0;
    if (a == 2 || b == 2)
        return 2;
    return 1;
}
inline int Or3(int a, int b) noexcept {
    if (a == 1 || b == 1)
        return 1;
    if (a == 2 || b == 2)
        return 2;
    return 0;
}
inline void WriteBool(Vector& out, idx_t i, int state) {
    out.FlatData<bool>()[i] = state == 1;
    if (state == 2)
        out.Validity().SetInvalid(i);
}

void NotKernel(const Vector& cv, Vector& out, idx_t count) {
    const View c(cv);
    const bool* cd = c.data<bool>();
    for (idx_t i = 0; i < count; i++) {
        WriteBool(out, i, c.Valid(i) ? (cd[c.u.sel[i]] ? 0 : 1) : 2);
    }
}

void IsNullKernel(const Vector& cv, bool negated, Vector& out, idx_t count) {
    const View c(cv);
    bool* od = out.FlatData<bool>();
    for (idx_t i = 0; i < count; i++)
        od[i] = c.Valid(i) == negated;
}

// result = value IN (items...) with SQL three-valued semantics. All operands have the same type.
template <class T>
void InListKernel(const Vector& value, const std::vector<const Vector*>& items, bool negated,
                  Vector& out, idx_t count) {
    const View v(value);
    const T* vd = v.data<T>();
    std::vector<uint8_t> found(count, 0), saw_null(count, 0);
    for (const Vector* item : items) {
        const View it(*item);
        const T* id = it.data<T>();
        for (idx_t i = 0; i < count; i++) {
            if (!it.Valid(i)) {
                saw_null[i] = 1;
            } else if (v.Valid(i) && Equals<T>::Apply(vd[v.u.sel[i]], id[it.u.sel[i]])) {
                found[i] = 1;
            }
        }
    }
    for (idx_t i = 0; i < count; i++) {
        int state;
        if (!v.Valid(i))
            state = 2;
        else if (found[i])
            state = negated ? 0 : 1;
        else if (saw_null[i])
            state = 2;
        else
            state = negated ? 1 : 0;
        WriteBool(out, i, state);
    }
}

// ====================================================================================
// Casts
// ====================================================================================

// Handles the common numeric/boolean conversions without leaving the typed domain. Returns false
// when it cannot (or when a value needs an error / special rounding): the caller then takes the
// general per-row path, which uses CastValue and therefore throws the proper error.
template <class From, class To, class F>
bool CastLoop(const View& s, Vector& out, idx_t count, F&& convert) {
    const From* sd = s.data<From>();
    To* od = out.FlatData<To>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!s.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        if (!convert(sd[s.u.sel[i]], od[i]))
            return false;
    }
    return true;
}

bool FastNumericCast(const Vector& src, Vector& out, idx_t count) {
    const View s(src);
    const TypeId from = src.type().id(), to = out.type().id();
    auto widen = [](auto v, auto& o) {
        o = static_cast<std::remove_reference_t<decltype(o)>>(v);
        return true;
    };
    auto to_bool = [](auto v, bool& o) {
        o = v != 0;
        return true;
    };
    switch (from) {
    case TypeId::Boolean:
        if (to == TypeId::Integer)
            return CastLoop<bool, int32_t>(s, out, count, widen);
        if (to == TypeId::BigInt)
            return CastLoop<bool, int64_t>(s, out, count, widen);
        if (to == TypeId::Double)
            return CastLoop<bool, double>(s, out, count, widen);
        return false;
    case TypeId::Integer:
        if (to == TypeId::BigInt)
            return CastLoop<int32_t, int64_t>(s, out, count, widen);
        if (to == TypeId::Double)
            return CastLoop<int32_t, double>(s, out, count, widen);
        if (to == TypeId::Boolean)
            return CastLoop<int32_t, bool>(s, out, count, to_bool);
        return false;
    case TypeId::BigInt:
        if (to == TypeId::Double)
            return CastLoop<int64_t, double>(s, out, count, widen);
        if (to == TypeId::Boolean)
            return CastLoop<int64_t, bool>(s, out, count, to_bool);
        if (to == TypeId::Integer) {
            return CastLoop<int64_t, int32_t>(s, out, count, [](int64_t v, int32_t& o) {
                if (v < std::numeric_limits<int32_t>::min() ||
                    v > std::numeric_limits<int32_t>::max()) {
                    return false;
                }
                o = static_cast<int32_t>(v);
                return true;
            });
        }
        return false;
    case TypeId::Double:
        if (to == TypeId::Boolean)
            return CastLoop<double, bool>(s, out, count, to_bool);
        if (to == TypeId::Integer) {
            return CastLoop<double, int32_t>(s, out, count, [](double v, int32_t& o) {
                const double r = std::nearbyint(v); // half to even, like CastValue
                if (!(r >= -2147483648.0 && r <= 2147483647.0))
                    return false; // NaN fails too
                o = static_cast<int32_t>(r);
                return true;
            });
        }
        if (to == TypeId::BigInt) {
            return CastLoop<double, int64_t>(s, out, count, [](double v, int64_t& o) {
                const double r = std::nearbyint(v);
                if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
                    return false;
                o = static_cast<int64_t>(r);
                return true;
            });
        }
        return false;
    default:
        return false;
    }
}

void CastKernel(const Vector& src, Vector& out, idx_t count) {
    // A failed fast attempt may have written some rows; the slow path rewrites all of them.
    out.Validity().Reset(out.capacity());
    if (FastNumericCast(src, out, count))
        return;
    out.Validity().Reset(out.capacity());
    for (idx_t i = 0; i < count; i++) {
        const Value v = src.GetValue(i);
        out.SetValue(i, v.IsNull() ? Value::Null(out.type()) : CastValue(v, out.type()));
    }
}

// ====================================================================================
// Functions
// ====================================================================================

template <class F> void MapDate(const Vector& cv, Vector& out, idx_t count, F&& fn) {
    const View c(cv);
    const int32_t* cd = c.data<int32_t>();
    int64_t* od = out.FlatData<int64_t>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!c.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        od[i] = fn(date_t{cd[c.u.sel[i]]});
    }
}

void SubstringKernel(const std::vector<const Vector*>& args, Vector& out, idx_t count) {
    const View s(*args[0]), a(*args[1]);
    std::optional<View> len;
    if (args.size() > 2)
        len.emplace(*args[2]);
    const bool a64 = args[1]->type().id() == TypeId::BigInt;
    const bool l64 = args.size() > 2 && args[2]->type().id() == TypeId::BigInt;
    string_t* od = out.FlatData<string_t>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!s.Valid(i) || !a.Valid(i) || (len && !len->Valid(i))) {
            ov.SetInvalid(i);
            continue;
        }
        const int64_t start = a64 ? a.data<int64_t>()[a.u.sel[i]] : a.data<int32_t>()[a.u.sel[i]];
        std::optional<long long> length;
        if (len)
            length =
                l64 ? len->data<int64_t>()[len->u.sel[i]] : len->data<int32_t>()[len->u.sel[i]];
        od[i] = out.AddString(SubstringView(s.data<string_t>()[s.u.sel[i]].view(), start, length));
    }
}

template <class F> void MapString(const Vector& cv, Vector& out, idx_t count, F&& fn) {
    const View c(cv);
    const string_t* cd = c.data<string_t>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!c.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        fn(cd[c.u.sel[i]].view(), i);
    }
}

void AbsKernel(const Vector& cv, Vector& out, idx_t count) {
    const View c(cv);
    ValidityMask& ov = out.Validity();
    DispatchType(cv.type().physical(), [&](auto tag) {
        using T = decltype(tag);
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            const T* cd = c.data<T>();
            T* od = out.FlatData<T>();
            for (idx_t i = 0; i < count; i++) {
                if (!c.Valid(i)) {
                    ov.SetInvalid(i);
                    continue;
                }
                const T v = cd[c.u.sel[i]];
                if constexpr (std::is_floating_point_v<T>) {
                    od[i] = std::fabs(v);
                } else {
                    if (v == std::numeric_limits<T>::min()) {
                        throw Error(ErrorCode::Execution, "Out of Range: overflow in abs of " +
                                                              cv.type().ToString() + " (" +
                                                              Show(v) + ")");
                    }
                    od[i] = v < 0 ? -v : v;
                }
            }
        } else {
            Internal("abs on a non-numeric type");
        }
    });
}

template <class F> void MapDouble(const Vector& cv, Vector& out, idx_t count, F&& fn) {
    const View c(cv);
    const double* cd = c.data<double>();
    double* od = out.FlatData<double>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!c.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        od[i] = fn(cd[c.u.sel[i]]);
    }
}

void LikeKernel(const ExprNode& n, const Vector& text, const Vector* pattern, Vector& out,
                idx_t count) {
    const View t(text);
    const string_t* td = t.data<string_t>();
    bool* od = out.FlatData<bool>();
    ValidityMask& ov = out.Validity();
    if (n.like) { // constant pattern (the usual case): compiled shapes
        for (idx_t i = 0; i < count; i++) {
            if (!t.Valid(i)) {
                ov.SetInvalid(i);
                od[i] = false;
                continue;
            }
            od[i] = n.like->Match(td[t.u.sel[i]].view());
        }
        return;
    }
    const View p(*pattern);
    const string_t* pd = p.data<string_t>();
    for (idx_t i = 0; i < count; i++) {
        if (!t.Valid(i) || !p.Valid(i)) {
            ov.SetInvalid(i);
            od[i] = false;
            continue;
        }
        od[i] = LikeMatch(td[t.u.sel[i]].view(), pd[p.u.sel[i]].view());
    }
}

template <class T> void NullIfKernel(const Vector& av, const Vector& bv, Vector& out, idx_t count) {
    const View a(av), b(bv);
    const T* ad = a.data<T>();
    const T* bd = b.data<T>();
    T* od = out.FlatData<T>();
    ValidityMask& ov = out.Validity();
    for (idx_t i = 0; i < count; i++) {
        if (!a.Valid(i)) {
            ov.SetInvalid(i);
            continue;
        }
        const T& x = ad[a.u.sel[i]];
        if (b.Valid(i) && Equals<T>::Apply(x, bd[b.u.sel[i]])) {
            ov.SetInvalid(i);
            continue;
        }
        if constexpr (std::is_same_v<T, string_t>) {
            od[i] = x.IsInlined() ? x : out.AddString(x.view());
        } else {
            od[i] = x;
        }
    }
}

// ====================================================================================
// Row-subset helpers (lazy evaluation)
// ====================================================================================

// Copies `src[j]` into `out[rows[j]]` for j < n (types equal), maintaining validity.
void ScatterRows(Vector& out, const sel_t* rows, idx_t n, const Vector& src) {
    const View s(src);
    ValidityMask& ov = out.Validity();
    DispatchType(out.type().physical(), [&](auto tag) {
        using T = decltype(tag);
        const T* sd = s.data<T>();
        T* od = out.FlatData<T>();
        for (idx_t j = 0; j < n; j++) {
            const idx_t i = rows[j];
            if (!s.Valid(j)) {
                ov.SetInvalid(i);
                continue;
            }
            ov.SetValid(i);
            if constexpr (std::is_same_v<T, string_t>) {
                const string_t& v = sd[s.u.sel[j]];
                od[i] = v.IsInlined() ? v : out.AddString(v.view());
            } else {
                od[i] = sd[s.u.sel[j]];
            }
        }
    });
}

// rows in [0, count) that are not in the sorted list `taken`.
void Complement(const sel_t* taken, idx_t n_taken, idx_t count, std::vector<sel_t>& out) {
    out.clear();
    idx_t t = 0;
    for (idx_t i = 0; i < count; i++) {
        if (t < n_taken && taken[t] == i) {
            t++;
        } else {
            out.push_back(static_cast<sel_t>(i));
        }
    }
}

// Merges two sorted, disjoint index lists.
idx_t MergeSorted(const sel_t* a, idx_t na, const sel_t* b, idx_t nb, sel_t* out) {
    idx_t i = 0, j = 0, n = 0;
    while (i < na && j < nb)
        out[n++] = a[i] < b[j] ? a[i++] : b[j++];
    while (i < na)
        out[n++] = a[i++];
    while (j < nb)
        out[n++] = b[j++];
    return n;
}

void EnsureCapacity(SelectionVector& sel, idx_t count) {
    if (!sel.IsSet() || sel.capacity() < count)
        sel = SelectionVector(std::max<idx_t>(count, 1));
}

// ====================================================================================
// Evaluation
// ====================================================================================

const Vector& Eval(ExprNode& n, const Input& in);
void EvalInto(ExprNode& n, const Input& in, Vector& out);
idx_t SelectBool(ExprNode& n, const Input& in, bool want, SelectionVector& out);

// Evaluates an arbitrary expression row by row through the reference interpreter. Used only for
// rarely needed combinations (ROUND), where clarity beats speed.
void FallbackEvaluate(const BoundExpr& e, const Input& in, Vector& out) {
    const DataChunk& chunk = *in.chunk;
    std::vector<idx_t> used;
    e.ForEach([&](const BoundExpr& x) {
        if (x.kind == BoundKind::ColumnRef)
            used.push_back(x.ordinal);
    });
    std::vector<Value> row(chunk.ColumnCount(), Value::Null(LogicalType::Integer()));
    for (idx_t i = 0; i < in.count; i++) {
        const idx_t phys = in.sel ? (*in.sel)[i] : i;
        for (idx_t c : used)
            row[c] = chunk.column(c).GetValue(phys);
        out.SetValue(i, EvaluateScalar(e, row));
    }
}

void EvalAndOr(ExprNode& n, const Input& in, Vector& out) {
    const bool is_and = n.expr->op == OperatorKind::And;
    const idx_t count = in.count;
    const Vector& l = Eval(*n.children[0], in);
    const View lv(l);
    const bool* ld = lv.data<bool>();
    // Rows whose left operand already decides the result (FALSE for AND, TRUE for OR) never
    // evaluate the right side.
    std::vector<sel_t> need;
    need.reserve(count);
    bool* od = out.FlatData<bool>();
    for (idx_t i = 0; i < count; i++) {
        const bool decided = lv.Valid(i) && ld[lv.Slot(i)] == !is_and;
        if (decided) {
            od[i] = !is_and;
        } else {
            need.push_back(static_cast<sel_t>(i));
        }
    }
    if (need.empty())
        return;
    SelectionVector storage;
    const bool all = need.size() == count;
    const Input in2 = all ? in : Subset(in, need.data(), need.size(), storage);
    const Vector& r = Eval(*n.children[1], in2);
    const View rv(r);
    const bool* rd = rv.data<bool>();
    for (idx_t j = 0; j < need.size(); j++) {
        const idx_t i = need[j];
        const int ls = BoolState(lv.Valid(i), ld[lv.Slot(i)]);
        const int rs = BoolState(rv.Valid(j), rd[rv.Slot(j)]);
        WriteBool(out, i, is_and ? And3(ls, rs) : Or3(ls, rs));
    }
}

void EvalCase(ExprNode& n, const Input& in, Vector& out) {
    const idx_t count = in.count;
    const size_t last = n.children.size() - 1; // the ELSE expression
    std::vector<sel_t> remaining(count);
    std::iota(remaining.begin(), remaining.end(), sel_t{0});
    for (size_t w = 0; w + 1 < last + 1 && w < last && !remaining.empty(); w += 2) {
        SelectionVector storage;
        const Input rin = remaining.size() == count
                              ? in
                              : Subset(in, remaining.data(), remaining.size(), storage);
        SelectionVector hits;
        const idx_t nt = SelectBool(*n.children[w], rin, true, hits);
        if (nt == 0)
            continue;
        std::vector<sel_t> rows(nt); // rows (positions within `in`) taking this THEN branch
        for (idx_t j = 0; j < nt; j++)
            rows[j] = remaining[hits[j]];
        SelectionVector then_storage;
        const Input tin = Subset(in, rows.data(), nt, then_storage);
        const Vector& tv = Eval(*n.children[w + 1], tin);
        ScatterRows(out, rows.data(), nt, tv);
        std::vector<sel_t> rest; // remaining minus the rows just handled (both ascending)
        rest.reserve(remaining.size() - nt);
        idx_t h = 0;
        for (idx_t j = 0; j < remaining.size(); j++) {
            if (h < nt && hits[h] == j)
                h++;
            else
                rest.push_back(remaining[j]);
        }
        remaining = std::move(rest);
    }
    if (!remaining.empty()) {
        SelectionVector storage;
        const Input ein = remaining.size() == count
                              ? in
                              : Subset(in, remaining.data(), remaining.size(), storage);
        const Vector& ev = Eval(*n.children[last], ein);
        ScatterRows(out, remaining.data(), remaining.size(), ev);
    }
}

void EvalCoalesce(ExprNode& n, const Input& in, Vector& out) {
    const idx_t count = in.count;
    out.Validity().SetAllInvalid(count); // NULL until some argument supplies a value
    std::vector<sel_t> remaining(count);
    std::iota(remaining.begin(), remaining.end(), sel_t{0});
    for (size_t k = 0; k < n.children.size() && !remaining.empty(); k++) {
        SelectionVector storage;
        const Input rin = remaining.size() == count
                              ? in
                              : Subset(in, remaining.data(), remaining.size(), storage);
        const Vector& v = Eval(*n.children[k], rin);
        ScatterRows(out, remaining.data(), remaining.size(), v); // NULLs stay NULL
        const View vv(v);
        std::vector<sel_t> still;
        for (idx_t j = 0; j < remaining.size(); j++) {
            if (!vv.Valid(j))
                still.push_back(remaining[j]);
        }
        remaining = std::move(still);
    }
}

void EvalOperator(ExprNode& n, const Input& in, Vector& out) {
    const BoundExpr& e = *n.expr;
    const idx_t count = in.count;
    if (e.op == OperatorKind::And || e.op == OperatorKind::Or)
        return EvalAndOr(n, in, out);
    if (e.op == OperatorKind::Not)
        return NotKernel(Eval(*n.children[0], in), out, count);
    if (e.op == OperatorKind::Negate) {
        const Vector& c = Eval(*n.children[0], in);
        switch (c.type().physical()) {
        case PhysicalType::Int32:
            return NegateKernel<int32_t>(c, out, count);
        case PhysicalType::Int64:
            return NegateKernel<int64_t>(c, out, count);
        case PhysicalType::Double:
            return NegateKernel<double>(c, out, count);
        default:
            Internal("negation of a non-numeric type");
        }
    }
    const Vector& l = Eval(*n.children[0], in);
    const Vector& r = Eval(*n.children[1], in);
    if (IsComparison(e.op)) {
        return DispatchType(l.type().physical(), [&](auto tag) {
            CompareExecuteT<decltype(tag)>(e.op, l, r, out, count);
        });
    }
    const bool dates = l.type().id() == TypeId::Date || r.type().id() == TypeId::Date;
    switch (e.op) {
    case OperatorKind::Add:
    case OperatorKind::Sub:
    case OperatorKind::Mul: {
        if (dates)
            return DateArithmeticKernel(e.op, l, r, out, count);
        const PhysicalType t = e.type.physical();
        if (e.op == OperatorKind::Add)
            return ArithmeticByType<AddOp>(t, l, r, out, count);
        if (e.op == OperatorKind::Sub)
            return ArithmeticByType<SubOp>(t, l, r, out, count);
        return ArithmeticByType<MulOp>(t, l, r, out, count);
    }
    case OperatorKind::Div:
        return DivideKernel(l, r, out, count);
    case OperatorKind::Mod:
        switch (e.type.physical()) {
        case PhysicalType::Int32:
            return ModuloKernel<int32_t>(l, r, out, count);
        case PhysicalType::Int64:
            return ModuloKernel<int64_t>(l, r, out, count);
        case PhysicalType::Double:
            return ModuloKernel<double>(l, r, out, count);
        default:
            Internal("modulo of a non-numeric type");
        }
    case OperatorKind::Concat:
        return ConcatKernel(l, r, out, count);
    default:
        Internal("unhandled operator");
    }
}

void EvalFunction(ExprNode& n, const Input& in, Vector& out) {
    const BoundExpr& e = *n.expr;
    const idx_t count = in.count;
    if (e.function == FunctionId::Coalesce)
        return EvalCoalesce(n, in, out);
    if (e.function == FunctionId::Round)
        return FallbackEvaluate(e, in, out);
    std::vector<const Vector*> args;
    for (auto& c : n.children)
        args.push_back(&Eval(*c, in));
    switch (e.function) {
    case FunctionId::Like:
        return LikeKernel(n, *args[0], args[1], out, count);
    case FunctionId::Year:
        return MapDate(*args[0], out, count, [](date_t d) { return Date::Year(d); });
    case FunctionId::Month:
        return MapDate(*args[0], out, count, [](date_t d) { return Date::Month(d); });
    case FunctionId::Day:
        return MapDate(*args[0], out, count, [](date_t d) { return Date::Day(d); });
    case FunctionId::DayOfWeek:
        return MapDate(*args[0], out, count, [](date_t d) { return Date::DayOfWeek(d); });
    case FunctionId::DayOfYear:
        return MapDate(*args[0], out, count, [](date_t d) { return Date::DayOfYear(d); });
    case FunctionId::Quarter:
        return MapDate(*args[0], out, count, [](date_t d) { return (Date::Month(d) - 1) / 3 + 1; });
    case FunctionId::Substring:
        return SubstringKernel(args, out, count);
    case FunctionId::Length: {
        int64_t* od = out.FlatData<int64_t>();
        return MapString(*args[0], out, count, [&](std::string_view s, idx_t i) {
            od[i] = static_cast<int64_t>(Utf8Length(s));
        });
    }
    case FunctionId::Upper:
    case FunctionId::Lower: {
        string_t* od = out.FlatData<string_t>();
        const bool upper = e.function == FunctionId::Upper;
        std::string tmp;
        return MapString(*args[0], out, count, [&](std::string_view s, idx_t i) {
            tmp.assign(s);
            for (char& c : tmp) { // ASCII case mapping only, like the interpreter
                if (upper && c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 32);
                if (!upper && c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + 32);
            }
            od[i] = out.AddString(tmp);
        });
    }
    case FunctionId::Abs:
        return AbsKernel(*args[0], out, count);
    case FunctionId::Floor:
        return MapDouble(*args[0], out, count, [](double v) { return std::floor(v); });
    case FunctionId::Ceil:
        return MapDouble(*args[0], out, count, [](double v) { return std::ceil(v); });
    case FunctionId::NullIf:
        return DispatchType(e.type.physical(), [&](auto tag) {
            NullIfKernel<decltype(tag)>(*args[0], *args[1], out, count);
        });
    case FunctionId::Coalesce:
    case FunctionId::Round:
        break;
    }
    Internal("unhandled function");
}

void EvalInto(ExprNode& n, const Input& in, Vector& out) {
    const BoundExpr& e = *n.expr;
    const idx_t count = in.count;
    CDB_ASSERT(out.type() == e.type && out.capacity() >= count);
    out.Reset();
    if (count == 0)
        return;
    switch (e.kind) {
    case BoundKind::ColumnRef:
    case BoundKind::Constant:
        VectorOps::Copy(Eval(n, in), out, nullptr, count);
        return;
    case BoundKind::Cast:
        CastKernel(Eval(*n.children[0], in), out, count);
        return;
    case BoundKind::Operator:
        EvalOperator(n, in, out);
        return;
    case BoundKind::Function:
        EvalFunction(n, in, out);
        return;
    case BoundKind::Case:
        EvalCase(n, in, out);
        return;
    case BoundKind::InList: {
        const Vector& v = Eval(*n.children[0], in);
        std::vector<const Vector*> items;
        for (size_t k = 1; k < n.children.size(); k++)
            items.push_back(&Eval(*n.children[k], in));
        DispatchType(v.type().physical(),
                     [&](auto tag) { InListKernel<decltype(tag)>(v, items, e.flag, out, count); });
        return;
    }
    case BoundKind::IsNull:
        IsNullKernel(Eval(*n.children[0], in), e.flag, out, count);
        return;
    case BoundKind::Aggregate:
        Internal("aggregates are evaluated by the aggregate operator");
    }
}

const Vector& Eval(ExprNode& n, const Input& in) {
    switch (n.expr->kind) {
    case BoundKind::ColumnRef: {
        const Vector& col = in.chunk->column(n.expr->ordinal);
        if (!in.sel)
            return col; // zero-copy: the chunk's own vector
        n.buffer.Reference(col);
        n.buffer.Slice(*in.sel, in.count); // a dictionary view over the selected rows
        return n.buffer;
    }
    case BoundKind::Constant:
        return n.buffer;
    default:
        EvalInto(n, in, n.buffer);
        return n.buffer;
    }
}

// ====================================================================================
// Selection (predicates evaluated straight into row indices)
// ====================================================================================

idx_t SelectBool(ExprNode& n, const Input& in, bool want, SelectionVector& out) {
    const BoundExpr& e = *n.expr;
    const idx_t count = in.count;
    EnsureCapacity(out, count);
    if (count == 0)
        return 0;
    sel_t* o = out.MutableData();

    if (e.kind == BoundKind::Constant) {
        const Value& v = *e.value;
        if (v.IsNull() || v.GetBoolean() != want)
            return 0;
        for (idx_t i = 0; i < count; i++)
            o[i] = static_cast<sel_t>(i);
        return count;
    }
    if (e.kind == BoundKind::Operator) {
        if (IsComparison(e.op)) {
            const Vector& l = Eval(*n.children[0], in);
            const Vector& r = Eval(*n.children[1], in);
            const OperatorKind op = want ? e.op : Negated(e.op);
            return DispatchType(l.type().physical(), [&](auto tag) {
                return CompareSelectT<decltype(tag)>(op, l, r, count, o);
            });
        }
        if (e.op == OperatorKind::Not)
            return SelectBool(*n.children[0], in, !want, out);
        if (e.op == OperatorKind::And || e.op == OperatorKind::Or) {
            const bool is_and = e.op == OperatorKind::And;
            if (is_and == want) {
                // AND is TRUE / OR is FALSE exactly when both sides are: narrow between them.
                SelectionVector s1;
                const idx_t n1 = SelectBool(*n.children[0], in, want, s1);
                if (n1 == 0)
                    return 0;
                SelectionVector storage;
                const Input in2 = Subset(in, s1.data(), n1, storage);
                SelectionVector s2;
                const idx_t n2 = SelectBool(*n.children[1], in2, want, s2);
                for (idx_t j = 0; j < n2; j++)
                    o[j] = s1[s2[j]];
                return n2;
            }
            // AND is FALSE / OR is TRUE when either side is: the second side only looks at the
            // rest.
            SelectionVector s1;
            const idx_t n1 = SelectBool(*n.children[0], in, want, s1);
            std::vector<sel_t> rest;
            Complement(s1.data(), n1, count, rest);
            if (rest.empty() || n1 == 0) {
                if (n1 == 0 && !rest.empty()) {
                    SelectionVector s2;
                    const idx_t n2 = SelectBool(*n.children[1], in, want, s2);
                    for (idx_t j = 0; j < n2; j++)
                        o[j] = s2[j];
                    return n2;
                }
                for (idx_t j = 0; j < n1; j++)
                    o[j] = s1[j];
                return n1;
            }
            SelectionVector storage;
            const Input in2 = Subset(in, rest.data(), rest.size(), storage);
            SelectionVector s2;
            const idx_t n2 = SelectBool(*n.children[1], in2, want, s2);
            std::vector<sel_t> mapped(n2);
            for (idx_t j = 0; j < n2; j++)
                mapped[j] = rest[s2[j]];
            return MergeSorted(s1.data(), n1, mapped.data(), n2, o);
        }
    }
    if (e.kind == BoundKind::IsNull) {
        const Vector& c = Eval(*n.children[0], in);
        const View cv(c);
        idx_t k = 0;
        for (idx_t i = 0; i < count; i++) {
            const bool is_true = (!cv.Valid(i)) != e.flag; // IS [NOT] NULL is never NULL
            if (is_true == want)
                o[k++] = static_cast<sel_t>(i);
        }
        return k;
    }
    // General case: materialise the BOOLEAN vector and pick the rows equal to `want`.
    const Vector& v = Eval(n, in);
    const View view(v);
    const bool* d = view.data<bool>();
    idx_t k = 0;
    for (idx_t i = 0; i < count; i++) {
        if (view.Valid(i) && d[view.Slot(i)] == want)
            o[k++] = static_cast<sel_t>(i);
    }
    return k;
}

} // namespace

// ====================================================================================
// Public API
// ====================================================================================

ExpressionExecutor::ExpressionExecutor(const BoundExpr& expr)
    : root_(std::make_unique<ExprNode>(expr)) {}
ExpressionExecutor::~ExpressionExecutor() = default;
ExpressionExecutor::ExpressionExecutor(ExpressionExecutor&&) noexcept = default;
ExpressionExecutor& ExpressionExecutor::operator=(ExpressionExecutor&&) noexcept = default;

LogicalType ExpressionExecutor::type() const {
    return root_->expr->type;
}

void ExpressionExecutor::Execute(const DataChunk& input, Vector& result) {
    const BoundExpr& e = *root_->expr;
    CDB_CHECK(result.type() == e.type);
    if (e.kind == BoundKind::ColumnRef) {
        result.Reference(input.column(e.ordinal)); // zero-copy; readers never mutate shared buffers
        return;
    }
    if (e.kind == BoundKind::Constant) {
        result.Reference(root_->buffer);
        return;
    }
    CDB_CHECK(result.capacity() >= input.size());
    EvalInto(*root_, Input{&input, nullptr, input.size()}, result);
}

idx_t ExpressionExecutor::Select(const DataChunk& input, SelectionVector& selection) {
    CDB_CHECK(root_->expr->type.id() == TypeId::Boolean);
    return SelectBool(*root_, Input{&input, nullptr, input.size()}, true, selection);
}

} // namespace cdb
