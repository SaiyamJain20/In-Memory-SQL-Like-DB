#include "catalog/catalog.h"

#include "common/error.h"
#include "main/database.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace cdb {

namespace {

std::vector<ColumnDefinition> OneColumn() {
    return {{"x", LogicalType::Integer()}};
}

ErrorCode CodeOf(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const Error& e) {
        return e.code();
    }
    return ErrorCode::Internal; // sentinel: nothing was thrown
}

} // namespace

TEST(Catalog, CreateAndGet) {
    Catalog c;
    auto t = c.CreateTable("Lineitem", OneColumn());
    EXPECT_EQ(t->name(), "Lineitem");
    EXPECT_EQ(c.GetTable("Lineitem").get(), t.get());
    EXPECT_EQ(c.TryGetTable("Lineitem").get(), t.get());
}

TEST(Catalog, NamesAreCaseInsensitiveButPreserveDisplayCase) {
    Catalog c;
    auto t = c.CreateTable("MyTable", OneColumn());
    EXPECT_EQ(c.GetTable("mytable").get(), t.get());
    EXPECT_EQ(c.GetTable("MYTABLE").get(), t.get());
    EXPECT_EQ(c.ListTables(), std::vector<std::string>{"MyTable"});
    EXPECT_EQ(CodeOf([&] { c.CreateTable("MYTABLE", OneColumn()); }), ErrorCode::Catalog);
}

TEST(Catalog, DuplicateCreateFailsUnlessIfNotExists) {
    Catalog c;
    auto t = c.CreateTable("t", OneColumn());
    EXPECT_EQ(CodeOf([&] { c.CreateTable("t", OneColumn()); }), ErrorCode::Catalog);
    auto again =
        c.CreateTable("t", {{"different", LogicalType::Varchar()}}, /*if_not_exists=*/true);
    EXPECT_EQ(again.get(), t.get()); // the existing table wins, schema untouched
    EXPECT_EQ(again->schema()[0].name, "x");
}

TEST(Catalog, MissingTableErrors) {
    Catalog c;
    EXPECT_EQ(CodeOf([&] { c.GetTable("nope"); }), ErrorCode::Catalog);
    EXPECT_EQ(c.TryGetTable("nope"), nullptr);
    EXPECT_EQ(CodeOf([&] { c.DropTable("nope"); }), ErrorCode::Catalog);
    c.DropTable("nope", /*if_exists=*/true); // no error
}

TEST(Catalog, ErrorMessagesNameTheTable) {
    Catalog c;
    try {
        c.GetTable("Orders");
        FAIL() << "expected an error";
    } catch (const Error& e) {
        EXPECT_NE(std::string(e.what()).find("Orders"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("does not exist"), std::string::npos);
    }
}

TEST(Catalog, DropRemovesTheTableButLiveSnapshotsKeepWorking) {
    Catalog c;
    auto t = c.CreateTable("t", OneColumn(), false, kVectorSize);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (idx_t i = 0; i < 10; i++)
        chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(i)));
    chunk.SetCardinality(10);
    t->Append(chunk);
    auto snap = t->Snapshot();

    c.DropTable("t");
    EXPECT_EQ(c.TryGetTable("t"), nullptr);
    EXPECT_TRUE(c.ListTables().empty());

    TableScan scan(snap, {0});
    DataChunk out;
    out.Initialize(scan.types());
    ASSERT_TRUE(scan.Next(out));
    EXPECT_EQ(out.size(), 10u);
    EXPECT_EQ(out.GetValue(0, 9).GetInteger(), 9);

    // and the name is free again
    auto t2 = c.CreateTable("t", OneColumn());
    EXPECT_NE(t2.get(), t.get());
    EXPECT_EQ(t2->RowCount(), 0u);
}

TEST(Catalog, ListTablesIsSortedCaseInsensitively) {
    Catalog c;
    for (const char* n : {"orders", "Customer", "lineitem", "Part", "AAA"}) {
        c.CreateTable(n, OneColumn());
    }
    EXPECT_EQ(c.ListTables(),
              (std::vector<std::string>{"AAA", "Customer", "lineitem", "orders", "Part"}));
}

TEST(Catalog, InvalidInputsAreRejected) {
    Catalog c;
    EXPECT_EQ(CodeOf([&] { c.CreateTable("", OneColumn()); }), ErrorCode::Catalog);
    EXPECT_EQ(CodeOf([&] { c.CreateTable("t", {}); }), ErrorCode::Binder);
    EXPECT_EQ(CodeOf([&] {
                  c.CreateTable("t",
                                {{"a", LogicalType::Integer()}, {"A", LogicalType::Integer()}});
              }),
              ErrorCode::Binder);
    // a failed create must not leave a half-registered table behind
    EXPECT_EQ(c.TryGetTable("t"), nullptr);
    EXPECT_TRUE(c.ListTables().empty());
}

TEST(Catalog, ConcurrentCreateOfTheSameNameHasExactlyOneWinner) {
    for (int round = 0; round < 20; round++) {
        Catalog c;
        std::atomic<int> winners{0}, losers{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; i++) {
            threads.emplace_back([&] {
                try {
                    c.CreateTable("shared", OneColumn());
                    winners++;
                } catch (const Error& e) {
                    if (e.code() == ErrorCode::Catalog)
                        losers++;
                }
            });
        }
        for (auto& t : threads)
            t.join();
        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(losers.load(), 7);
        EXPECT_EQ(c.ListTables().size(), 1u);
    }
}

TEST(Catalog, ConcurrentCreatesOfDistinctNamesAllSucceed) {
    Catalog c;
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < 25; j++) {
                c.CreateTable("t_" + std::to_string(i) + "_" + std::to_string(j), OneColumn());
                c.ListTables();
                c.TryGetTable("t_0_0");
            }
        });
    }
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(c.ListTables().size(), 200u);
}

TEST(Database, OwnsACatalog) {
    Database db;
    db.catalog().CreateTable("t", OneColumn());
    const Database& cdb = db;
    EXPECT_EQ(cdb.catalog().ListTables().size(), 1u);
}

} // namespace cdb
