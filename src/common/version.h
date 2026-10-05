#pragma once

namespace cdb {

#ifdef CDB_VERSION
inline constexpr const char* kVersion = CDB_VERSION;
#else
inline constexpr const char* kVersion = "unknown";
#endif

} // namespace cdb
