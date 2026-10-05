#pragma once

#include "types/value.h"

#include <optional>

namespace cdb {

// Which conversions are applied silently (e.g. to make `int_col < double_col` well typed).
// Only numeric widening INTEGER -> BIGINT -> DOUBLE (and identity).
bool CanCastImplicitly(LogicalType from, LogicalType to) noexcept;

// Which conversions CAST(x AS t) accepts at all:
//   BOOLEAN -> INTEGER BIGINT VARCHAR            INTEGER -> BOOLEAN BIGINT DOUBLE VARCHAR
//   BIGINT  -> BOOLEAN INTEGER DOUBLE VARCHAR    DOUBLE  -> BOOLEAN INTEGER BIGINT VARCHAR
//   DATE    -> VARCHAR                           VARCHAR -> everything
bool CanCastExplicitly(LogicalType from, LogicalType to) noexcept;

// The smallest type both operands can be implicitly widened to (numeric promotion), or nullopt.
std::optional<LogicalType> CommonSuperType(LogicalType a, LogicalType b) noexcept;

// CAST(v AS target). NULL stays NULL (of the target type). Matches DuckDB's behaviour:
//   * DOUBLE -> integer rounds half away from zero; NaN, infinity and out-of-range values are
//   errors
//   * VARCHAR -> number trims whitespace; '1.5'::INTEGER is 2; '', 'abc', '1e999'::DOUBLE are
//   errors
//   * VARCHAR -> BOOLEAN accepts true/t/yes/1 and false/f/no/0 (case-insensitive)
//   * numbers -> BOOLEAN is `<> 0`
// Throws Error(Type) for impossible casts and failed conversions.
Value CastValue(const Value& v, LogicalType target);

// DATE + interval, with SQL month semantics: adding months clamps the day to the end of the
// target month (2020-01-31 + 1 month = 2020-02-29). `unit` is year, month, week or day; other
// units throw Error(NotImplemented) since DATE has no time-of-day.
date_t AddInterval(date_t date, int64_t amount, const std::string& unit);

} // namespace cdb
