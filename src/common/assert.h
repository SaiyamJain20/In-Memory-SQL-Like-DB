#pragma once

namespace cdb::detail {

// Prints "<kind> failed: <expr> at <file>:<line>" (plus optional message) and aborts.
[[noreturn]] void AssertionFailed(const char* kind, const char* expr, const char* file, int line,
                                  const char* message) noexcept;

} // namespace cdb::detail

// CDB_CHECK: always on. Use for invariants that guard memory safety or data integrity.
#define CDB_CHECK(cond)                                                                            \
    (static_cast<bool>(cond)                                                                       \
         ? static_cast<void>(0)                                                                    \
         : ::cdb::detail::AssertionFailed("CDB_CHECK", #cond, __FILE__, __LINE__, nullptr))

// CDB_ASSERT: active in Debug and sanitizer builds, compiled out otherwise. Use freely in
// hot paths for cheap sanity checks; never put required side effects inside the condition.
#if defined(CDB_ENABLE_ASSERTS)
#define CDB_ASSERT(cond)                                                                           \
    (static_cast<bool>(cond)                                                                       \
         ? static_cast<void>(0)                                                                    \
         : ::cdb::detail::AssertionFailed("CDB_ASSERT", #cond, __FILE__, __LINE__, nullptr))
#else
#define CDB_ASSERT(cond) static_cast<void>(0)
#endif

// Marks code that must be unreachable; aborts if reached.
#define CDB_UNREACHABLE(msg)                                                                       \
    ::cdb::detail::AssertionFailed("CDB_UNREACHABLE", "", __FILE__, __LINE__, msg)
