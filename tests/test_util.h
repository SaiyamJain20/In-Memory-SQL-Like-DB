#pragma once

// Shared helpers for tests: deterministic random data generators for every type. Strings and
// doubles are deliberately nasty (embedded NULs, bytes >= 0x80, shared prefixes, NaN, -0.0, inf)
// so property tests exercise the edge cases that hand-written examples miss.

#include "types/value.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace cdb::test {

using Rng = std::mt19937_64;

inline const std::vector<LogicalType>& AllTypes() {
    static const std::vector<LogicalType> kTypes = {LogicalType::Boolean(), LogicalType::Integer(),
                                                    LogicalType::BigInt(),  LogicalType::Double(),
                                                    LogicalType::Date(),    LogicalType::Varchar()};
    return kTypes;
}

inline uint64_t RandBelow(Rng& rng, uint64_t n) {
    return std::uniform_int_distribution<uint64_t>(0, n - 1)(rng);
}

inline bool Chance(Rng& rng, double p) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng) < p;
}

// Strings drawn from a tiny alphabet (so long common prefixes are likely) with lengths that
// straddle the 4-byte-prefix and 12-byte-inline boundaries, plus bytes that break signed-char
// comparisons and embedded NULs.
inline std::string RandomString(Rng& rng) {
    static const char kAlphabet[] = {'a', 'b', 'c', '\0', '\x01', '\x7f', '\x80', '\xff'};
    size_t len;
    switch (RandBelow(rng, 6)) {
    case 0:
        len = RandBelow(rng, 5);
        break; // 0..4
    case 1:
        len = 4 + RandBelow(rng, 10);
        break; // 4..13 (inline boundary)
    case 2:
        len = 11 + RandBelow(rng, 4);
        break; // 11..14
    case 3:
        len = 13 + RandBelow(rng, 40);
        break; // out-of-line
    case 4:
        len = 100 + RandBelow(rng, 300);
        break;
    default:
        len = RandBelow(rng, 20);
        break;
    }
    std::string s(len, 'a');
    for (char& c : s) {
        // Few distinct letters => frequent equal prefixes between independently drawn strings.
        c = kAlphabet[RandBelow(rng, Chance(rng, 0.7) ? 3 : sizeof(kAlphabet))];
    }
    return s;
}

inline double RandomDouble(Rng& rng) {
    switch (RandBelow(rng, 10)) {
    case 0:
        return 0.0;
    case 1:
        return -0.0;
    case 2:
        return std::numeric_limits<double>::infinity();
    case 3:
        return -std::numeric_limits<double>::infinity();
    case 4:
        return std::numeric_limits<double>::quiet_NaN();
    case 5:
        return std::numeric_limits<double>::denorm_min();
    case 6:
        return static_cast<double>(static_cast<int64_t>(RandBelow(rng, 2000)) - 1000);
    default:
        return std::uniform_real_distribution<double>(-1e9, 1e9)(rng);
    }
}

inline Value RandomValue(Rng& rng, LogicalType type, double null_probability = 0.2) {
    if (Chance(rng, null_probability)) {
        return Value::Null(type);
    }
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(RandBelow(rng, 2) == 1);
    case TypeId::Integer:
        return Value::Integer(Chance(rng, 0.1)
                                  ? (Chance(rng, 0.5) ? std::numeric_limits<int32_t>::min()
                                                      : std::numeric_limits<int32_t>::max())
                                  : static_cast<int32_t>(rng()));
    case TypeId::BigInt:
        return Value::BigInt(Chance(rng, 0.1)
                                 ? (Chance(rng, 0.5) ? std::numeric_limits<int64_t>::min()
                                                     : std::numeric_limits<int64_t>::max())
                                 : static_cast<int64_t>(rng()));
    case TypeId::Double:
        return Value::Double(RandomDouble(rng));
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(RandBelow(rng, 3000000)) - 719162});
    case TypeId::Varchar:
        return Value::Varchar(RandomString(rng));
    }
    return Value::Null(type);
}

// Bitwise comparison for values that may be NaN / signed zero: stricter than Value::operator==
// (which treats -0.0 == 0.0). Used where a vector must round-trip exact bits.
inline bool BitIdentical(const Value& a, const Value& b) {
    if (a.type() != b.type() || a.IsNull() != b.IsNull()) {
        return false;
    }
    if (a.IsNull()) {
        return true;
    }
    if (a.type().id() == TypeId::Double) {
        const double x = a.GetDouble(), y = b.GetDouble();
        return std::memcmp(&x, &y, sizeof(double)) == 0;
    }
    return a == b;
}

// An EXPLAIN line ends with its row estimate, `  (~123 rows)`. Returns true if `line` does, and
// stores the line without it in `*shape` (the plan's shape, which the planner tests compare).
// (Written by hand: std::regex trips GCC 13's -Wmaybe-uninitialized under -O2 -Werror.)
inline bool StripEstimate(const std::string& line, std::string* shape) {
    const std::string open = "  (~", close = " rows)";
    if (line.size() < open.size() + close.size() + 1 ||
        line.compare(line.size() - close.size(), close.size(), close) != 0) {
        return false;
    }
    const size_t at = line.rfind(open);
    if (at == std::string::npos) {
        return false;
    }
    const size_t from = at + open.size(), to = line.size() - close.size();
    if (from >= to) {
        return false; // no digits
    }
    for (size_t i = from; i < to; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return false;
        }
    }
    if (shape != nullptr) {
        *shape = line.substr(0, at);
    }
    return true;
}

} // namespace cdb::test
