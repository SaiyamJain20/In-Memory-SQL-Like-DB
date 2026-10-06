#pragma once

// Helpers for execution-engine tests: random chunks whose columns are flat, constant or
// dictionary vectors (so operators are exercised on every vector format), small-domain value
// generators (so groups and join keys repeat), and NULL-aware tuple ordering for reference
// implementations.

#include "test_util.h"
#include "vector/data_chunk.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace cdb::test {

using ValueGen = std::function<Value(Rng&, LogicalType)>;

// Values from a small domain, so that random rows collide often: a handful of ints, doubles
// including -0.0 / 0.0 / NaN, dates, booleans and strings (some longer than the 12-byte inline
// limit and sharing prefixes).
inline Value SmallDomainValue(Rng& rng, LogicalType type, double null_probability = 0.15) {
    if (Chance(rng, null_probability)) {
        return Value::Null(type);
    }
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(RandBelow(rng, 2) == 1);
    case TypeId::Integer:
        return Value::Integer(static_cast<int32_t>(RandBelow(rng, 6)) - 2);
    case TypeId::BigInt:
        return Value::BigInt(static_cast<int64_t>(RandBelow(rng, 6)) * 1000000007LL - 2);
    case TypeId::Double: {
        static const double kPool[] = {0.0, -0.0, 1.5, -2.25, 100.0, NAN, 7.0};
        return Value::Double(kPool[RandBelow(rng, std::size(kPool))]);
    }
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(RandBelow(rng, 4)) + 10000});
    case TypeId::Varchar: {
        static const char* const kPool[] = {"",
                                            "a",
                                            "ab",
                                            "abcdefghijkl",
                                            "abcdefghijklm",
                                            "abcdefghijklmnopqrstuvwxyz",
                                            "abcdefghijklmnopqrstuvwxyZ",
                                            "zz",
                                            std::string_view("a\0b", 3).data()};
        const size_t k = RandBelow(rng, std::size(kPool));
        return k + 1 == std::size(kPool) ? Value::Varchar(std::string("a\0b", 3))
                                         : Value::Varchar(kPool[k]);
    }
    }
    return Value::Null(type);
}

// A chunk of `count` rows (<= kVectorSize) over `types`; each column independently picks its
// vector format. Row contents come from `gen`.
inline DataChunk RandomVariedChunk(Rng& rng, const std::vector<LogicalType>& types, idx_t count,
                                   const ValueGen& gen) {
    DataChunk chunk;
    chunk.Initialize(types);
    for (idx_t c = 0; c < types.size(); c++) {
        switch (RandBelow(rng, 4)) {
        case 0: { // constant
            Vector v = Vector::MakeConstant(gen(rng, types[c]));
            chunk.column(c).Reference(v);
            break;
        }
        case 1: { // dictionary over a smaller child
            const idx_t child_rows = 1 + RandBelow(rng, std::max<idx_t>(count, 1));
            Vector child(types[c]);
            for (idx_t r = 0; r < child_rows; r++) {
                child.SetValue(r, gen(rng, types[c]));
            }
            SelectionVector sel(std::max<idx_t>(count, 1));
            for (idx_t r = 0; r < count; r++) {
                sel.Set(r, static_cast<sel_t>(RandBelow(rng, child_rows)));
            }
            child.Slice(sel, count);
            chunk.column(c).Reference(child);
            break;
        }
        default: // flat
            for (idx_t r = 0; r < count; r++) {
                chunk.column(c).SetValue(r, gen(rng, types[c]));
            }
            break;
        }
    }
    chunk.SetCardinality(count);
    return chunk;
}

// The logical rows of a chunk, row-major.
inline std::vector<std::vector<Value>> RowsOf(const DataChunk& chunk) {
    std::vector<std::vector<Value>> rows(chunk.size());
    for (idx_t r = 0; r < chunk.size(); r++) {
        for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
            rows[r].push_back(chunk.GetValue(c, r));
        }
    }
    return rows;
}

// Total order over values of one type including NULL (NULL == NULL, NULL after everything).
inline int CompareWithNulls(const Value& a, const Value& b) {
    if (a.IsNull() || b.IsNull()) {
        return a.IsNull() == b.IsNull() ? 0 : (a.IsNull() ? 1 : -1);
    }
    return Value::Compare(a, b);
}

inline int CompareTuples(const std::vector<Value>& a, const std::vector<Value>& b) {
    for (size_t i = 0; i < a.size(); i++) {
        if (const int c = CompareWithNulls(a[i], b[i])) {
            return c;
        }
    }
    return 0;
}

struct TupleLess {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
        return CompareTuples(a, b) < 0;
    }
};

// Same values, bit-for-bit for doubles except NaN == NaN (-0.0 vs 0.0 is a difference).
inline bool SameValue(const Value& a, const Value& b) {
    if (a.type() != b.type() || a.IsNull() != b.IsNull()) {
        return false;
    }
    if (a.IsNull()) {
        return true;
    }
    if (a.type().id() == TypeId::Double) {
        const double x = a.GetDouble(), y = b.GetDouble();
        if (std::isnan(x) || std::isnan(y)) {
            return std::isnan(x) && std::isnan(y);
        }
        return std::memcmp(&x, &y, sizeof x) == 0;
    }
    return a == b;
}

} // namespace cdb::test
