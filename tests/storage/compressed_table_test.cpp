// Compression through the real write path: Table appends, row groups seal (and encode), snapshots
// and scans read encoded and raw segments side by side, and bulk loads merge them.

#include "storage/table.h"

#include "storage_test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;

const std::vector<ColumnDefinition> kSchema = {
    {"id", LogicalType::BigInt()},    {"flag", LogicalType::Integer()},
    {"price", LogicalType::Double()}, {"day", LogicalType::Date()},
    {"mode", LogicalType::Varchar()}, {"ok", LogicalType::Boolean()},
};

// Rows shaped like warehouse data: ascending ids, a small-domain integer, two-decimal prices, dates
// in runs, a handful of string values with NULLs, and booleans.
struct Generator {
    Rng rng{99};
    int64_t next_id = 1;
    int32_t day = 10000;
    idx_t day_left = 0;

    void Row(std::vector<Value>& row) {
        row.clear();
        next_id += static_cast<int64_t>(RandBelow(rng, 3));
        row.push_back(Value::BigInt(next_id));
        row.push_back(Value::Integer(static_cast<int32_t>(RandBelow(rng, 5))));
        row.push_back(Chance(rng, 0.05)
                          ? Value::Null(LogicalType::Double())
                          : Value::Double(static_cast<double>(RandBelow(rng, 500000)) / 100.0));
        if (day_left == 0) {
            day += static_cast<int32_t>(RandBelow(rng, 20));
            day_left = 50 + RandBelow(rng, 400);
        }
        day_left--;
        row.push_back(Value::Date(date_t{day}));
        static const char* const modes[] = {"AIR",  "RAIL", "SHIP",   "TRUCK",
                                            "MAIL", "FOB",  "REG AIR"};
        row.push_back(Chance(rng, 0.03) ? Value::Null(LogicalType::Varchar())
                                        : Value::Varchar(modes[RandBelow(rng, 7)]));
        row.push_back(Value::Boolean(Chance(rng, 0.9)));
    }
};

// Appends `rows` generated rows in chunks of random sizes; returns the model, one vector per
// column.
std::vector<std::vector<Value>> Fill(Table& table, idx_t rows, Generator& gen, Rng& sizes) {
    std::vector<std::vector<Value>> model(kSchema.size());
    std::vector<LogicalType> types;
    for (const auto& c : kSchema) {
        types.push_back(c.type);
    }
    DataChunk chunk;
    chunk.Initialize(types);
    std::vector<Value> row;
    idx_t done = 0;
    while (done < rows) {
        const idx_t n = std::min<idx_t>(rows - done, 1 + RandBelow(sizes, kVectorSize));
        chunk.Reset();
        for (idx_t i = 0; i < n; i++) {
            gen.Row(row);
            for (idx_t c = 0; c < kSchema.size(); c++) {
                chunk.SetValue(c, i, row[c]);
                model[c].push_back(row[c]);
            }
        }
        chunk.SetCardinality(n);
        table.Append(chunk);
        done += n;
    }
    return model;
}

void ExpectScanEquals(const Table& table, const std::vector<std::vector<Value>>& model,
                      const std::string& what) {
    const auto snap = table.Snapshot();
    ASSERT_EQ(snap->row_count(), model[0].size()) << what;
    TableScan scan(snap, test::AllColumns(*snap));
    const auto got = test::ScanAll(scan);
    for (idx_t c = 0; c < model.size(); c++) {
        ASSERT_EQ(got[c].size(), model[c].size()) << what;
        for (idx_t r = 0; r < model[c].size(); r++) {
            ASSERT_TRUE(test::BitIdentical(got[c][r], model[c][r]))
                << what << " column " << c << " row " << r << ": " << got[c][r].ToString() << " vs "
                << model[c][r].ToString();
        }
    }
}

size_t EncodedSegments(const Table& table) {
    const auto snap = table.Snapshot();
    size_t encoded = 0;
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        for (idx_t c = 0; c < snap->row_group(g).ColumnCount(); c++) {
            encoded += snap->row_group(g).column(c).encoded();
        }
    }
    return encoded;
}

} // namespace

TEST(CompressedTable, ScansReturnExactlyWhatWasAppended) {
    const test::ScopedCompression on(true);
    Table table("t", kSchema, 4 * kVectorSize);
    Generator gen;
    Rng sizes(1);
    const auto model = Fill(table, 70000, gen, sizes); // many sealed groups plus an open tail
    ExpectScanEquals(table, model, "after load");
    // 70000 rows in groups of 8192: 8 sealed groups x 6 columns, every one encoded; the open tail
    // is raw
    EXPECT_EQ(EncodedSegments(table), 8 * kSchema.size())
        << "every sealed column of this data must be encoded";
    // projections, in any order, and a column repeated
    const auto snap = table.Snapshot();
    TableScan scan(snap, {4, 0, 2, 4});
    const auto got = test::ScanAll(scan);
    for (idx_t r = 0; r < model[0].size(); r += 977) {
        ASSERT_TRUE(test::BitIdentical(got[0][r], model[4][r]));
        ASSERT_TRUE(test::BitIdentical(got[1][r], model[0][r]));
        ASSERT_TRUE(test::BitIdentical(got[2][r], model[2][r]));
        ASSERT_TRUE(test::BitIdentical(got[3][r], model[4][r]));
    }
}

TEST(CompressedTable, MemoryFootprintShrinksSubstantially) {
    size_t compressed = 0, raw = 0;
    for (const bool on : {true, false}) {
        const test::ScopedCompression mode(on);
        Table table("t", kSchema, 8 * kVectorSize);
        Generator gen;
        Rng sizes(2);
        Fill(table, 160000, gen, sizes);
        (on ? compressed : raw) = table.MemoryUsage();
    }
    EXPECT_LT(compressed * 10, raw * 4) << "compressed " << compressed << " bytes vs raw " << raw
                                        << ": expected under 40% on warehouse-shaped data";
}

TEST(CompressedTable, SnapshotsStayConsistentWhileGroupsSealAndEncode) {
    const test::ScopedCompression on(true);
    Table table("t", kSchema, 2 * kVectorSize);
    Generator gen;
    Rng sizes(3);
    auto model = Fill(table, 3000, gen, sizes); // one sealed group + an open tail
    const auto early = table.Snapshot();
    const std::vector<std::vector<Value>> early_model = model;
    const auto more = Fill(table, 20000, gen, sizes); // seals more groups, encodes them
    for (idx_t c = 0; c < model.size(); c++) {
        model[c].insert(model[c].end(), more[c].begin(), more[c].end());
    }
    // the early snapshot still shows exactly the first 3000 rows
    TableScan old_scan(early, test::AllColumns(*early));
    const auto old_rows = test::ScanAll(old_scan);
    ASSERT_EQ(old_rows[0].size(), 3000U);
    for (idx_t c = 0; c < model.size(); c++) {
        for (idx_t r = 0; r < 3000; r++) {
            ASSERT_TRUE(test::BitIdentical(old_rows[c][r], early_model[c][r])) << c << " " << r;
        }
    }
    ExpectScanEquals(table, model, "latest");
}

TEST(CompressedTable, ZoneMapsStillPruneEncodedGroups) {
    const test::ScopedCompression on(true);
    Table table("t", kSchema, 2 * kVectorSize);
    Generator gen;
    Rng sizes(4);
    const auto model = Fill(table, 40000, gen, sizes);
    ASSERT_GT(EncodedSegments(table), 0U);
    // ids ascend, so a high threshold can only match the last groups
    const int64_t threshold = model[0].back().GetBigInt() - 500;
    TableScan scan(table.Snapshot(), {0},
                   {TableFilter{0, CompareOp::Ge, Value::BigInt(threshold)}});
    idx_t rows_seen = 0;
    DataChunk chunk;
    chunk.Initialize(scan.types());
    while (scan.Next(chunk)) {
        rows_seen += chunk.size();
    }
    EXPECT_GT(scan.row_groups_skipped(), scan.row_groups_scanned() * 3)
        << "most groups must be skipped";
    EXPECT_LT(rows_seen, 6 * 2 * kVectorSize);
    // every row that satisfies the filter is among the rows that were read
    idx_t matching = 0;
    for (const Value& v : model[0]) {
        matching += v.GetBigInt() >= threshold;
    }
    EXPECT_GE(rows_seen, matching);
}

TEST(CompressedTable, BulkLoadMergePreservesEncodedGroupsAndTheTail) {
    const test::ScopedCompression on(true);
    Table target("t", kSchema, 2 * kVectorSize);
    Generator gen;
    Rng sizes(5);
    auto model = Fill(target, 5000, gen, sizes);
    auto staging = std::make_unique<Table>("staging", kSchema, 2 * kVectorSize);
    const auto more = Fill(*staging, 11000, gen, sizes);
    target.Merge(std::move(staging));
    for (idx_t c = 0; c < model.size(); c++) {
        model[c].insert(model[c].end(), more[c].begin(), more[c].end());
    }
    ExpectScanEquals(target, model, "after merge");
    EXPECT_GT(EncodedSegments(target), 0U);
    // appends after the merge still work and are visible
    const auto tail = Fill(target, 3000, gen, sizes);
    for (idx_t c = 0; c < model.size(); c++) {
        model[c].insert(model[c].end(), tail[c].begin(), tail[c].end());
    }
    ExpectScanEquals(target, model, "after appends");
}

TEST(CompressedTable, IncompressibleColumnsStayRawAndStayCorrect) {
    const test::ScopedCompression on(true);
    const std::vector<ColumnDefinition> schema = {
        {"r", LogicalType::BigInt()}, {"d", LogicalType::Double()}, {"s", LogicalType::Varchar()}};
    Table table("t", schema, 2 * kVectorSize);
    test::TableModel model(schema.size());
    Rng rng(6);
    for (int k = 0; k < 8; k++) {
        const DataChunk chunk = test::RandomChunk(schema, rng, kVectorSize, model, 0.1);
        table.Append(chunk);
    }
    const auto snap = table.Snapshot();
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        EXPECT_FALSE(snap->row_group(g).column(0).encoded()) << "random 64-bit values";
        EXPECT_FALSE(snap->row_group(g).column(2).encoded()) << "random strings";
    }
    TableScan scan(snap, test::AllColumns(*snap));
    const auto got = test::ScanAll(scan);
    for (idx_t c = 0; c < schema.size(); c++) {
        for (idx_t r = 0; r < model.rows(); r++) {
            ASSERT_TRUE(test::BitIdentical(got[c][r], model.cols[c][r])) << c << " " << r;
        }
    }
}

} // namespace cdb
