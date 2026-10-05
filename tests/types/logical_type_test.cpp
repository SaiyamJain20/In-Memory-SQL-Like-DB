#include "types/logical_type.h"

#include "test_util.h"

#include <gtest/gtest.h>

namespace cdb {

TEST(LogicalType, PhysicalMapping) {
    EXPECT_EQ(LogicalType::Boolean().physical(), PhysicalType::Bool);
    EXPECT_EQ(LogicalType::Integer().physical(), PhysicalType::Int32);
    EXPECT_EQ(LogicalType::BigInt().physical(), PhysicalType::Int64);
    EXPECT_EQ(LogicalType::Double().physical(), PhysicalType::Double);
    EXPECT_EQ(LogicalType::Date().physical(), PhysicalType::Int32); // shares INTEGER's kernels
    EXPECT_EQ(LogicalType::Varchar().physical(), PhysicalType::String);
}

TEST(LogicalType, Widths) {
    EXPECT_EQ(LogicalType::Boolean().width(), 1u);
    EXPECT_EQ(LogicalType::Integer().width(), 4u);
    EXPECT_EQ(LogicalType::BigInt().width(), 8u);
    EXPECT_EQ(LogicalType::Double().width(), 8u);
    EXPECT_EQ(LogicalType::Date().width(), 4u);
    EXPECT_EQ(LogicalType::Varchar().width(), 16u);
}

TEST(LogicalType, Classification) {
    EXPECT_TRUE(LogicalType::Integer().IsNumeric());
    EXPECT_TRUE(LogicalType::BigInt().IsNumeric());
    EXPECT_TRUE(LogicalType::Double().IsNumeric());
    EXPECT_FALSE(LogicalType::Boolean().IsNumeric());
    EXPECT_FALSE(LogicalType::Date().IsNumeric());
    EXPECT_FALSE(LogicalType::Varchar().IsNumeric());

    EXPECT_TRUE(LogicalType::Integer().IsIntegral());
    EXPECT_TRUE(LogicalType::BigInt().IsIntegral());
    EXPECT_FALSE(LogicalType::Double().IsIntegral());

    EXPECT_TRUE(LogicalType::Date().IsFixedWidth());
    EXPECT_FALSE(LogicalType::Varchar().IsFixedWidth());
}

TEST(LogicalType, NamesRoundTripThroughFromName) {
    for (LogicalType t : test::AllTypes()) {
        auto parsed = LogicalType::FromName(t.ToString());
        ASSERT_TRUE(parsed.has_value()) << t.ToString();
        EXPECT_EQ(*parsed, t);
    }
}

TEST(LogicalType, FromNameIsCaseInsensitiveAndAcceptsAliases) {
    EXPECT_EQ(LogicalType::FromName("int"), LogicalType::Integer());
    EXPECT_EQ(LogicalType::FromName("Int4"), LogicalType::Integer());
    EXPECT_EQ(LogicalType::FromName("bigint"), LogicalType::BigInt());
    EXPECT_EQ(LogicalType::FromName("INT8"), LogicalType::BigInt());
    EXPECT_EQ(LogicalType::FromName("bool"), LogicalType::Boolean());
    EXPECT_EQ(LogicalType::FromName("float8"), LogicalType::Double());
    EXPECT_EQ(LogicalType::FromName("text"), LogicalType::Varchar());
    EXPECT_EQ(LogicalType::FromName("String"), LogicalType::Varchar());
}

TEST(LogicalType, FromNameRejectsUnknown) {
    for (const char* bad : {"", "INTEGER ", " INT", "DECIMAL", "TIMESTAMP", "blob", "int2x"}) {
        EXPECT_FALSE(LogicalType::FromName(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(LogicalType, EqualityIsByTypeId) {
    EXPECT_EQ(LogicalType::Integer(), LogicalType(TypeId::Integer));
    EXPECT_NE(LogicalType::Integer(), LogicalType::BigInt());
    // DATE and INTEGER share a physical type but are distinct logical types.
    EXPECT_NE(LogicalType::Date(), LogicalType::Integer());
}

TEST(LogicalType, PhysicalTypeOfMatchesElementTypes) {
    static_assert(PhysicalTypeOf<bool>::value == PhysicalType::Bool);
    static_assert(PhysicalTypeOf<int32_t>::value == PhysicalType::Int32);
    static_assert(PhysicalTypeOf<int64_t>::value == PhysicalType::Int64);
    static_assert(PhysicalTypeOf<double>::value == PhysicalType::Double);
    static_assert(PhysicalTypeOf<string_t>::value == PhysicalType::String);
    for (LogicalType t : test::AllTypes()) {
        EXPECT_GT(PhysicalTypeSize(t.physical()), 0u);
        EXPECT_NE(std::string(PhysicalTypeName(t.physical())), "");
    }
}

} // namespace cdb
