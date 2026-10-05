#include "storage/table.h"

#include "common/error.h"
#include "storage_test_util.h"

#include <gtest/gtest.h>

namespace cdb {

namespace {

using test::AllTypesSchema;
using test::ExpectColumnsEqual;
using test::RandomChunk;
using test::ScanAll;
using test::TableModel;

constexpr idx_t kSmallGroup = 2 * kVectorSize; // 4096-row groups: boundaries hit cheaply

// Appends `total` random rows to `table` in random-sized chunks (sometimes empty), mirroring
// them into `model`.
void Fill(Table& table, TableModel& model, test::Rng& rng, idx_t total, double nulls = 0.2) {
    idx_t done = 0;
    while (done < total) {
        const idx_t n = std::min<idx_t>(test::RandBelow(rng, kVectorSize + 1), total - done);
        DataChunk chunk = RandomChunk(table.schema(), rng, n, model, nulls);
        table.Append(chunk);
        done += n;
    }
}

} // namespace

TEST(Table, AppendThenScanRoundTripsEveryType) {
    for (idx_t total : {idx_t{0}, idx_t{1}, idx_t{2047}, idx_t{2048}, idx_t{4095}, idx_t{4096},
                        idx_t{4097}, idx_t{10000}, idx_t{3 * kSmallGroup}}) {
        test::Rng rng(total + 1);
        Table table("t", AllTypesSchema(), kSmallGroup);
        TableModel model(table.schema().size());
        Fill(table, model, rng, total);

        EXPECT_EQ(table.RowCount(), total);
        auto snap = table.Snapshot();
        EXPECT_EQ(snap->row_count(), total);
        EXPECT_EQ(snap->row_group_count(), (total + kSmallGroup - 1) / kSmallGroup) << total;

        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), model.cols, "total=" + std::to_string(total));
    }
}

TEST(Table, DefaultRowGroupSizeIsSixtyVectors) {
    EXPECT_EQ(kRowGroupSize, 60u * kVectorSize);
    Table t("t", {{"x", LogicalType::Integer()}});
    EXPECT_EQ(t.row_group_size(), kRowGroupSize);
}

TEST(Table, ExactGroupBoundaryLeavesNoOpenTail) {
    test::Rng rng(1);
    Table table("t", {{"x", LogicalType::BigInt()}}, kSmallGroup);
    TableModel model(1);
    Fill(table, model, rng, kSmallGroup);
    EXPECT_EQ(table.Snapshot()->row_group_count(), 1u);
    Fill(table, model, rng, 1);
    EXPECT_EQ(table.Snapshot()->row_group_count(), 2u);
    EXPECT_EQ(table.Snapshot()->row_group(1).count(), 1u);
}

TEST(Table, ProjectionSelectsOrdersAndMayRepeatColumns) {
    test::Rng rng(2);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    Fill(table, model, rng, 6000);
    auto snap = table.Snapshot();

    for (std::vector<idx_t> cols :
         {std::vector<idx_t>{5}, {5, 0}, {3, 3, 1}, {0, 1, 2, 3, 4, 5}, {4, 2}}) {
        TableScan scan(snap, cols);
        std::vector<std::vector<Value>> expect;
        for (idx_t c : cols)
            expect.push_back(model.cols[c]);
        ExpectColumnsEqual(ScanAll(scan), expect);
    }
    // Empty projection (what COUNT(*) needs): no columns, but the chunks still carry row counts.
    TableScan none(snap, {});
    DataChunk chunk;
    chunk.Initialize({});
    idx_t rows = 0;
    while (none.Next(chunk)) {
        EXPECT_EQ(chunk.ColumnCount(), 0u);
        rows += chunk.size();
    }
    EXPECT_EQ(rows, 6000u);
}

TEST(Table, ScanOnlyTouchesProjectedColumns) {
    test::Rng rng(3);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    Fill(table, model, rng, 9000);
    auto snap = table.Snapshot();

    TableScan scan(snap, {1, 5}); // i and s only
    ScanAll(scan);
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        for (idx_t c = 0; c < snap->schema().size(); c++) {
            const uint64_t calls = snap->row_group(g).column(c).scan_calls();
            if (c == 1 || c == 5) {
                EXPECT_GT(calls, 0u) << "group " << g << " column " << c;
            } else {
                EXPECT_EQ(calls, 0u) << "column " << c << " was scanned but not projected";
            }
        }
    }
}

TEST(Table, SnapshotsAreIsolatedFromLaterAppends) {
    test::Rng rng(4);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    Fill(table, model, rng, 5000); // one sealed group + a partial open tail
    auto snap1 = table.Snapshot();
    const auto frozen = model.cols;

    Fill(table, model, rng, 9000); // seals more groups and grows the tail
    auto snap2 = table.Snapshot();

    TableScan s1(snap1, test::AllColumns(*snap1));
    ExpectColumnsEqual(ScanAll(s1), frozen, "old snapshot");
    TableScan s2(snap2, test::AllColumns(*snap2));
    ExpectColumnsEqual(ScanAll(s2), model.cols, "new snapshot");
    EXPECT_EQ(snap1->row_count(), 5000u);
    EXPECT_EQ(snap2->row_count(), 14000u);
}

TEST(Table, TailSnapshotIsCachedUntilTheNextAppend) {
    test::Rng rng(5);
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    TableModel model(1);
    Fill(table, model, rng, 100);
    auto a = table.Snapshot();
    auto b = table.Snapshot();
    ASSERT_EQ(a->row_group_count(), 1u);
    EXPECT_EQ(a->row_group_ptr(0).get(), b->row_group_ptr(0).get()); // no re-copy

    Fill(table, model, rng, 1);
    auto c = table.Snapshot();
    EXPECT_NE(a->row_group_ptr(0).get(), c->row_group_ptr(0).get());
    EXPECT_EQ(c->row_count(), 101u);
    EXPECT_EQ(a->row_count(), 100u);
}

TEST(Table, EmptyTable) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    EXPECT_EQ(table.RowCount(), 0u);
    auto snap = table.Snapshot();
    EXPECT_EQ(snap->row_group_count(), 0u);
    TableScan scan(snap, {0});
    DataChunk chunk;
    chunk.Initialize(scan.types());
    EXPECT_FALSE(scan.Next(chunk));
    EXPECT_FALSE(scan.Next(chunk)); // stays exhausted
    EXPECT_EQ(chunk.size(), 0u);
}

TEST(Table, EmptyChunkAppendIsANoOp) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    table.Append(chunk);
    EXPECT_EQ(table.RowCount(), 0u);
    EXPECT_EQ(table.Snapshot()->row_group_count(), 0u);
}

TEST(Table, AppendsFromDictionaryAndConstantVectors) {
    Table table("t", {{"a", LogicalType::Integer()}, {"b", LogicalType::Varchar()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer(), LogicalType::Varchar()});
    for (idx_t i = 0; i < 100; i++) {
        chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(i)));
        chunk.SetValue(1, i, Value::Varchar(std::string(20, 'a' + i % 26)));
    }
    chunk.SetCardinality(100);
    SelectionVector sel(3);
    sel.Set(0, 99);
    sel.Set(1, 0);
    sel.Set(2, 50);
    chunk.column(0).Slice(sel, 3);
    chunk.column(1).SetConstant(Value::Varchar("constant string, longer than twelve"));
    chunk.SetCardinality(3);
    table.Append(chunk);

    TableScan scan = table.Scan({0, 1});
    auto rows = ScanAll(scan);
    ASSERT_EQ(rows[0].size(), 3u);
    EXPECT_EQ(rows[0][0].GetInteger(), 99);
    EXPECT_EQ(rows[0][1].GetInteger(), 0);
    EXPECT_EQ(rows[0][2].GetInteger(), 50);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(rows[1][i].GetVarchar(), "constant string, longer than twelve");
    }
}

TEST(Table, LongStringsAcrossManyGroups) {
    test::Rng rng(6);
    Table table("t", {{"id", LogicalType::BigInt()}, {"s", LogicalType::Varchar()}}, kSmallGroup);
    std::vector<Value> ids, strs;
    DataChunk chunk;
    chunk.Initialize({LogicalType::BigInt(), LogicalType::Varchar()});
    for (idx_t base = 0; base < 20000; base += 1000) {
        chunk.Reset();
        for (idx_t i = 0; i < 1000; i++) {
            const idx_t id = base + i;
            ids.push_back(Value::BigInt(static_cast<int64_t>(id)));
            strs.push_back(
                Value::Varchar("row-" + std::to_string(id) + std::string(15 + id % 40, 'x')));
            chunk.SetValue(0, i, ids.back());
            chunk.SetValue(1, i, strs.back());
        }
        chunk.SetCardinality(1000);
        table.Append(chunk);
    }
    TableScan scan = table.Scan({0, 1});
    ExpectColumnsEqual(ScanAll(scan), {ids, strs});
}

TEST(Table, MemoryUsageGrowsWithData) {
    test::Rng rng(7);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    const size_t empty = table.MemoryUsage();
    Fill(table, model, rng, 2000);
    const size_t small = table.MemoryUsage();
    Fill(table, model, rng, 20000);
    const size_t big = table.MemoryUsage();
    EXPECT_EQ(empty, 0u);
    EXPECT_GT(small, 2000u * 20); // at least the fixed-width payload
    EXPECT_GT(big, small);
}

// ------------------------------------------------------------------ schema / errors

TEST(Table, FindColumnIsCaseInsensitive) {
    Table t("t", {{"Id", LogicalType::Integer()}, {"Name", LogicalType::Varchar()}});
    EXPECT_EQ(t.FindColumn("id"), 0u);
    EXPECT_EQ(t.FindColumn("NAME"), 1u);
    EXPECT_FALSE(t.FindColumn("missing").has_value());
}

TEST(Table, InvalidSchemasAreRejected) {
    auto code_of = [](std::vector<ColumnDefinition> schema) {
        try {
            Table t("t", std::move(schema));
        } catch (const Error& e) {
            return e.code();
        }
        return ErrorCode::Internal; // sentinel: no error was thrown
    };
    EXPECT_EQ(code_of({}), ErrorCode::Binder);
    EXPECT_EQ(code_of({{"a", LogicalType::Integer()}, {"A", LogicalType::BigInt()}}),
              ErrorCode::Binder); // duplicate, case-insensitively
    EXPECT_EQ(code_of({{"", LogicalType::Integer()}}), ErrorCode::Binder);
}

TEST(TableDeathTest, ContractViolationsAbort) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    DataChunk wrong;
    wrong.Initialize({LogicalType::BigInt()});
    wrong.SetCardinality(0);
    EXPECT_DEATH(table.Append(wrong), "CDB_CHECK"); // type mismatch
    DataChunk two;
    two.Initialize({LogicalType::Integer(), LogicalType::Integer()});
    EXPECT_DEATH(table.Append(two), "CDB_CHECK");                                // arity mismatch
    EXPECT_DEATH(Table("t", {{"x", LogicalType::Integer()}}, 100), "CDB_CHECK"); // bad group size
    EXPECT_DEATH(table.Scan({7}), "CDB_CHECK");                                  // no such column
    TableScan scan = table.Scan({0});
    DataChunk bad;
    bad.Initialize({LogicalType::Double()});
    EXPECT_DEATH(scan.Next(bad), "CDB_CHECK"); // output chunk has the wrong schema
}

// ------------------------------------------------------------------ zone-map pruning

TEST(TableZoneMaps, SortedColumnPrunesWholeGroups) {
    Table table("t", {{"id", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (idx_t base = 0; base < 5 * kSmallGroup; base += kVectorSize) {
        chunk.Reset();
        for (idx_t i = 0; i < kVectorSize; i++) {
            chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(base + i)));
        }
        chunk.SetCardinality(kVectorSize);
        table.Append(chunk);
    }
    auto snap = table.Snapshot();
    ASSERT_EQ(snap->row_group_count(), 5u); // ids [0,4096) [4096,8192) ... [16384,20480)

    struct Case {
        CompareOp op;
        int32_t k;
        idx_t scanned;
    };
    const Case cases[] = {
        {CompareOp::Lt, 4096, 1},  {CompareOp::Lt, 4097, 2},  {CompareOp::Le, 4095, 1},
        {CompareOp::Ge, 16384, 1}, {CompareOp::Gt, 16383, 1}, {CompareOp::Gt, 20479, 0},
        {CompareOp::Eq, 9000, 1},  {CompareOp::Eq, 99999, 0}, {CompareOp::Ge, 0, 5},
        {CompareOp::Ne, 5, 5},     {CompareOp::Lt, 0, 0},
    };
    for (const Case& c : cases) {
        TableScan scan(snap, {0}, {{0, c.op, Value::Integer(c.k)}});
        ScanAll(scan);
        EXPECT_EQ(scan.row_groups_scanned(), c.scanned)
            << "id " << CompareOpName(c.op) << " " << c.k;
        EXPECT_EQ(scan.row_groups_scanned() + scan.row_groups_skipped(), 5u);
    }
}

TEST(TableZoneMaps, MultipleFiltersAreConjunctive) {
    Table table("t", {{"a", LogicalType::Integer()}, {"b", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer(), LogicalType::Integer()});
    for (idx_t base = 0; base < 4 * kSmallGroup; base += kVectorSize) {
        chunk.Reset();
        for (idx_t i = 0; i < kVectorSize; i++) {
            chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(base + i))); // ascending
            chunk.SetValue(1, i,
                           Value::Integer(static_cast<int32_t>(100000 - base - i))); // descending
        }
        chunk.SetCardinality(kVectorSize);
        table.Append(chunk);
    }
    auto snap = table.Snapshot();
    ASSERT_EQ(snap->row_group_count(), 4u);
    // a >= 8192 keeps groups 2,3; b >= 90000 keeps groups 0,1 (b falls from 100000) => none left
    TableScan scan(
        snap, {0},
        {{0, CompareOp::Ge, Value::Integer(8192)}, {1, CompareOp::Ge, Value::Integer(95000)}});
    ScanAll(scan);
    EXPECT_EQ(scan.row_groups_scanned(), 0u);
}

TEST(TableZoneMaps, AllNullGroupsAreSkippedByAnyComparison) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (idx_t i = 0; i < kVectorSize; i++)
        chunk.SetValue(0, i, Value::Null(LogicalType::Integer()));
    chunk.SetCardinality(kVectorSize);
    table.Append(chunk);
    for (CompareOp op : test::AllOps()) {
        TableScan scan = table.Scan({0}, {{0, op, Value::Integer(1)}});
        ScanAll(scan);
        EXPECT_EQ(scan.row_groups_scanned(), 0u) << CompareOpName(op);
    }
}

TEST(TableZoneMaps, PruningIsSoundOverRandomTablesAndFilters) {
    // Whatever the data and predicate, every row that satisfies the predicate must still be
    // returned (pruning may only remove groups with no matches). Also require that pruning
    // fires often enough for the test to mean something.
    size_t pruned_groups = 0, total_groups = 0;
    for (uint64_t seed = 0; seed < 25; seed++) {
        test::Rng rng(seed);
        Table table("t", AllTypesSchema(), kSmallGroup);
        TableModel model(table.schema().size());
        // cluster values by group so zone maps are tight: draw each chunk's values near a centre
        Fill(table, model, rng, 3 * kSmallGroup + test::RandBelow(rng, 3000), 0.15);
        auto snap = table.Snapshot();
        for (int k = 0; k < 20; k++) {
            const idx_t col = test::RandBelow(rng, table.schema().size());
            const LogicalType type = table.schema()[col].type;
            // half the time use a value that exists in the data, so Eq/Le/Ge can actually match
            Value constant = test::Chance(rng, 0.5) && !model.cols[col].empty()
                                 ? model.cols[col][test::RandBelow(rng, model.rows())]
                                 : test::RandomValue(rng, type, 0.05);
            const CompareOp op = test::AllOps()[test::RandBelow(rng, 6)];

            TableScan scan(snap, {col}, {{col, op, constant}});
            const auto got = ScanAll(scan)[0];
            // The scan output is a subsequence of the table's rows for this column: every
            // matching row must be in it. Count matches on both sides.
            size_t expect_matches = 0, got_matches = 0;
            for (const Value& v : model.cols[col])
                expect_matches += test::Satisfies(v, op, constant);
            for (const Value& v : got)
                got_matches += test::Satisfies(v, op, constant);
            ASSERT_EQ(got_matches, expect_matches)
                << "seed " << seed << " " << type.ToString() << " " << CompareOpName(op) << " "
                << constant.ToString();
            pruned_groups += scan.row_groups_skipped();
            total_groups += snap->row_group_count();
        }
    }
    EXPECT_GT(pruned_groups, 0u);
    EXPECT_LT(pruned_groups, total_groups);
}

} // namespace cdb
