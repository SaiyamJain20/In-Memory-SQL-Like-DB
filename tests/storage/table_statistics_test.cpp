#include "storage/table_statistics.h"

#include "storage_test_util.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <set>
#include <thread>

namespace cdb {

namespace {

constexpr idx_t kGroup = 2 * kVectorSize;

const std::vector<ColumnDefinition> kSchema = {
    {"id", LogicalType::BigInt()}, {"m", LogicalType::Integer()}, {"s", LogicalType::Varchar()},
    {"x", LogicalType::Double()},  {"d", LogicalType::Date()},    {"b", LogicalType::Boolean()},
    {"n", LogicalType::Integer()},
};

// id: 0..rows-1 (all distinct); m: id % 7; s: "s" + id % 100; x: id / 4.0 with every fifth NULL;
// d: one constant date; b: id % 2; n: always NULL.
void Fill(Table& table, idx_t first, idx_t rows) {
    DataChunk chunk;
    std::vector<LogicalType> types;
    for (const ColumnDefinition& c : kSchema) {
        types.push_back(c.type);
    }
    chunk.Initialize(types);
    for (idx_t at = 0; at < rows; at += kVectorSize) {
        chunk.Reset();
        const idx_t n = std::min<idx_t>(kVectorSize, rows - at);
        for (idx_t i = 0; i < n; i++) {
            const int64_t id = static_cast<int64_t>(first + at + i);
            chunk.SetValue(0, i, Value::BigInt(id));
            chunk.SetValue(1, i, Value::Integer(static_cast<int32_t>(id % 7)));
            chunk.SetValue(2, i, Value::Varchar("s" + std::to_string(id % 100)));
            chunk.SetValue(3, i,
                           id % 5 == 0 ? Value::Null(LogicalType::Double())
                                       : Value::Double(static_cast<double>(id) / 4.0));
            chunk.SetValue(4, i, Value::Date(date_t{19000}));
            chunk.SetValue(5, i, Value::Boolean(id % 2 == 1));
            chunk.SetValue(6, i, Value::Null(LogicalType::Integer()));
        }
        chunk.SetCardinality(n);
        table.Append(chunk);
    }
}

void ExpectNear(double estimate, double truth, double relative, const std::string& what) {
    EXPECT_NEAR(estimate, truth, std::max(0.5, relative * truth)) << what;
}

} // namespace

TEST(TableStatistics, AnEmptyTableHasNoRowsNoBoundsAndNoValues) {
    Table table("t", kSchema, kGroup);
    const auto stats = table.Statistics();
    EXPECT_EQ(stats->row_count, 0U);
    ASSERT_EQ(stats->columns.size(), kSchema.size());
    for (const ColumnStatistics& c : stats->columns) {
        EXPECT_EQ(c.null_count, 0U);
        EXPECT_FALSE(c.min.has_value());
        EXPECT_EQ(c.distinct, 0.0);
    }
    EXPECT_EQ(stats->NullFraction(0), 0.0);
}

TEST(TableStatistics, SumsBoundsAndDistinctCountsOverRowGroupsAndTheOpenTail) {
    for (const bool compression : {true, false}) {
        const test::ScopedCompression mode(compression);
        // 3 full row groups and an open tail of 1,234 rows
        const idx_t rows = 3 * kGroup + 1234;
        Table table("t", kSchema, kGroup);
        Fill(table, 0, rows);
        const auto stats = table.Statistics();
        ASSERT_EQ(stats->row_count, rows);

        const ColumnStatistics& id = stats->columns[0];
        EXPECT_EQ(id.null_count, 0U);
        ASSERT_TRUE(id.min && id.max);
        EXPECT_EQ(id.min->GetBigInt(), 0);
        EXPECT_EQ(id.max->GetBigInt(), static_cast<int64_t>(rows) - 1) << "the tail is counted";
        ExpectNear(id.distinct, static_cast<double>(rows), 0.05, "id");

        EXPECT_EQ(stats->columns[1].min->GetInteger(), 0);
        EXPECT_EQ(stats->columns[1].max->GetInteger(), 6);
        ExpectNear(stats->columns[1].distinct, 7, 0.0, "m");

        EXPECT_EQ(stats->columns[2].min->GetVarchar(), "s0");
        EXPECT_EQ(stats->columns[2].max->GetVarchar(), "s99");
        ExpectNear(stats->columns[2].distinct, 100, 0.03, "s");

        // every fifth id is NULL in x
        idx_t x_nulls = 0;
        for (idx_t i = 0; i < rows; i++) {
            x_nulls += i % 5 == 0;
        }
        EXPECT_EQ(stats->columns[3].null_count, x_nulls);
        EXPECT_NEAR(stats->NullFraction(3),
                    static_cast<double>(x_nulls) / static_cast<double>(rows), 1e-12);
        EXPECT_EQ(stats->columns[3].min->GetDouble(), 0.25);
        ExpectNear(stats->columns[3].distinct, static_cast<double>(rows - x_nulls), 0.05, "x");

        ExpectNear(stats->columns[4].distinct, 1, 0.0, "constant date");
        EXPECT_EQ(Value::Compare(*stats->columns[4].min, *stats->columns[4].max), 0);
        ExpectNear(stats->columns[5].distinct, 2, 0.0, "b");

        // a column of NULLs: counted as such, no bounds, no distinct values
        const ColumnStatistics& n = stats->columns[6];
        EXPECT_EQ(n.null_count, rows);
        EXPECT_FALSE(n.min.has_value());
        EXPECT_FALSE(n.max.has_value());
        EXPECT_EQ(n.distinct, 0.0);
        EXPECT_EQ(stats->NullFraction(6), 1.0);
    }
}

TEST(TableStatistics, TheDistinctCountIsNeverAboveTheNumberOfValues) {
    Table table("t", kSchema, kGroup);
    Fill(table, 0, 3);
    const auto stats = table.Statistics();
    for (idx_t c = 0; c < 6; c++) {
        EXPECT_LE(stats->columns[c].distinct,
                  static_cast<double>(stats->row_count - stats->columns[c].null_count))
            << kSchema[c].name;
        EXPECT_GE(stats->columns[c].distinct, 1.0) << kSchema[c].name;
    }
    EXPECT_NEAR(stats->columns[0].distinct, 3.0, 0.01);
}

TEST(TableStatistics, ALongStringTakesTheBoundsAwayButNotTheDistinctCount) {
    Table table("t", {{"s", LogicalType::Varchar()}}, kGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Varchar()});
    chunk.SetValue(0, 0, Value::Varchar("a"));
    chunk.SetValue(0, 1, Value::Varchar(std::string(100, 'b')));
    chunk.SetValue(0, 2, Value::Varchar("c"));
    chunk.SetCardinality(3);
    table.Append(chunk);
    // more row groups of ordinary strings do not bring the bounds back
    chunk.Reset();
    for (idx_t i = 0; i < kVectorSize; i++) {
        chunk.SetValue(0, i, Value::Varchar("s" + std::to_string(i % 10)));
    }
    chunk.SetCardinality(kVectorSize);
    for (int k = 0; k < 4; k++) {
        table.Append(chunk);
    }
    ASSERT_GE(table.Snapshot()->row_group_count(), 2U);
    const auto stats = table.Statistics();
    EXPECT_FALSE(stats->columns[0].min.has_value());
    EXPECT_FALSE(stats->columns[0].max.has_value());
    EXPECT_NEAR(stats->columns[0].distinct, 13.0, 0.5) << "a, c, the long one and s0 ... s9";
}

TEST(TableStatistics, AreComputedOnceAndRecomputedAfterEveryKindOfChange) {
    Table table("t", kSchema, kGroup);
    Fill(table, 0, 500);
    const auto first = table.Statistics();
    EXPECT_EQ(table.Statistics().get(), first.get()) << "shared while nothing changes";
    EXPECT_EQ(first->row_count, 500U);

    Fill(table, 500, 500); // Append
    const auto after_append = table.Statistics();
    EXPECT_NE(after_append.get(), first.get());
    EXPECT_EQ(after_append->row_count, 1000U);
    EXPECT_EQ(first->row_count, 500U) << "an old result is not modified";

    auto staging = std::make_unique<Table>("s", kSchema, kGroup); // Merge of a small load
    Fill(*staging, 1000, 10);
    table.Merge(std::move(staging));
    EXPECT_EQ(table.Statistics()->row_count, 1010U);

    staging = std::make_unique<Table>("s", kSchema, kGroup); // Merge of a bulk load
    Fill(*staging, 1010, 3 * kGroup);
    table.Merge(std::move(staging));
    EXPECT_EQ(table.Statistics()->row_count, 1010U + 3 * kGroup);
    EXPECT_EQ(table.Statistics()->columns[0].max->GetBigInt(),
              static_cast<int64_t>(1010 + 3 * kGroup) - 1);

    Table other("o", kSchema, kGroup); // AppendRowGroups / LoadRowGroups
    Fill(other, 0, 2 * kGroup + 5);
    Table loaded("l", kSchema, kGroup);
    std::vector<std::shared_ptr<const RowGroup>> groups;
    const auto snap = other.Snapshot();
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        groups.push_back(snap->row_group_ptr(g));
    }
    const auto empty = loaded.Statistics();
    loaded.LoadRowGroups(std::move(groups));
    EXPECT_EQ(empty->row_count, 0U);
    EXPECT_EQ(loaded.Statistics()->row_count, 2 * kGroup + 5);
}

TEST(TableStatistics, ConcurrentReadersAndWritersSeeConsistentSnapshots) {
    Table table("t", kSchema, kGroup);
    std::atomic<bool> done{false};
    std::atomic<bool> bad{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; r++) {
        readers.emplace_back([&] {
            idx_t previous = 0;
            while (!done.load()) {
                const auto stats = table.Statistics();
                // rows only grow, and a column's bounds always cover exactly what was inserted
                if (stats->row_count < previous) {
                    bad = true;
                }
                previous = stats->row_count;
                if (stats->row_count > 0 &&
                    (!stats->columns[0].max || stats->columns[0].max->GetBigInt() !=
                                                   static_cast<int64_t>(stats->row_count) - 1)) {
                    bad = true;
                }
            }
        });
    }
    for (idx_t at = 0; at < 20000; at += 500) {
        Fill(table, at, 500);
    }
    done = true;
    for (auto& t : readers) {
        t.join();
    }
    EXPECT_FALSE(bad.load());
    EXPECT_EQ(table.Statistics()->row_count, 20000U);
}

TEST(TableStatistics, RandomDataMatchesAnExactComputation) {
    for (uint64_t seed = 1; seed <= 6; seed++) {
        test::Rng rng(seed);
        Table table("t", test::AllTypesSchema(), kGroup);
        test::TableModel model(table.schema().size());
        const idx_t rows = 1 + test::RandBelow(rng, 3 * kGroup);
        for (idx_t done = 0; done < rows;) {
            const idx_t n = std::min<idx_t>(1 + test::RandBelow(rng, kVectorSize), rows - done);
            table.Append(test::RandomChunk(table.schema(), rng, n, model, 0.3));
            done += n;
        }
        const auto stats = table.Statistics();
        ASSERT_EQ(stats->row_count, rows);
        for (idx_t c = 0; c < model.cols.size(); c++) {
            idx_t nulls = 0;
            std::optional<Value> lo, hi;
            std::vector<Value> distinct;
            for (const Value& v : model.cols[c]) {
                if (v.IsNull()) {
                    nulls++;
                    continue;
                }
                if (!lo || Value::Compare(v, *lo) < 0) {
                    lo = v;
                }
                if (!hi || Value::Compare(v, *hi) > 0) {
                    hi = v;
                }
                distinct.push_back(v);
            }
            std::sort(distinct.begin(), distinct.end(),
                      [](const Value& a, const Value& b) { return Value::Compare(a, b) < 0; });
            distinct.erase(std::unique(distinct.begin(), distinct.end(),
                                       [](const Value& a, const Value& b) {
                                           return Value::Compare(a, b) == 0;
                                       }),
                           distinct.end());
            const ColumnStatistics& got = stats->columns[c];
            const std::string what =
                "seed " + std::to_string(seed) + " column " + table.schema()[c].name;
            EXPECT_EQ(got.null_count, nulls) << what;
            if (lo && got.min) { // (VARCHAR bounds are absent when some string is too long)
                EXPECT_EQ(Value::Compare(*got.min, *lo), 0) << what;
                EXPECT_EQ(Value::Compare(*got.max, *hi), 0) << what;
            }
            ExpectNear(got.distinct, static_cast<double>(distinct.size()), 0.06, what);
        }
    }
}

} // namespace cdb
