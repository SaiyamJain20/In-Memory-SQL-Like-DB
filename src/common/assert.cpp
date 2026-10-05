#include "common/assert.h"

#include <cstdio>
#include <cstdlib>

namespace cdb::detail {

void AssertionFailed(const char* kind, const char* expr, const char* file, int line,
                     const char* message) noexcept {
    std::fprintf(stderr, "%s failed: %s at %s:%d%s%s\n", kind, expr, file, line,
                 message != nullptr ? " - " : "", message != nullptr ? message : "");
    std::fflush(stderr);
    std::abort();
}

} // namespace cdb::detail
