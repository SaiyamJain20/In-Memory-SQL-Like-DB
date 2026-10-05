#pragma once

#include "common/error.h"
#include "parser/ast.h"

#include <string>
#include <string_view>
#include <vector>

namespace cdb {

// Limits that keep hostile input (fuzzers, generated SQL) from exhausting the stack.
inline constexpr uint32_t kMaxExprDepth = 1000;   // height of any expression tree
inline constexpr uint32_t kMaxParseNesting = 400; // grammar recursion depth (a parenthesis costs 2)

// Parses zero or more `;`-separated statements. Throws Error(Syntax) with a byte position for
// malformed input and Error(NotImplemented) for valid SQL this engine does not support yet
// (UNION, WITH, PRIMARY KEY, ...).
std::vector<StatementPtr> ParseStatements(std::string_view sql);

// Parses exactly one statement (an optional trailing `;` is allowed).
StatementPtr ParseStatement(std::string_view sql);

// Parses a standalone expression (used by tests and tools).
ExprPtr ParseExpression(std::string_view sql);

// Renders an Error that carries a position with the offending line and a caret:
//
//   Syntax Error: syntax error at or near "foo" (expected FROM)
//   LINE 1: SELECT * foo
//                    ^
// Errors without a position are returned as plain `what()`.
std::string FormatErrorWithContext(std::string_view sql, const Error& error);

} // namespace cdb
