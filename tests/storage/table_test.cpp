#include "storage/table.h"

#include "common/error.h"
#include "storage_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

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

TEST(Table, AppendRowGroupsKeepsTheOpenTailAndAppendsTheGroupsInOrder) {
    test::Rng rng(70);
    // Three sealed groups built elsewhere (a loader that builds row groups on several threads).
    Table source("src", AllTypesSchema(), kSmallGroup);
    TableModel source_model(source.schema().size());
    Fill(source, source_model, rng, 3 * kSmallGroup);
    const auto source_snapshot = source.Snapshot();
    ASSERT_EQ(source_snapshot->row_group_count(), 3U);

    // A table with one sealed group and a partial open tail.
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel expected(table.schema().size());
    Fill(table, expected, rng, kSmallGroup + 1000);
    ASSERT_EQ(table.Snapshot()->row_group_count(), 2U);

    // Append groups 0 and 2 (not 1): the tail is sealed first, then the groups follow in order.
    table.AppendRowGroups({source_snapshot->row_group_ptr(0), source_snapshot->row_group_ptr(2)});
    for (idx_t c = 0; c < expected.cols.size(); c++) {
        for (const auto& [first, last] :
             {std::pair<idx_t, idx_t>{0, kSmallGroup},
              std::pair<idx_t, idx_t>{2 * kSmallGroup, 3 * kSmallGroup}}) {
            expected.cols[c].insert(expected.cols[c].end(),
                                    source_model.cols[c].begin() + static_cast<long>(first),
                                    source_model.cols[c].begin() + static_cast<long>(last));
        }
    }
    EXPECT_EQ(table.RowCount(), expected.rows());
    {
        const auto snap = table.Snapshot();
        std::vector<idx_t> sizes;
        for (idx_t g = 0; g < snap->row_group_count(); g++) {
            sizes.push_back(snap->row_group(g).count());
        }
        EXPECT_EQ(sizes, (std::vector<idx_t>{kSmallGroup, 1000, kSmallGroup, kSmallGroup}))
            << "the open tail became a short sealed group; no open tail is left";
        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), expected.cols, "after AppendRowGroups");
    }
    // Appending goes on in a fresh open group.
    Fill(table, expected, rng, 500);
    {
        const auto snap = table.Snapshot();
        EXPECT_EQ(snap->row_group_count(), 5U);
        EXPECT_EQ(snap->row_group(4).count(), 500U);
        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), expected.cols, "after appending again");
    }
    // The source groups are shared, not consumed.
    EXPECT_EQ(source.RowCount(), 3 * kSmallGroup);
    table.AppendRowGroups({});
    EXPECT_EQ(table.RowCount(), expected.rows());
}

TEST(Table, ValidateChunkRejectsNullsInNotNullColumnsWithoutAppending) {
    Table table("t",
                {{"a", LogicalType::Integer(), /*not_null=*/true}, {"b", LogicalType::Integer()}});
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer(), LogicalType::Integer()});
    for (idx_t r = 0; r < 4; r++) {
        chunk.SetValue(0, r, Value::Integer(static_cast<int32_t>(r)));
        chunk.SetValue(1, r, r % 2 ? Value::Null(LogicalType::Integer()) : Value::Integer(1));
    }
    chunk.SetCardinality(4);
    EXPECT_NO_THROW(table.ValidateChunk(chunk)) << "NULLs in a nullable column are fine";
    chunk.SetValue(0, 2, Value::Null(LogicalType::Integer()));
    try {
        table.ValidateChunk(chunk);
        FAIL() << "expected a NOT NULL error";
    } catch (const Error& e) {
        EXPECT_NE(
            std::string(e.what()).find("NOT NULL constraint failed: column \"a\" of table \"t\""),
            std::string::npos)
            << e.what();
    }
    EXPECT_EQ(table.RowCount(), 0U) << "validating appends nothing";
    EXPECT_THROW(table.Append(chunk), Error) << "Append rejects the same chunk";
    EXPECT_EQ(table.RowCount(), 0U);
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

// ------------------------------------------------------------------ NOT NULL

TEST(TableNotNull, RejectsNullsAndLeavesTheTableUntouched) {
    Table table("t",
                {{"id", LogicalType::Integer(), /*not_null=*/true},
                 {"note", LogicalType::Varchar(), /*not_null=*/false}},
                kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer(), LogicalType::Varchar()});
    for (idx_t i = 0; i < 100; i++) {
        chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(i)));
        chunk.SetValue(1, i, i % 2 ? Value::Null(LogicalType::Varchar()) : Value::Varchar("x"));
    }
    chunk.SetCardinality(100);
    table.Append(chunk); // NULLs in the nullable column are fine
    EXPECT_EQ(table.RowCount(), 100u);

    chunk.SetValue(0, 57, Value::Null(LogicalType::Integer()));
    try {
        table.Append(chunk);
        FAIL() << "expected a NOT NULL violation";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Execution);
        EXPECT_NE(std::string(e.what()).find("NOT NULL"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("\"id\""), std::string::npos);
    }
    EXPECT_EQ(table.RowCount(), 100u); // atomic: no partial append
}

TEST(TableNotNull, ChecksOnlyTheRowsOfTheChunkAndAnyVectorFormat) {
    Table table("t", {{"x", LogicalType::Integer(), true}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (idx_t i = 0; i < 10; i++)
        chunk.SetValue(0, i, Value::Integer(1));
    chunk.SetValue(0, 500, Value::Null(LogicalType::Integer())); // beyond the cardinality
    chunk.SetCardinality(10);
    table.Append(chunk); // the NULL past row 10 is not part of the chunk
    EXPECT_EQ(table.RowCount(), 10u);

    DataChunk constant_null;
    constant_null.Initialize({LogicalType::Integer()});
    constant_null.column(0).SetConstant(Value::Null(LogicalType::Integer()));
    constant_null.SetCardinality(3);
    EXPECT_THROW(table.Append(constant_null), Error);

    DataChunk dict;
    dict.Initialize({LogicalType::Integer()});
    for (idx_t i = 0; i < 4; i++)
        dict.SetValue(0, i, Value::Integer(5));
    dict.SetValue(0, 2, Value::Null(LogicalType::Integer()));
    dict.SetCardinality(4);
    SelectionVector sel(2);
    sel.Set(0, 0);
    sel.Set(1, 2); // selects the NULL
    dict.Slice(sel, 2);
    EXPECT_THROW(table.Append(dict), Error);
    EXPECT_EQ(table.RowCount(), 10u);
}

// ------------------------------------------------------------------ Merge (atomic bulk load)

TEST(TableMerge, AppendsStagingRowsAfterExistingOnes) {
    test::Rng rng(11);
    for (idx_t base_rows : {idx_t{0}, idx_t{100}, idx_t{kSmallGroup}, idx_t{kSmallGroup + 7}}) {
        for (idx_t staged_rows : {idx_t{0}, idx_t{1}, idx_t{3000}, idx_t{2 * kSmallGroup + 5}}) {
            Table table("t", AllTypesSchema(), kSmallGroup);
            TableModel model(table.schema().size());
            Fill(table, model, rng, base_rows);
            auto staging = std::make_unique<Table>("staging", AllTypesSchema(), kSmallGroup);
            Fill(*staging, model, rng,
                 staged_rows); // the model gets the staged rows after the base rows

            table.Merge(std::move(staging));
            EXPECT_EQ(table.RowCount(), base_rows + staged_rows);
            auto snap = table.Snapshot();
            TableScan scan(snap, test::AllColumns(*snap));
            ExpectColumnsEqual(ScanAll(scan), model.cols,
                               "base=" + std::to_string(base_rows) +
                                   " staged=" + std::to_string(staged_rows));

            // the table keeps working after the merge: further appends land in order
            Fill(table, model, rng, 1234);
            auto snap2 = table.Snapshot();
            TableScan scan2(snap2, test::AllColumns(*snap2));
            ExpectColumnsEqual(ScanAll(scan2), model.cols, "after a further append");
        }
    }
}

TEST(TableMerge, OldSnapshotsAreUnaffectedAndTheTailCacheIsInvalidated) {
    test::Rng rng(12);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    Fill(table, model, rng, 500);
    auto before = table.Snapshot();
    const auto frozen = model.cols;
    auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
    Fill(*staging, model, rng, 5000);
    table.Merge(std::move(staging));

    TableScan old_scan(before, test::AllColumns(*before));
    ExpectColumnsEqual(ScanAll(old_scan), frozen, "snapshot taken before the merge");
    auto after = table.Snapshot();
    TableScan new_scan(after, test::AllColumns(*after));
    ExpectColumnsEqual(ScanAll(new_scan), model.cols, "snapshot taken after the merge");
}

TEST(TableMerge, ReadersSeeEitherNoneOrAllOfTheMergedRows) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    constexpr idx_t kBatch = 7000;
    constexpr int kMerges = 20;
    std::atomic<bool> done{false}, torn{false};
    std::thread reader([&] {
        while (!done.load() && !torn.load()) {
            const idx_t n = table.Snapshot()->row_count();
            if (n % kBatch != 0)
                torn = true; // a partial merge became visible
        }
    });
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    for (int m = 0; m < kMerges; m++) {
        auto staging = std::make_unique<Table>(
            "s", std::vector<ColumnDefinition>{{"x", LogicalType::Integer()}}, kSmallGroup);
        for (idx_t first = 0; first < kBatch; first += kVectorSize) {
            chunk.Reset();
            const idx_t n = std::min<idx_t>(kVectorSize, kBatch - first);
            for (idx_t i = 0; i < n; i++)
                chunk.SetValue(0, i, Value::Integer(static_cast<int32_t>(first + i)));
            chunk.SetCardinality(n);
            staging->Append(chunk);
        }
        table.Merge(std::move(staging));
    }
    done = true;
    reader.join();
    EXPECT_FALSE(torn.load());
    EXPECT_EQ(table.RowCount(), kBatch * kMerges);
}

TEST(TableMerge, SmallLoadsContinueTheOpenRowGroupInsteadOfSealingShortOnes) {
    test::Rng rng(13);
    Table table("t", AllTypesSchema(), kSmallGroup);
    TableModel model(table.schema().size());
    // a table filled by single-row INSERTs (each one a staging table merged in) is one row group
    for (int i = 0; i < 300; i++) {
        auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
        Fill(*staging, model, rng, 1);
        table.Merge(std::move(staging));
    }
    {
        auto snap = table.Snapshot();
        EXPECT_EQ(snap->row_count(), 300U);
        EXPECT_EQ(snap->row_group_count(), 1U) << "not one row group per statement";
        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), model.cols, "after 300 single-row merges");
    }
    // a small load that overflows the open row group fills it and continues in the next
    {
        const idx_t room = kSmallGroup - 300;
        auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
        Fill(*staging, model, rng, room + 10);
        table.Merge(std::move(staging));
        auto snap = table.Snapshot();
        ASSERT_EQ(snap->row_group_count(), 2U);
        EXPECT_EQ(snap->row_group(0).count(), kSmallGroup);
        EXPECT_EQ(snap->row_group(1).count(), 10U);
        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), model.cols, "after the overflowing merge");
    }
    // a load that brings sealed row groups of its own is adopted as before: our short tail is
    // sealed, so the order of the rows is kept
    {
        auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
        Fill(*staging, model, rng, 2 * kSmallGroup + 17);
        table.Merge(std::move(staging));
        auto snap = table.Snapshot();
        std::vector<idx_t> counts;
        for (idx_t g = 0; g < snap->row_group_count(); g++) {
            counts.push_back(snap->row_group(g).count());
        }
        EXPECT_EQ(counts, (std::vector<idx_t>{kSmallGroup, 10, kSmallGroup, kSmallGroup, 17}));
        TableScan scan(snap, test::AllColumns(*snap));
        ExpectColumnsEqual(ScanAll(scan), model.cols, "after the bulk merge");
    }
}

TEST(TableMerge, RandomMixOfAppendsAndMergesKeepsEveryRowInOrder) {
    for (uint64_t seed = 1; seed <= 3; seed++) {
        test::Rng rng(seed * 77);
        Table table("t", AllTypesSchema(), kSmallGroup);
        TableModel model(table.schema().size());
        for (int step = 0; step < 40; step++) {
            switch (test::RandBelow(rng, 4)) {
            case 0:
                Fill(table, model, rng, test::RandBelow(rng, 1500));
                break;
            case 1: { // a few rows, as an INSERT ... VALUES
                auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
                Fill(*staging, model, rng, test::RandBelow(rng, 5));
                table.Merge(std::move(staging));
                break;
            }
            case 2: { // up to a bit more than a row group
                auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
                Fill(*staging, model, rng, test::RandBelow(rng, kSmallGroup + 500));
                table.Merge(std::move(staging));
                break;
            }
            default: { // a bulk load
                auto staging = std::make_unique<Table>("s", AllTypesSchema(), kSmallGroup);
                Fill(*staging, model, rng, test::RandBelow(rng, 2 * kSmallGroup));
                table.Merge(std::move(staging));
                break;
            }
            }
            if (step % 10 == 9) {
                auto snap = table.Snapshot();
                ASSERT_EQ(snap->row_count(), model.rows()) << "seed " << seed << " step " << step;
                TableScan scan(snap, test::AllColumns(*snap));
                ExpectColumnsEqual(ScanAll(scan), model.cols,
                                   "seed " + std::to_string(seed) + " step " +
                                       std::to_string(step));
            }
        }
        // no row group is empty, none exceeds the row group size
        auto snap = table.Snapshot();
        for (idx_t g = 0; g < snap->row_group_count(); g++) {
            EXPECT_GT(snap->row_group(g).count(), 0U);
            EXPECT_LE(snap->row_group(g).count(), kSmallGroup);
        }
    }
}

// ---------------------------------------------------------------- LoadRowGroups

namespace {
std::vector<std::shared_ptr<const RowGroup>> GroupsOf(const Table& table) {
    const auto snap = table.Snapshot();
    std::vector<std::shared_ptr<const RowGroup>> groups;
    for (idx_t g = 0; g < snap->row_group_count(); g++) {
        groups.push_back(snap->row_group_ptr(g));
    }
    return groups;
}
} // namespace

TEST(TableLoadRowGroups, RebuildsATableFromItsRowGroupsForEveryLengthAroundTheBoundaries) {
    for (idx_t total : {idx_t{0}, idx_t{1}, idx_t{2047}, idx_t{2048}, idx_t{4095}, idx_t{4096},
                        idx_t{4097}, idx_t{9000}, idx_t{3 * kSmallGroup}}) {
        test::Rng rng(total + 5);
        Table original("t", AllTypesSchema(), kSmallGroup);
        TableModel model(AllTypesSchema().size());
        Fill(original, model, rng, total);
        Table loaded("t", AllTypesSchema(), kSmallGroup);
        loaded.LoadRowGroups(GroupsOf(original));
        EXPECT_EQ(loaded.RowCount(), total);
        auto scan = loaded.Scan(test::AllColumns(*loaded.Snapshot()));
        ExpectColumnsEqual(ScanAll(scan), model.cols, "total " + std::to_string(total));
        // and it keeps accepting rows, in order
        TableModel more(AllTypesSchema().size());
        loaded.Append(RandomChunk(loaded.schema(), rng, 100, more));
        for (size_t c = 0; c < model.cols.size(); c++) {
            model.cols[c].insert(model.cols[c].end(), more.cols[c].begin(), more.cols[c].end());
        }
        auto again = loaded.Scan(test::AllColumns(*loaded.Snapshot()));
        ExpectColumnsEqual(ScanAll(again), model.cols,
                           "after appending, total " + std::to_string(total));
    }
}

TEST(TableLoadRowGroups, AShortLastGroupBecomesTheOpenTailSoRestartsDoNotFragmentTheTable) {
    test::Rng rng(11);
    Table original("t", AllTypesSchema(), kSmallGroup);
    TableModel model(AllTypesSchema().size());
    Fill(original, model, rng, kSmallGroup + 100); // one full group and a tail of 100
    const auto original_groups = GroupsOf(original);
    ASSERT_EQ(original_groups.size(), 2U);
    for (int restart = 0; restart < 5; restart++) {
        Table loaded("t", AllTypesSchema(), kSmallGroup);
        loaded.LoadRowGroups(GroupsOf(original));
        TableModel extra(AllTypesSchema().size());
        loaded.Append(RandomChunk(loaded.schema(), rng, 50, extra));
        EXPECT_EQ(loaded.Snapshot()->row_group_count(), 2U)
            << "the tail group was re-opened and continued, not sealed as a short group";
        EXPECT_EQ(loaded.RowCount(), kSmallGroup + 150);
    }
}

TEST(TableLoadRowGroups, AFullLastGroupStaysSealedAndShortOnesInTheMiddleStayShort) {
    test::Rng rng(12);
    // groups: full, short (a bulk load left it), full
    Table source("t", AllTypesSchema(), kSmallGroup);
    TableModel model(AllTypesSchema().size());
    std::vector<std::shared_ptr<const RowGroup>> groups;
    for (const idx_t rows : {kSmallGroup, idx_t{300}, kSmallGroup}) {
        Table piece("p", AllTypesSchema(), kSmallGroup);
        TableModel m(AllTypesSchema().size());
        Fill(piece, m, rng, rows);
        for (size_t c = 0; c < m.cols.size(); c++) {
            model.cols[c].insert(model.cols[c].end(), m.cols[c].begin(), m.cols[c].end());
        }
        for (auto& g : GroupsOf(piece)) {
            groups.push_back(g);
        }
    }
    ASSERT_EQ(groups.size(), 3U);
    Table loaded("t", AllTypesSchema(), kSmallGroup);
    loaded.LoadRowGroups(groups);
    const auto snap = loaded.Snapshot();
    ASSERT_EQ(snap->row_group_count(), 3U);
    EXPECT_EQ(snap->row_group(1).count(), 300U);
    auto scan = loaded.Scan(test::AllColumns(*snap));
    ExpectColumnsEqual(ScanAll(scan), model.cols);
}

TEST(TableLoadRowGroups, TheReopenedTailStillEnforcesNotNull) {
    std::vector<ColumnDefinition> schema = {{"x", LogicalType::Integer(), true}};
    Table nullable("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    chunk.SetValue(0, 0, Value::Integer(1));
    chunk.SetValue(0, 1, Value::Null(LogicalType::Integer()));
    chunk.SetCardinality(2);
    nullable.Append(chunk);
    Table strict("t", schema, kSmallGroup);
    try {
        strict.LoadRowGroups(GroupsOf(nullable));
        FAIL() << "a NULL in a NOT NULL column must not be loaded silently";
    } catch (const Error& e) {
        EXPECT_EQ(e.code(), ErrorCode::Execution);
    }
}

TEST(TableLoadRowGroupsDeathTest, OnlyAnEmptyTableCanBeLoaded) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    DataChunk chunk;
    chunk.Initialize({LogicalType::Integer()});
    chunk.SetValue(0, 0, Value::Integer(1));
    chunk.SetCardinality(1);
    table.Append(chunk);
    EXPECT_DEATH(table.LoadRowGroups({}), "CDB_CHECK");
}

TEST(TableMergeDeathTest, StagingMustMatchTheTable) {
    Table table("t", {{"x", LogicalType::Integer()}}, kSmallGroup);
    EXPECT_DEATH(
        table.Merge(std::make_unique<Table>(
            "s", std::vector<ColumnDefinition>{{"x", LogicalType::BigInt()}}, kSmallGroup)),
        "CDB_CHECK");
    EXPECT_DEATH(
        table.Merge(std::make_unique<Table>(
            "s", std::vector<ColumnDefinition>{{"x", LogicalType::Integer()}}, 2 * kSmallGroup)),
        "CDB_CHECK");
    EXPECT_DEATH(table.Merge(nullptr), "CDB_CHECK");
}

} // namespace cdb
