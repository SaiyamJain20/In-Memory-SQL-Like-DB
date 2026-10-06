#include "execution/group_table.h"

#include "common/error.h"
#include "execution/chunk_store.h"
#include "execution/hashing.h"
#include "execution/key_index.h"

#include "exec_test_util.h"
#include "kernels/cpu.h"
#include "kernels/kernel_test_util.h"

#include <gtest/gtest.h>

#include <map>
#include <set>

namespace cdb {

namespace {

using test::Chance;
using test::CompareTuples;
using test::RandBelow;
using test::Rng;
using test::RowsOf;
using test::SameValue;
using test::SmallDomainValue;
using test::TupleLess;

const std::vector<LogicalType> kAllTypes = {LogicalType::Boolean(), LogicalType::Integer(),
                                            LogicalType::BigInt(),  LogicalType::Double(),
                                            LogicalType::Date(),    LogicalType::Varchar()};

test::ValueGen SmallGen(double nulls = 0.15) {
    return [nulls](Rng& rng, LogicalType t) { return SmallDomainValue(rng, t, nulls); };
}

} // namespace

// ---------------------------------------------------------------------------------- hashing

TEST(Hashing, EqualValuesHashEqualInEveryVectorFormat) {
    Rng rng(1);
    for (const LogicalType type : kAllTypes) {
        for (int iter = 0; iter < 20; iter++) {
            const idx_t n = 1 + RandBelow(rng, 200);
            const DataChunk chunk = test::RandomVariedChunk(rng, {type}, n, SmallGen());
            std::vector<uint64_t> h(n);
            HashVector(chunk.column(0), n, h.data(), false);
            const auto rows = RowsOf(chunk);
            for (idx_t i = 0; i < n; i++) {
                for (idx_t j = i + 1; j < n; j++) {
                    if (test::CompareWithNulls(rows[i][0], rows[j][0]) == 0) {
                        ASSERT_EQ(h[i], h[j]) << type.ToString() << ": " << rows[i][0].ToString();
                    }
                }
            }
        }
    }
}

TEST(Hashing, DoublesHashByEqualityNotBits) {
    Vector v(LogicalType::Double());
    double* d = v.FlatData<double>();
    d[0] = 0.0;
    d[1] = -0.0;
    d[2] = std::numeric_limits<double>::quiet_NaN();
    d[3] = -std::numeric_limits<double>::quiet_NaN();
    d[4] = std::numeric_limits<double>::signaling_NaN();
    d[5] = 1.0;
    uint64_t h[6];
    HashVector(v, 6, h, false);
    EXPECT_EQ(h[0], h[1]);
    EXPECT_EQ(h[2], h[3]);
    EXPECT_EQ(h[2], h[4]);
    EXPECT_NE(h[0], h[2]);
    EXPECT_NE(h[0], h[5]);
}

TEST(Hashing, NullsHashTogetherAndDifferFromValues) {
    Vector v(LogicalType::Integer());
    v.SetValue(0, Value::Null(LogicalType::Integer()));
    v.SetValue(1, Value::Null(LogicalType::Integer()));
    v.SetValue(2, Value::Integer(0));
    uint64_t h[3];
    HashVector(v, 3, h, false);
    EXPECT_EQ(h[0], h[1]);
    EXPECT_NE(h[0], h[2]);
}

TEST(Hashing, CombiningDependsOnColumnOrderAndSpreadsBits) {
    Vector a(LogicalType::Integer()), b(LogicalType::Integer());
    for (idx_t i = 0; i < 1000; i++) {
        a.SetValue(i, Value::Integer(static_cast<int32_t>(i % 10)));
        b.SetValue(i, Value::Integer(static_cast<int32_t>(i / 10)));
    }
    const Vector* ab[] = {&a, &b};
    const Vector* ba[] = {&b, &a};
    std::vector<uint64_t> h1(1000), h2(1000);
    HashColumns(ab, 2, 1000, h1.data());
    HashColumns(ba, 2, 1000, h2.data());
    EXPECT_NE(h1, h2);
    // All 1000 distinct (a, b) pairs: expect (essentially) no 64-bit collisions, and the low bits
    // used for table slots must be well spread.
    std::set<uint64_t> distinct(h1.begin(), h1.end());
    EXPECT_EQ(distinct.size(), 1000U);
    std::set<uint64_t> low;
    for (const uint64_t h : h1) {
        low.insert(h & 1023);
    }
    EXPECT_GT(low.size(), 550U) << "1000 keys into 1024 buckets should fill ~63%";
}

TEST(Hashing, StringsOfEveryLengthAndWithEmbeddedNuls) {
    std::set<uint64_t> seen;
    for (size_t len = 0; len < 40; len++) {
        seen.insert(HashBytes(std::string(len, 'a').data(), len));
        seen.insert(HashBytes(std::string(len, '\0').data(), len));
    }
    EXPECT_EQ(seen.size(), 79U) << "lengths 0..39 of 'a' and of NUL are all distinct (len 0 twice)";
}

// ---------------------------------------------------------------------------------- ChunkStore

TEST(ChunkStore, AppendGatherMatchesModelAcrossChunkBoundaries) {
    Rng rng(2);
    ChunkStore store(kAllTypes);
    std::vector<std::vector<Value>> model;
    const idx_t sizes[] = {1, 100, 2047, 1, 2048, 5, 2048, 2048, 0, 33};
    for (const idx_t n : sizes) {
        const DataChunk chunk = test::RandomVariedChunk(rng, kAllTypes, n, SmallGen());
        // sometimes through a selection (reversed, every other row)
        if (n > 4 && Chance(rng, 0.5)) {
            SelectionVector sel(n / 2);
            for (idx_t i = 0; i < n / 2; i++) {
                sel.Set(i, static_cast<sel_t>(n - 1 - 2 * i));
            }
            store.Append(chunk, &sel, n / 2);
            const auto rows = RowsOf(chunk);
            for (idx_t i = 0; i < n / 2; i++) {
                model.push_back(rows[n - 1 - 2 * i]);
            }
        } else {
            store.Append(chunk);
            for (auto& r : RowsOf(chunk)) {
                model.push_back(std::move(r));
            }
        }
        ASSERT_EQ(store.Count(), model.size());
    }
    // random gathers, incl. repeated and unordered rows
    for (int iter = 0; iter < 20; iter++) {
        const idx_t n = 1 + RandBelow(rng, 500);
        std::vector<uint32_t> rows(n);
        for (auto& r : rows) {
            r = static_cast<uint32_t>(RandBelow(rng, model.size()));
        }
        for (idx_t c = 0; c < kAllTypes.size(); c++) {
            Vector out(kAllTypes[c]);
            store.Gather(c, rows.data(), n, out);
            for (idx_t i = 0; i < n; i++) {
                ASSERT_TRUE(SameValue(out.GetValue(i), model[rows[i]][c]))
                    << "col " << c << " row " << rows[i];
            }
        }
    }
    // sequential reads
    DataChunk out;
    out.Initialize(kAllTypes);
    for (idx_t first = 0; first < model.size(); first += 1500) {
        const idx_t n = std::min<idx_t>(1500, model.size() - first);
        store.ReadRange(first, n, out);
        for (idx_t i = 0; i < n; i++) {
            for (idx_t c = 0; c < kAllTypes.size(); c++) {
                ASSERT_TRUE(SameValue(out.GetValue(c, i), model[first + i][c]));
            }
        }
    }
}

TEST(ChunkStore, OwnsItsStringsAfterTheSourceIsGone) {
    ChunkStore store({LogicalType::Varchar()});
    {
        DataChunk chunk;
        chunk.Initialize({LogicalType::Varchar()});
        for (idx_t i = 0; i < 100; i++) {
            chunk.SetValue(0, i,
                           Value::Varchar("a string that is definitely longer than twelve " +
                                          std::to_string(i)));
        }
        chunk.SetCardinality(100);
        store.Append(chunk);
    } // source chunk and its heap destroyed
    EXPECT_EQ(store.GetValue(0, 42).GetVarchar(),
              "a string that is definitely longer than twelve 42");
}

TEST(ChunkStore, AppendStoreAndClear) {
    Rng rng(3);
    ChunkStore a(kAllTypes), b(kAllTypes);
    std::vector<std::vector<Value>> model;
    for (int k = 0; k < 3; k++) {
        const DataChunk chunk = test::RandomVariedChunk(rng, kAllTypes, 1500, SmallGen());
        b.Append(chunk);
        for (auto& r : RowsOf(chunk)) {
            model.push_back(std::move(r));
        }
    }
    a.AppendStore(b);
    ASSERT_EQ(a.Count(), model.size());
    for (idx_t r = 0; r < model.size(); r += 97) {
        for (idx_t c = 0; c < kAllTypes.size(); c++) {
            ASSERT_TRUE(SameValue(a.GetValue(c, r), model[r][c]));
        }
    }
    a.Clear();
    EXPECT_EQ(a.Count(), 0U);
    EXPECT_EQ(a.ChunkCount(), 0U);
}

// ---------------------------------------------------------------------------------- KeyIndex

TEST(KeyIndex, IdsFollowFirstAppearanceAndMatchAModel) {
    Rng rng(4);
    for (int round = 0; round < 40; round++) {
        // random key shape: 1-3 columns of random types
        std::vector<LogicalType> types;
        const size_t ncols = 1 + RandBelow(rng, 3);
        for (size_t c = 0; c < ncols; c++) {
            types.push_back(kAllTypes[RandBelow(rng, kAllTypes.size())]);
        }
        KeyIndex index(types);
        std::map<std::vector<Value>, uint32_t, TupleLess> model;
        for (int batch = 0; batch < 6; batch++) {
            const idx_t n = RandBelow(rng, 3) == 0 ? kVectorSize : 1 + RandBelow(rng, 300);
            const DataChunk keys = test::RandomVariedChunk(rng, types, n, SmallGen());
            std::vector<uint32_t> ids(n);
            std::vector<sel_t> new_rows;
            const idx_t created = index.FindOrInsert(keys, n, ids.data(), &new_rows);
            const auto rows = RowsOf(keys);
            idx_t expect_new = 0;
            std::vector<sel_t> expect_new_rows;
            for (idx_t i = 0; i < n; i++) {
                auto [it, inserted] = model.emplace(rows[i], static_cast<uint32_t>(model.size()));
                if (inserted) {
                    expect_new++;
                    expect_new_rows.push_back(static_cast<sel_t>(i));
                }
                ASSERT_EQ(ids[i], it->second) << "round " << round << " row " << i;
            }
            ASSERT_EQ(created, expect_new);
            ASSERT_EQ(new_rows, expect_new_rows);
            ASSERT_EQ(index.Count(), model.size());
        }
        // every stored key round-trips
        for (const auto& [key, id] : model) {
            for (size_t c = 0; c < ncols; c++) {
                ASSERT_TRUE(SameValue(index.keys().GetValue(c, id), key[c]) ||
                            test::CompareWithNulls(index.keys().GetValue(c, id), key[c]) == 0);
            }
        }
    }
}

TEST(KeyComparator, NullsAreEqualOnlyWhenAskedTo) {
    // Joins never let a NULL key reach the comparator (they are filtered out first) so the
    // `nulls_equal = false` rule has no operator-level test: check the contract directly. GROUP BY
    // and DISTINCT use nulls_equal = true.
    ChunkStore store({LogicalType::Integer()});
    DataChunk stored;
    stored.Initialize({LogicalType::Integer()});
    stored.SetValue(0, 0, Value::Null(LogicalType::Integer()));
    stored.SetValue(0, 1, Value::Integer(1));
    stored.SetCardinality(2);
    store.Append(stored);

    DataChunk input;
    input.Initialize({LogicalType::Integer()});
    input.SetValue(0, 0, Value::Null(LogicalType::Integer()));
    input.SetValue(0, 1, Value::Integer(1));
    input.SetValue(0, 2, Value::Integer(2));
    input.SetCardinality(3);
    const std::vector<const Vector*> cols = {&input.column(0)};

    const KeyComparator group(cols, /*nulls_equal=*/true);
    EXPECT_TRUE(group.StoredEqualsInput(store, 0, 0)) << "NULL = NULL when grouping";
    EXPECT_TRUE(group.StoredEqualsInput(store, 1, 1));
    EXPECT_FALSE(group.StoredEqualsInput(store, 0, 1)) << "NULL never equals a value";
    EXPECT_FALSE(group.StoredEqualsInput(store, 1, 0));
    EXPECT_FALSE(group.StoredEqualsInput(store, 1, 2));
    EXPECT_TRUE(group.InputEqualsInput(0, 0));
    EXPECT_FALSE(group.InputEqualsInput(0, 1));

    const KeyComparator join(cols, /*nulls_equal=*/false);
    EXPECT_FALSE(join.StoredEqualsInput(store, 0, 0)) << "NULL = NULL is not a match in a join";
    EXPECT_TRUE(join.StoredEqualsInput(store, 1, 1));
    EXPECT_FALSE(join.StoredEqualsInput(store, 0, 1));
    EXPECT_FALSE(join.InputEqualsInput(0, 0));
    EXPECT_TRUE(join.InputEqualsInput(1, 1));
}

TEST(KeyIndex, ManyDistinctKeysForceTableGrowth) {
    KeyIndex index({LogicalType::BigInt(), LogicalType::Varchar()});
    DataChunk keys;
    keys.Initialize({LogicalType::BigInt(), LogicalType::Varchar()});
    std::vector<uint32_t> ids(kVectorSize);
    constexpr int64_t kTotal = 150000;
    for (int64_t base = 0; base < kTotal; base += kVectorSize) {
        const idx_t n = std::min<int64_t>(kVectorSize, kTotal - base);
        keys.Reset();
        for (idx_t i = 0; i < n; i++) {
            keys.SetValue(0, i, Value::BigInt(base + static_cast<int64_t>(i)));
            keys.SetValue(1, i, Value::Varchar("key-" + std::to_string((base + i) % 977)));
        }
        keys.SetCardinality(n);
        ASSERT_EQ(index.FindOrInsert(keys, n, ids.data()), n);
        for (idx_t i = 0; i < n; i++) {
            ASSERT_EQ(ids[i], base + i);
        }
    }
    EXPECT_EQ(index.Count(), static_cast<idx_t>(kTotal));
    // second pass: all found, same ids
    for (int64_t base = 0; base < kTotal; base += kVectorSize) {
        const idx_t n = std::min<int64_t>(kVectorSize, kTotal - base);
        keys.Reset();
        for (idx_t i = 0; i < n; i++) {
            keys.SetValue(0, i, Value::BigInt(base + static_cast<int64_t>(i)));
            keys.SetValue(1, i, Value::Varchar("key-" + std::to_string((base + i) % 977)));
        }
        keys.SetCardinality(n);
        ASSERT_EQ(index.FindOrInsert(keys, n, ids.data()), 0U);
        for (idx_t i = 0; i < n; i++) {
            ASSERT_EQ(ids[i], base + i);
        }
    }
}

TEST(KeyIndex, NullsAreOneGroupAndDoubleZerosAndNaNsMerge) {
    KeyIndex index({LogicalType::Double()});
    DataChunk keys;
    keys.Initialize({LogicalType::Double()});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<std::optional<double>> in = {0.0,  -0.0, nan,         std::nullopt,
                                                   -nan, 1.0,  std::nullopt};
    for (idx_t i = 0; i < in.size(); i++) {
        keys.SetValue(0, i, in[i] ? Value::Double(*in[i]) : Value::Null(LogicalType::Double()));
    }
    keys.SetCardinality(in.size());
    std::vector<uint32_t> ids(in.size());
    EXPECT_EQ(index.FindOrInsert(keys, in.size(), ids.data()), 4U);
    EXPECT_EQ(ids, (std::vector<uint32_t>{0, 0, 1, 2, 1, 3, 2}));
}

TEST(KeyIndex, ZeroKeyColumnsIsASingleGroup) {
    KeyIndex index({});
    DataChunk none;
    none.Initialize({});
    std::vector<uint32_t> ids(5, 9);
    EXPECT_EQ(index.FindOrInsert(none, 5, ids.data()), 1U);
    EXPECT_EQ(ids, (std::vector<uint32_t>(5, 0)));
    EXPECT_EQ(index.FindOrInsert(none, 3, ids.data()), 0U);
    EXPECT_EQ(index.Count(), 1U);
    EXPECT_EQ(index.FindOrInsert(none, 0, ids.data()), 0U);
}

// ---------------------------------------------------------------------------------- aggregates

namespace {

struct Reference {
    // key tuple -> row indexes, in input order
    std::map<std::vector<Value>, std::vector<size_t>, TupleLess> groups;
};

// One aggregate evaluated naively over the argument values of one group.
Value ReferenceAggregate(const AggregateSpec& spec, const std::vector<Value>& args) {
    std::vector<Value> vals;
    for (const Value& v : args) {
        if (spec.kind == AggregateKind::CountStar || !v.IsNull()) {
            vals.push_back(v);
        }
    }
    if (spec.distinct && spec.kind != AggregateKind::CountStar) {
        std::vector<Value> uniq;
        for (const Value& v : vals) {
            const bool seen = std::any_of(uniq.begin(), uniq.end(), [&](const Value& u) {
                return test::CompareWithNulls(u, v) == 0;
            });
            if (!seen) {
                uniq.push_back(v);
            }
        }
        vals = std::move(uniq);
    }
    const LogicalType result = AggregateResultType(spec);
    switch (spec.kind) {
    case AggregateKind::CountStar:
    case AggregateKind::Count:
        return Value::BigInt(static_cast<int64_t>(vals.size()));
    case AggregateKind::Sum:
    case AggregateKind::Avg: {
        if (vals.empty()) {
            return Value::Null(result);
        }
        if (spec.kind == AggregateKind::Sum && result.id() == TypeId::BigInt) {
            int64_t sum = 0;
            for (const Value& v : vals) {
                sum += v.type().id() == TypeId::Integer ? v.GetInteger() : v.GetBigInt();
            }
            return Value::BigInt(sum);
        }
        double sum = 0;
        for (const Value& v : vals) {
            sum += v.type().id() == TypeId::Double
                       ? v.GetDouble()
                       : (v.type().id() == TypeId::Integer ? v.GetInteger()
                                                           : static_cast<double>(v.GetBigInt()));
        }
        return Value::Double(
            spec.kind == AggregateKind::Sum ? sum : sum / static_cast<double>(vals.size()));
    }
    case AggregateKind::Min:
    case AggregateKind::Max: {
        if (vals.empty()) {
            return Value::Null(result);
        }
        Value best = vals[0];
        for (const Value& v : vals) {
            const int c = Value::Compare(v, best);
            if (spec.kind == AggregateKind::Min ? c < 0 : c > 0) {
                best = v;
            }
        }
        return best;
    }
    }
    return Value::Null(result);
}

struct AggInput {
    std::vector<LogicalType> group_types;
    std::vector<AggregateSpec> specs;
    std::vector<DataChunk> chunks;        // [group columns..., argument columns...]
    std::vector<std::vector<Value>> rows; // all rows, row-major, same column layout
};

// Argument i of the aggregate list lives in column group_types.size() + i.
AggInput MakeInput(Rng& rng, const std::vector<LogicalType>& group_types,
                   const std::vector<AggregateSpec>& specs, int chunks, double nulls = 0.15,
                   const test::ValueGen& gen_override = nullptr) {
    AggInput in;
    in.group_types = group_types;
    in.specs = specs;
    std::vector<LogicalType> col_types = group_types;
    for (const AggregateSpec& s : specs) {
        col_types.push_back(s.arg_type);
    }
    const test::ValueGen gen = gen_override ? gen_override : SmallGen(nulls);
    for (int k = 0; k < chunks; k++) {
        const idx_t n = Chance(rng, 0.2) ? kVectorSize : RandBelow(rng, 400);
        in.chunks.push_back(test::RandomVariedChunk(rng, col_types, n, gen));
        for (auto& r : RowsOf(in.chunks.back())) {
            in.rows.push_back(std::move(r));
        }
    }
    return in;
}

void SinkChunk(GroupTable& table, const AggInput& in, const DataChunk& chunk) {
    DataChunk keys;
    keys.Initialize(in.group_types);
    for (idx_t c = 0; c < in.group_types.size(); c++) {
        keys.column(c).Reference(chunk.column(c));
    }
    keys.SetCardinality(chunk.size());
    std::vector<const Vector*> args;
    for (size_t a = 0; a < in.specs.size(); a++) {
        args.push_back(in.specs[a].kind == AggregateKind::CountStar
                           ? nullptr
                           : &chunk.column(in.group_types.size() + a));
    }
    table.Sink(keys, args, chunk.size());
}

// Reads every group of the table into key -> aggregate values.
std::map<std::vector<Value>, std::vector<Value>, TupleLess> ReadAll(const GroupTable& table) {
    std::map<std::vector<Value>, std::vector<Value>, TupleLess> out;
    DataChunk chunk;
    chunk.Initialize(table.output_types());
    for (idx_t first = 0; first < table.GroupCount(); first += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, table.GroupCount() - first);
        table.Scan(first, n, chunk);
        for (auto& row : RowsOf(chunk)) {
            std::vector<Value> key(row.begin(), row.begin() + table.GroupColumnCount());
            std::vector<Value> aggs(row.begin() + table.GroupColumnCount(), row.end());
            EXPECT_TRUE(out.emplace(std::move(key), std::move(aggs)).second) << "duplicate group";
        }
    }
    return out;
}

void ExpectMatchesReference(const AggInput& in, const GroupTable& table, const char* what) {
    Reference ref;
    for (size_t r = 0; r < in.rows.size(); r++) {
        std::vector<Value> key(in.rows[r].begin(), in.rows[r].begin() + in.group_types.size());
        ref.groups[key].push_back(r);
    }
    const auto got = ReadAll(table);
    if (in.group_types.empty()) {
        ASSERT_EQ(got.size(), 1U) << what;
    } else {
        ASSERT_EQ(got.size(), ref.groups.size()) << what;
    }
    for (const auto& [key, rows] : ref.groups) {
        const auto it = got.find(key);
        ASSERT_NE(it, got.end()) << what;
        for (size_t a = 0; a < in.specs.size(); a++) {
            std::vector<Value> args;
            for (const size_t r : rows) {
                args.push_back(in.specs[a].kind == AggregateKind::CountStar
                                   ? Value::Integer(0)
                                   : in.rows[r][in.group_types.size() + a]);
            }
            const Value want = ReferenceAggregate(in.specs[a], args);
            ASSERT_TRUE(SameValue(it->second[a], want))
                << what << ": aggregate " << a << " (" << AggregateName(in.specs[a].kind)
                << (in.specs[a].distinct ? " DISTINCT" : "") << ") got " << it->second[a].ToString()
                << " want " << want.ToString();
        }
    }
}

std::vector<AggregateSpec> AllSpecsFor(LogicalType t) {
    std::vector<AggregateSpec> out;
    for (const bool distinct : {false, true}) {
        out.push_back({AggregateKind::Count, t, distinct});
        out.push_back({AggregateKind::Min, t, distinct});
        out.push_back({AggregateKind::Max, t, distinct});
        if (t.IsNumeric()) {
            out.push_back({AggregateKind::Sum, t, distinct});
            out.push_back({AggregateKind::Avg, t, distinct});
        }
    }
    return out;
}

} // namespace

TEST(GroupTable, EveryAggregateOnEveryTypeMatchesTheReference) {
    Rng rng(5);
    for (const LogicalType t : kAllTypes) {
        std::vector<AggregateSpec> specs = AllSpecsFor(t);
        specs.push_back({AggregateKind::CountStar, LogicalType::Integer(), false});
        for (const bool grouped : {true, false}) {
            const std::vector<LogicalType> groups =
                grouped ? std::vector<LogicalType>{LogicalType::Integer(), LogicalType::Varchar()}
                        : std::vector<LogicalType>{};
            AggInput in = MakeInput(rng, groups, specs, 6);
            // sums of random doubles are order dependent only across merges; here it is one table
            GroupTable table(in.group_types, in.specs);
            for (const DataChunk& c : in.chunks) {
                SinkChunk(table, in, c);
            }
            ExpectMatchesReference(in, table,
                                   (t.ToString() + (grouped ? " grouped" : " global")).c_str());
        }
    }
}

TEST(GroupTable, GlobalAggregateOverEmptyInputYieldsOneRow) {
    GroupTable table({}, {{AggregateKind::CountStar, LogicalType::Integer(), false},
                          {AggregateKind::Count, LogicalType::Integer(), false},
                          {AggregateKind::Sum, LogicalType::BigInt(), false},
                          {AggregateKind::Avg, LogicalType::Double(), false},
                          {AggregateKind::Min, LogicalType::Varchar(), false},
                          {AggregateKind::Max, LogicalType::Date(), false}});
    ASSERT_EQ(table.GroupCount(), 1U);
    const auto got = ReadAll(table);
    const auto& v = got.begin()->second;
    EXPECT_EQ(v[0], Value::BigInt(0));
    EXPECT_EQ(v[1], Value::BigInt(0));
    for (size_t i = 2; i < v.size(); i++) {
        EXPECT_TRUE(v[i].IsNull()) << i;
    }
}

TEST(GroupTable, GroupedAggregateOverEmptyInputYieldsNoRows) {
    GroupTable table({LogicalType::Integer()},
                     {{AggregateKind::CountStar, LogicalType::Integer(), false}});
    DataChunk keys;
    keys.Initialize({LogicalType::Integer()});
    table.Sink(keys, {nullptr}, 0);
    EXPECT_EQ(table.GroupCount(), 0U);
}

TEST(GroupTable, AllNullArgumentsGiveNullOrZero) {
    GroupTable table({LogicalType::Integer()},
                     {{AggregateKind::Count, LogicalType::Integer(), false},
                      {AggregateKind::Sum, LogicalType::Integer(), false},
                      {AggregateKind::Min, LogicalType::Integer(), false},
                      {AggregateKind::CountStar, LogicalType::Integer(), false}});
    DataChunk in;
    in.Initialize({LogicalType::Integer(), LogicalType::Integer()});
    for (idx_t i = 0; i < 5; i++) {
        in.SetValue(0, i, Value::Integer(1));
        in.SetValue(1, i, Value::Null(LogicalType::Integer()));
    }
    in.SetCardinality(5);
    DataChunk keys;
    keys.Initialize({LogicalType::Integer()});
    keys.column(0).Reference(in.column(0));
    keys.SetCardinality(5);
    table.Sink(keys, {&in.column(1), &in.column(1), &in.column(1), nullptr}, 5);
    const auto got = ReadAll(table);
    ASSERT_EQ(got.size(), 1U);
    const auto& v = got.begin()->second;
    EXPECT_EQ(v[0], Value::BigInt(0));
    EXPECT_TRUE(v[1].IsNull());
    EXPECT_TRUE(v[2].IsNull());
    EXPECT_EQ(v[3], Value::BigInt(5));
}

TEST(GroupTable, MergingPartialTablesEqualsOneTable) {
    // Integer-valued data keeps floating-point sums exact, so the merged result must be identical.
    Rng rng(6);
    const test::ValueGen exact = [](Rng& rng_, LogicalType t) {
        if (t.id() == TypeId::Double) {
            return Chance(rng_, 0.15) ? Value::Null(t)
                                      : Value::Double(static_cast<double>(RandBelow(rng_, 9)) - 4);
        }
        return SmallDomainValue(rng_, t);
    };
    for (int round = 0; round < 25; round++) {
        const LogicalType arg = kAllTypes[RandBelow(rng, kAllTypes.size())];
        std::vector<AggregateSpec> specs = AllSpecsFor(arg);
        specs.push_back({AggregateKind::CountStar, LogicalType::Integer(), false});
        const bool grouped = Chance(rng, 0.8);
        const std::vector<LogicalType> groups =
            grouped ? std::vector<LogicalType>{kAllTypes[RandBelow(rng, kAllTypes.size())]}
                    : std::vector<LogicalType>{};
        AggInput in = MakeInput(rng, groups, specs, 8, 0.15, exact);
        const size_t parts = 1 + RandBelow(rng, 4);
        std::vector<std::unique_ptr<GroupTable>> tables;
        for (size_t p = 0; p < parts; p++) {
            tables.push_back(std::make_unique<GroupTable>(in.group_types, in.specs));
        }
        for (const DataChunk& c : in.chunks) {
            SinkChunk(*tables[RandBelow(rng, parts)], in, c);
        }
        GroupTable merged(in.group_types, in.specs);
        for (auto& t : tables) {
            merged.Combine(*t);
        }
        ExpectMatchesReference(in, merged, "merged");
    }
}

TEST(GroupTable, CombineOfEmptyTablesAndIntoEmpty) {
    GroupTable a({LogicalType::Integer()},
                 {{AggregateKind::CountStar, LogicalType::Integer(), false}});
    GroupTable b({LogicalType::Integer()},
                 {{AggregateKind::CountStar, LogicalType::Integer(), false}});
    a.Combine(b);
    EXPECT_EQ(a.GroupCount(), 0U);
    Rng rng(7);
    AggInput in = MakeInput(rng, {LogicalType::Integer()},
                            {{AggregateKind::CountStar, LogicalType::Integer(), false}}, 3);
    for (const DataChunk& c : in.chunks) {
        SinkChunk(b, in, c);
    }
    a.Combine(b);
    ExpectMatchesReference(in, a, "combined into empty");
    a.Combine(GroupTable({LogicalType::Integer()},
                         {{AggregateKind::CountStar, LogicalType::Integer(), false}}));
    ExpectMatchesReference(in, a, "combined with empty");
}

TEST(GroupTable, SumOverflowIsAnErrorNotAWrap) {
    const AggregateSpec sum{AggregateKind::Sum, LogicalType::BigInt(), false};
    auto chunk_of = [](std::vector<int64_t> values) {
        DataChunk c;
        c.Initialize({LogicalType::BigInt()});
        for (idx_t i = 0; i < values.size(); i++) {
            c.SetValue(0, i, Value::BigInt(values[i]));
        }
        c.SetCardinality(values.size());
        return c;
    };
    DataChunk none;
    none.Initialize({});
    {
        GroupTable t({}, {sum});
        const DataChunk c = chunk_of({std::numeric_limits<int64_t>::max(), 1});
        EXPECT_THROW(t.Sink(none, {&c.column(0)}, 2), Error);
    }
    {
        GroupTable t({}, {sum});
        const DataChunk c = chunk_of({std::numeric_limits<int64_t>::min(), -1});
        EXPECT_THROW(t.Sink(none, {&c.column(0)}, 2), Error);
    }
    {
        // exactly at the limit is fine, one more is not (here via Combine)
        GroupTable a({}, {sum}), b({}, {sum});
        const DataChunk big = chunk_of({std::numeric_limits<int64_t>::max()});
        const DataChunk one = chunk_of({1});
        a.Sink(none, {&big.column(0)}, 1);
        b.Sink(none, {&one.column(0)}, 1);
        EXPECT_THROW(a.Combine(b), Error);
    }
    {
        GroupTable t({}, {sum});
        const DataChunk c = chunk_of({std::numeric_limits<int64_t>::max(), -5, 5});
        EXPECT_NO_THROW(t.Sink(none, {&c.column(0)}, 3));
        EXPECT_EQ(ReadAll(t).begin()->second[0],
                  Value::BigInt(std::numeric_limits<int64_t>::max()));
    }
}

namespace {

// Feeds `chunks` of all-valid Flat values to an aggregate without GROUP BY.
std::vector<Value> RunUngrouped(const std::vector<AggregateSpec>& specs, LogicalType type,
                                const std::vector<std::vector<Value>>& chunks) {
    GroupTable table({}, specs);
    DataChunk none;
    none.Initialize({});
    for (const auto& values : chunks) {
        DataChunk in;
        in.Initialize({type});
        for (idx_t i = 0; i < values.size(); i++) {
            in.SetValue(0, i, values[i]);
        }
        in.SetCardinality(values.size());
        std::vector<const Vector*> args(specs.size(), &in.column(0));
        table.Sink(none, args, values.size());
    }
    return ReadAll(table).begin()->second;
}

} // namespace

TEST(GroupTable, UngroupedAggregatesOverFlatAllValidInputMatchSequentialSemantics) {
    // These inputs take the vectorised kernels (Flat, no NULLs); the answers must be the ones a
    // plain row-at-a-time loop gives, with SIMD on and off.
    Rng rng(31);
    for (const bool simd : {true, false}) {
        const test::ScopedSimd mode(simd);
        for (const LogicalType type :
             {LogicalType::Integer(), LogicalType::BigInt(), LogicalType::Date()}) {
            for (int round = 0; round < 30; round++) {
                std::vector<std::vector<Value>> chunks;
                std::vector<Value> all;
                const size_t nchunks = 1 + RandBelow(rng, 5);
                for (size_t c = 0; c < nchunks; c++) {
                    std::vector<Value> values;
                    const idx_t n =
                        std::vector<idx_t>{1, 7, 8, 9, 100, 2047, 2048}[RandBelow(rng, 7)];
                    for (idx_t i = 0; i < n; i++) {
                        const int64_t v =
                            round % 3 == 0
                                ? static_cast<int64_t>(RandBelow(rng, 1000000)) - 500000
                                : static_cast<int64_t>(rng()) >> (16 + RandBelow(rng, 16));
                        values.push_back(type.id() == TypeId::BigInt ? Value::BigInt(v)
                                         : type.id() == TypeId::Integer
                                             ? Value::Integer(static_cast<int32_t>(v))
                                             : Value::Date(date_t{static_cast<int32_t>(v)}));
                        all.push_back(values.back());
                    }
                    chunks.push_back(std::move(values));
                }
                std::vector<AggregateSpec> specs = {{AggregateKind::Min, type, false},
                                                    {AggregateKind::Max, type, false}};
                if (type.id() != TypeId::Date) {
                    specs.push_back({AggregateKind::Sum, type, false});
                }
                const std::vector<Value> got = RunUngrouped(specs, type, chunks);
                Value lo = all[0], hi = all[0];
                int64_t sum = 0;
                for (const Value& v : all) {
                    lo = Value::Compare(v, lo) < 0 ? v : lo;
                    hi = Value::Compare(v, hi) > 0 ? v : hi;
                    if (type.id() == TypeId::Integer) {
                        sum += v.GetInteger();
                    } else if (type.id() == TypeId::BigInt) {
                        sum += v.GetBigInt();
                    }
                }
                ASSERT_EQ(got[0], lo) << type.ToString() << (simd ? " simd" : " scalar");
                ASSERT_EQ(got[1], hi);
                if (type.id() != TypeId::Date) {
                    ASSERT_EQ(got[2], Value::BigInt(sum));
                }
            }
        }
    }
}

TEST(GroupTable, UngroupedSumOverflowFollowsTheSequentialRuleWithAndWithoutSimd) {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    const AggregateSpec sum{AggregateKind::Sum, LogicalType::BigInt(), false};
    for (const bool simd : {true, false}) {
        const test::ScopedSimd mode(simd);
        const auto run = [&](std::vector<int64_t> values) {
            std::vector<Value> v;
            for (const int64_t x : values) {
                v.push_back(Value::BigInt(x));
            }
            return RunUngrouped({sum}, LogicalType::BigInt(), {v});
        };
        // the running sum overflows at the second value even though the total would fit again
        EXPECT_THROW(run({kMax, 1, -1}), Error) << (simd ? "simd" : "scalar");
        EXPECT_THROW(run({kMax, kMax}), Error);
        EXPECT_THROW(run({std::numeric_limits<int64_t>::min(), -1}), Error);
        // exactly at the limit is fine
        EXPECT_EQ(run({kMax - 1, 1})[0], Value::BigInt(kMax));
        EXPECT_EQ(run({kMax, -kMax, kMax / 2})[0], Value::BigInt(kMax / 2));
        // across chunks: the second chunk starts from the first one's total
        std::vector<Value> first = {Value::BigInt(kMax - 5)},
                           second = {Value::BigInt(10), Value::BigInt(-10)};
        EXPECT_THROW(RunUngrouped({sum}, LogicalType::BigInt(), {first, second}), Error);
    }
}

namespace {
// Cuts a long column into chunks of at most one vector.
std::vector<std::vector<Value>> Split(const std::vector<Value>& values) {
    std::vector<std::vector<Value>> out;
    for (size_t at = 0; at < values.size(); at += kVectorSize) {
        out.emplace_back(values.begin() + static_cast<long>(at),
                         values.begin() +
                             static_cast<long>(std::min<size_t>(values.size(), at + kVectorSize)));
    }
    return out;
}
} // namespace

TEST(GroupTable, UngroupedDoubleSumIsCloseToSequentialAndExactForRepresentableValues) {
    Rng rng(32);
    for (const bool simd : {true, false}) {
        const test::ScopedSimd mode(simd);
        std::vector<Value> values;
        double sequential = 0, exact_sum = 0;
        std::vector<Value> exact_values;
        for (int i = 0; i < 5000; i++) {
            const double x =
                static_cast<double>(static_cast<int64_t>(RandBelow(rng, 2000000)) - 1000000) /
                100.0;
            values.push_back(Value::Double(x));
            sequential += x;
            const double e = static_cast<double>(RandBelow(rng, 100)) * 0.25;
            exact_values.push_back(Value::Double(e));
            exact_sum += e;
        }
        const AggregateSpec sum{AggregateKind::Sum, LogicalType::Double(), false};
        const double got = RunUngrouped({sum}, LogicalType::Double(), Split(values))[0].GetDouble();
        EXPECT_NEAR(got, sequential, 1e-6);
        EXPECT_EQ(RunUngrouped({sum}, LogicalType::Double(), Split(exact_values))[0],
                  Value::Double(exact_sum));
    }
}

TEST(GroupTable, MinMaxFollowTheTotalOrderForDoubles) {
    GroupTable t({}, {{AggregateKind::Min, LogicalType::Double(), false},
                      {AggregateKind::Max, LogicalType::Double(), false}});
    DataChunk c;
    c.Initialize({LogicalType::Double()});
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double vals[] = {3.0, nan, -1.0, std::numeric_limits<double>::infinity()};
    for (idx_t i = 0; i < 4; i++) {
        c.SetValue(0, i, Value::Double(vals[i]));
    }
    c.SetCardinality(4);
    DataChunk none;
    none.Initialize({});
    t.Sink(none, {&c.column(0), &c.column(0)}, 4);
    const auto v = ReadAll(t).begin()->second;
    EXPECT_EQ(v[0], Value::Double(-1.0));
    EXPECT_TRUE(std::isnan(v[1].GetDouble())) << "NaN is the largest value";
}

TEST(GroupTable, StringMinMaxAreBytewiseAndOwnTheirBytes) {
    GroupTable t({LogicalType::Integer()}, {{AggregateKind::Min, LogicalType::Varchar(), false},
                                            {AggregateKind::Max, LogicalType::Varchar(), false}});
    std::vector<std::string> inputs = {"banana split with extra", "apple pie with extra cream",
                                       "\xC3\xA4pfel", "Zebra", "apple"};
    {
        DataChunk in;
        in.Initialize({LogicalType::Integer(), LogicalType::Varchar()});
        for (idx_t i = 0; i < inputs.size(); i++) {
            in.SetValue(0, i, Value::Integer(1));
            in.SetValue(1, i, Value::Varchar(inputs[i]));
        }
        in.SetCardinality(inputs.size());
        DataChunk keys;
        keys.Initialize({LogicalType::Integer()});
        keys.column(0).Reference(in.column(0));
        keys.SetCardinality(inputs.size());
        t.Sink(keys, {&in.column(1), &in.column(1)}, inputs.size());
    } // input gone: the table must have copied what it keeps
    const auto v = ReadAll(t).begin()->second;
    EXPECT_EQ(v[0], Value::Varchar("Zebra"));        // uppercase sorts before lowercase
    EXPECT_EQ(v[1], Value::Varchar("\xC3\xA4pfel")); // bytes >= 0x80 sort last (unsigned)
}

TEST(GroupTable, ManyGroupsAcrossManyChunks) {
    Rng rng(8);
    GroupTable table({LogicalType::BigInt()},
                     {{AggregateKind::CountStar, LogicalType::Integer(), false},
                      {AggregateKind::Sum, LogicalType::BigInt(), false}});
    std::map<int64_t, std::pair<int64_t, int64_t>> model;
    DataChunk in;
    in.Initialize({LogicalType::BigInt(), LogicalType::BigInt()});
    DataChunk keys;
    keys.Initialize({LogicalType::BigInt()});
    for (int chunk = 0; chunk < 100; chunk++) {
        in.Reset();
        for (idx_t i = 0; i < kVectorSize; i++) {
            const int64_t k = static_cast<int64_t>(RandBelow(rng, 60000));
            const int64_t v = static_cast<int64_t>(RandBelow(rng, 1000));
            in.SetValue(0, i, Value::BigInt(k));
            in.SetValue(1, i, Value::BigInt(v));
            model[k].first++;
            model[k].second += v;
        }
        in.SetCardinality(kVectorSize);
        keys.column(0).Reference(in.column(0));
        keys.SetCardinality(kVectorSize);
        table.Sink(keys, {nullptr, &in.column(1)}, kVectorSize);
    }
    const auto got = ReadAll(table);
    ASSERT_EQ(got.size(), model.size());
    for (const auto& [k, cs] : model) {
        const auto& v = got.at({Value::BigInt(k)});
        ASSERT_EQ(v[0], Value::BigInt(cs.first));
        ASSERT_EQ(v[1], Value::BigInt(cs.second));
    }
}

TEST(GroupTable, ScanReturnsGroupsInFirstAppearanceOrder) {
    GroupTable t({LogicalType::Varchar()},
                 {{AggregateKind::CountStar, LogicalType::Integer(), false}});
    DataChunk keys;
    keys.Initialize({LogicalType::Varchar()});
    const char* order[] = {"zeta", "alpha", "zeta", "mid", "alpha", "omega"};
    for (idx_t i = 0; i < 6; i++) {
        keys.SetValue(0, i, Value::Varchar(order[i]));
    }
    keys.SetCardinality(6);
    t.Sink(keys, {nullptr}, 6);
    DataChunk out;
    out.Initialize(t.output_types());
    t.Scan(0, t.GroupCount(), out);
    ASSERT_EQ(out.size(), 4U);
    EXPECT_EQ(out.GetValue(0, 0), Value::Varchar("zeta"));
    EXPECT_EQ(out.GetValue(0, 1), Value::Varchar("alpha"));
    EXPECT_EQ(out.GetValue(0, 2), Value::Varchar("mid"));
    EXPECT_EQ(out.GetValue(0, 3), Value::Varchar("omega"));
    EXPECT_EQ(out.GetValue(1, 0), Value::BigInt(2));
    // a window in the middle
    t.Scan(1, 2, out);
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out.GetValue(0, 0), Value::Varchar("alpha"));
    EXPECT_EQ(out.GetValue(0, 1), Value::Varchar("mid"));
}

} // namespace cdb
