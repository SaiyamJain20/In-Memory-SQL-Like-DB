# ADR 0002 — Error handling: exceptions at boundaries, none in hot loops

- **Status:** accepted
- **Date:** 2026-10-06

## Context
An engine has two kinds of failure. *Query errors* (syntax error, unknown column, type mismatch,
division by zero, out of memory) are expected, user-facing, and can occur deep in a call stack
(e.g. inside the binder's recursion or a kernel). *Programming errors* (violated invariants) are
bugs.

## Decision
- Query errors are reported by throwing `cdb::Error` (a `std::runtime_error` subclass carrying an
  `ErrorCode` and, where available, a source position). This keeps parser / binder / planner code
  linear instead of threading a `Result<T>` through every recursive call.
- Inner loops (kernels, hash probes, scans) are `noexcept` and never throw per-row. Conditions that
  can fail per-row (overflow, divide by zero, invalid cast) are detected with branch-free checks
  over the whole vector and raised once, after the loop, with the offending row identified.
- Programming errors use `CDB_ASSERT` (always on in Debug and sanitizer builds, compiled out in
  Release hot paths) and `CDB_CHECK` (always on; for invariants guarding memory safety).
- The public `Connection::Query()` API catches `cdb::Error` and returns a `QueryResult` with an
  error state, so embedding applications never see exceptions cross the API.

## Consequences
- Simple, readable front end; zero overhead on the success path (zero-cost exceptions).
- Exception safety matters: operators hold state in RAII types; tests include error-injection cases
  to verify no leaks (ASan) when a query fails midway.
