#include "storage/column_stats.h"

#include "common/assert.h"
#include "types/string_t.h"

#include <cmath>

namespace cdb {

const char* CompareOpName(CompareOp op) noexcept {
    switch (op) {
    case CompareOp::Eq:
        return "=";
    case CompareOp::Ne:
        return "<>";
    case CompareOp::Lt:
        return "<";
    case CompareOp::Le:
        return "<=";
    case CompareOp::Gt:
        return ">";
    case CompareOp::Ge:
        return ">=";
    }
    CDB_UNREACHABLE("CompareOpName");
}

bool ColumnStats::CanSkip(CompareOp op, const Value& constant) const {
    if (AllNull() || constant.IsNull()) {
        return true; // NULL compared with anything is never true
    }
    if (!min.has_value()) {
        return false; // no usable bounds
    }
    CDB_CHECK(constant.type() == min->type());
    const int vs_min = Value::Compare(constant, *min); // sign of (constant - min)
    const int vs_max = Value::Compare(constant, *max);
    switch (op) {
    case CompareOp::Eq:
        return vs_min < 0 || vs_max > 0;
    case CompareOp::Ne:
        return vs_min == 0 && vs_max == 0; // every non-NULL value equals c
    case CompareOp::Lt:
        return vs_min <= 0; // min >= c: nothing is < c
    case CompareOp::Le:
        return vs_min < 0; // min >  c
    case CompareOp::Gt:
        return vs_max >= 0; // max <= c
    case CompareOp::Ge:
        return vs_max > 0; // max <  c
    }
    CDB_UNREACHABLE("ColumnStats::CanSkip");
}

namespace {

// Strict "a sorts before b" matching Value::Compare for each element type.
template <class T> struct Less {
    bool operator()(T a, T b) const { return a < b; }
};
template <> struct Less<double> {
    bool operator()(double a, double b) const {
        if (std::isnan(a))
            return false; // NaN sorts last
        if (std::isnan(b))
            return true;
        return a < b;
    }
};
template <> struct Less<string_t> {
    bool operator()(const string_t& a, const string_t& b) const {
        return string_t::Compare(a, b) < 0;
    }
};

template <class T>
bool MinMax(const T* v, const ValidityMask& validity, idx_t count, T& mn, T& mx) {
    Less<T> less;
    bool any = false;
    for (idx_t i = 0; i < count; i++) {
        if (!validity.IsValid(i)) {
            continue;
        }
        if (!any) {
            mn = mx = v[i];
            any = true;
        } else {
            if (less(v[i], mn))
                mn = v[i];
            if (less(mx, v[i]))
                mx = v[i];
        }
    }
    return any;
}

} // namespace

ColumnStats ComputeColumnStats(LogicalType type, const uint8_t* data, const ValidityMask& validity,
                               idx_t count) {
    ColumnStats st;
    st.count = count;
    st.null_count = count - validity.CountValid(count);
    if (st.AllNull()) {
        return st;
    }
    switch (type.id()) {
    case TypeId::Boolean: {
        bool mn = false, mx = false;
        if (MinMax(reinterpret_cast<const bool*>(data), validity, count, mn, mx)) {
            st.min = Value::Boolean(mn);
            st.max = Value::Boolean(mx);
        }
        break;
    }
    case TypeId::Integer: {
        int32_t mn = 0, mx = 0;
        if (MinMax(reinterpret_cast<const int32_t*>(data), validity, count, mn, mx)) {
            st.min = Value::Integer(mn);
            st.max = Value::Integer(mx);
        }
        break;
    }
    case TypeId::Date: {
        int32_t mn = 0, mx = 0;
        if (MinMax(reinterpret_cast<const int32_t*>(data), validity, count, mn, mx)) {
            st.min = Value::Date(date_t{mn});
            st.max = Value::Date(date_t{mx});
        }
        break;
    }
    case TypeId::BigInt: {
        int64_t mn = 0, mx = 0;
        if (MinMax(reinterpret_cast<const int64_t*>(data), validity, count, mn, mx)) {
            st.min = Value::BigInt(mn);
            st.max = Value::BigInt(mx);
        }
        break;
    }
    case TypeId::Double: {
        double mn = 0, mx = 0;
        if (MinMax(reinterpret_cast<const double*>(data), validity, count, mn, mx)) {
            st.min = Value::Double(mn);
            st.max = Value::Double(mx);
        }
        break;
    }
    case TypeId::Varchar: {
        const auto* strs = reinterpret_cast<const string_t*>(data);
        for (idx_t i = 0; i < count; i++) {
            if (validity.IsValid(i) && strs[i].size() > ColumnStats::kMaxBoundStringLength) {
                return st; // bounds unavailable; segment is never pruned
            }
        }
        string_t mn, mx;
        if (MinMax(strs, validity, count, mn, mx)) {
            st.min = Value::Varchar(std::string(mn.view()));
            st.max = Value::Varchar(std::string(mx.view()));
        }
        break;
    }
    }
    return st;
}

} // namespace cdb
