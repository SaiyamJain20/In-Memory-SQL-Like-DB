#include "common/assert.h"
#include "common/error.h"
#include "common/version.h"

#include <gtest/gtest.h>

#include <string>

namespace cdb {

TEST(Error, CarriesCodeAndMessage) {
    Error e(ErrorCode::Binder, "column \"x\" not found");
    EXPECT_EQ(e.code(), ErrorCode::Binder);
    EXPECT_FALSE(e.position().has_value());
    EXPECT_EQ(std::string(e.what()), "Binder Error: column \"x\" not found");
}

TEST(Error, CarriesSourcePosition) {
    Error e(ErrorCode::Syntax, "unexpected token", 17);
    ASSERT_TRUE(e.position().has_value());
    EXPECT_EQ(*e.position(), 17u);
}

TEST(Error, IsCatchableAsStdException) {
    try {
        throw Error(ErrorCode::Execution, "division by zero");
    } catch (const std::exception& ex) {
        EXPECT_NE(std::string(ex.what()).find("division by zero"), std::string::npos);
        return;
    }
    FAIL() << "exception was not caught";
}

TEST(Error, EveryCodeHasADistinctName) {
    const ErrorCode all[] = {ErrorCode::Internal, ErrorCode::Syntax,        ErrorCode::Binder,
                             ErrorCode::Catalog,  ErrorCode::Type,          ErrorCode::Execution,
                             ErrorCode::Io,       ErrorCode::Corruption,
                             ErrorCode::NotImplemented};
    for (size_t i = 0; i < std::size(all); i++) {
        for (size_t j = i + 1; j < std::size(all); j++) {
            EXPECT_STRNE(ErrorCodeName(all[i]), ErrorCodeName(all[j]));
        }
    }
}

TEST(Assert, CheckPassesSilently) {
    CDB_CHECK(1 + 1 == 2);
    CDB_ASSERT(true);
}

TEST(AssertDeathTest, CheckAbortsOnFailure) {
    EXPECT_DEATH(CDB_CHECK(1 + 1 == 3), "CDB_CHECK failed: 1 \\+ 1 == 3");
}

#if defined(CDB_ENABLE_ASSERTS)
TEST(AssertDeathTest, AssertAbortsWhenEnabled) {
    EXPECT_DEATH(CDB_ASSERT(false), "CDB_ASSERT failed");
}
#endif

TEST(Version, IsSet) {
    EXPECT_STRNE(kVersion, "unknown");
}

} // namespace cdb
