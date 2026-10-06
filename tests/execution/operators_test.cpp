// Operator-level tests: each physical operator is wired into a hand-built pipeline and compared
// with a naive reference implementation over random data in every vector format.

#include "execution/basic_operators.h"
#include "execution/hash_aggregate.h"
#include "execution/hash_join.h"
#include "execution/pipeline.h"
#include "execution/sort.h"
#include "planner/scalar_eval.h"

#include "exec_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <tuple>

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
using Rows = std::vector<std::vector<Value>>;

// A source that replays prepared chunks (test only).
class ChunkListSource final : public PhysicalOperator {
  public:
    ChunkListSource(std::vector<LogicalType> types, const std::vector<DataChunk>* chunks)
        : PhysicalOperator(std::move(types)), chunks_(chunks) {}
    std::string Name() const override { return "CHUNK_LIST"; }

    struct State final : GlobalSourceState {
        size_t next = 0;
    };
    struct Local final : LocalSourceState {};
    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override {
        return std::make_unique<State>();
    }
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override {
        return std::make_unique<Local>();
    }
    bool GetData(GlobalSourceState& g, LocalSourceState&, DataChunk& out) override {
        auto& s = static_cast<State&>(g);
        if (s.next >= chunks_->size()) {
            return false;
        }
        const DataChunk& in = (*chunks_)[s.next++];
        for (idx_t c = 0; c < in.ColumnCount(); c++) {
            out.column(c).Reference(in.column(c));
        }
        out.SetCardinality(in.size());
        return true;
    }

  private:
    const std::vector<DataChunk>* chunks_;
};

Rows Collect(PhysicalPlan& plan) {
    Executor ex(plan);
    ex.Run();
    Rows rows;
    for (const DataChunk& c : PhysicalResultCollector::TakeChunks(*ex.SinkState(*plan.root))) {
        c.Verify();
        for (auto& r : RowsOf(c)) {
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

struct Data {
    std::vector<DataChunk> chunks;
    Rows rows;
};

Data MakeData(Rng& rng, const std::vector<LogicalType>& types, int nchunks,
              const test::ValueGen& gen, bool allow_big = true) {
    Data d;
    for (int k = 0; k < nchunks; k++) {
        idx_t n;
        switch (RandBelow(rng, 6)) {
        case 0:
            n = 0;
            break;
        case 1:
            n = 1;
            break;
        case 2:
            n = allow_big ? kVectorSize : 100;
            break;
        default:
            n = 1 + RandBelow(rng, 300);
            break;
        }
        d.chunks.push_back(test::RandomVariedChunk(rng, types, n, gen));
        for (auto& r : RowsOf(d.chunks.back())) {
            d.rows.push_back(std::move(r));
        }
    }
    return d;
}

test::ValueGen Gen(double nulls = 0.15) {
    return [nulls](Rng& rng, LogicalType t) { return SmallDomainValue(rng, t, nulls); };
}

void ExpectSameMultiset(Rows got, Rows want, const std::string& what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    std::sort(got.begin(), got.end(), TupleLess());
    std::sort(want.begin(), want.end(), TupleLess());
    for (size_t i = 0; i < got.size(); i++) {
        for (size_t c = 0; c < got[i].size(); c++) {
            ASSERT_TRUE(test::CompareWithNulls(got[i][c], want[i][c]) == 0)
                << what << ": row " << i << " col " << c << ": " << got[i][c].ToString() << " vs "
                << want[i][c].ToString();
        }
    }
}

BoundExprPtr Col(idx_t i, LogicalType t) {
    return BoundExpr::ColumnRef(i, t);
}
BoundExprPtr Cmp(OperatorKind op, BoundExprPtr a, BoundExprPtr b) {
    return BoundExpr::Binary(op, std::move(a), std::move(b), LogicalType::Boolean());
}

// Single pipeline: ChunkListSource -> ops -> ResultCollector.
struct Simple {
    PhysicalPlan plan;
    PhysicalOperator* source = nullptr;
    PhysicalResultCollector* sink = nullptr;
    Pipeline pipeline;

    Simple(const std::vector<LogicalType>& in_types, const std::vector<DataChunk>* chunks) {
        source = &plan.Make<ChunkListSource>(in_types, chunks);
        pipeline.source = source;
    }
    template <class T, class... A> T& Add(A&&... args) {
        T& op = plan.Make<T>(std::forward<A>(args)...);
        pipeline.operators.push_back(&op);
        return op;
    }
    Rows Run(const std::vector<LogicalType>& out_types) {
        sink = &plan.Make<PhysicalResultCollector>(out_types);
        pipeline.sink = sink;
        plan.root = sink;
        plan.pipelines.push_back(pipeline);
        return Collect(plan);
    }
};

const std::vector<LogicalType> kTypes = {LogicalType::Integer(), LogicalType::Varchar(),
                                         LogicalType::Double(), LogicalType::Date()};

} // namespace

// ---------------------------------------------------------------------------------- filter /
// project

TEST(Operators, FilterKeepsExactlyTheTrueRowsInOrder) {
    Rng rng(1);
    for (int round = 0; round < 40; round++) {
        Data d = MakeData(rng, kTypes, 5, Gen());
        Simple s(kTypes, &d.chunks);
        // col0 > 0 AND col1 IS NOT NULL
        auto pred = BoundExpr::Binary(
            OperatorKind::And,
            Cmp(OperatorKind::Gt, Col(0, kTypes[0]), BoundExpr::Constant(Value::Integer(0))),
            BoundExpr::IsNull(Col(1, kTypes[1]), true), LogicalType::Boolean());
        s.Add<PhysicalFilter>(kTypes, pred->Clone());
        const Rows got = s.Run(kTypes);
        Rows want;
        for (const auto& r : d.rows) {
            if (!r[0].IsNull() && r[0].GetInteger() > 0 && !r[1].IsNull()) {
                want.push_back(r);
            }
        }
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); i++) { // order preserved
            ASSERT_EQ(CompareTuples(got[i], want[i]), 0) << i;
        }
    }
}

TEST(Operators, FilterThatKeepsEverythingOrNothing) {
    Rng rng(2);
    Data d = MakeData(rng, kTypes, 4, Gen(0.0));
    {
        Simple s(kTypes, &d.chunks);
        s.Add<PhysicalFilter>(kTypes, BoundExpr::Constant(Value::Boolean(true)));
        EXPECT_EQ(s.Run(kTypes).size(), d.rows.size());
    }
    {
        Simple s(kTypes, &d.chunks);
        s.Add<PhysicalFilter>(kTypes, BoundExpr::Constant(Value::Boolean(false)));
        EXPECT_TRUE(s.Run(kTypes).empty());
    }
    {
        Simple s(kTypes, &d.chunks);
        s.Add<PhysicalFilter>(kTypes, BoundExpr::Constant(Value::Null(LogicalType::Boolean())));
        EXPECT_TRUE(s.Run(kTypes).empty());
    }
}

TEST(Operators, ProjectionComputesExpressionsAndReordersColumns) {
    Rng rng(3);
    for (int round = 0; round < 20; round++) {
        Data d = MakeData(rng, kTypes, 4, Gen());
        Simple s(kTypes, &d.chunks);
        std::vector<BoundExprPtr> exprs;
        exprs.push_back(Col(3, kTypes[3]));
        exprs.push_back(BoundExpr::Constant(Value::Varchar("k")));
        exprs.push_back(Col(0, kTypes[0]));
        exprs.push_back(
            Cmp(OperatorKind::Lt, Col(0, kTypes[0]), BoundExpr::Constant(Value::Integer(0))));
        const std::vector<LogicalType> out = {kTypes[3], LogicalType::Varchar(), kTypes[0],
                                              LogicalType::Boolean()};
        s.Add<PhysicalProjection>(out, std::move(exprs));
        const Rows got = s.Run(out);
        ASSERT_EQ(got.size(), d.rows.size());
        for (size_t i = 0; i < got.size(); i++) {
            ASSERT_TRUE(SameValue(got[i][0], d.rows[i][3]));
            ASSERT_EQ(got[i][1], Value::Varchar("k"));
            ASSERT_TRUE(SameValue(got[i][2], d.rows[i][0]));
            if (d.rows[i][0].IsNull()) {
                ASSERT_TRUE(got[i][3].IsNull());
            } else {
                ASSERT_EQ(got[i][3].GetBoolean(), d.rows[i][0].GetInteger() < 0);
            }
        }
    }
}

TEST(Operators, FilterThenProjectionThenFilterChain) {
    Rng rng(4);
    Data d = MakeData(rng, kTypes, 8, Gen());
    Simple s(kTypes, &d.chunks);
    s.Add<PhysicalFilter>(
        kTypes, Cmp(OperatorKind::Ge, Col(0, kTypes[0]), BoundExpr::Constant(Value::Integer(-1))));
    std::vector<BoundExprPtr> exprs;
    exprs.push_back(Col(0, kTypes[0]));
    exprs.push_back(Col(1, kTypes[1]));
    s.Add<PhysicalProjection>(std::vector<LogicalType>{kTypes[0], kTypes[1]}, std::move(exprs));
    s.Add<PhysicalFilter>(
        std::vector<LogicalType>{kTypes[0], kTypes[1]},
        Cmp(OperatorKind::Le, Col(0, kTypes[0]), BoundExpr::Constant(Value::Integer(1))));
    const Rows got = s.Run({kTypes[0], kTypes[1]});
    Rows want;
    for (const auto& r : d.rows) {
        if (!r[0].IsNull() && r[0].GetInteger() >= -1 && r[0].GetInteger() <= 1) {
            want.push_back({r[0], r[1]});
        }
    }
    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size(); i++) {
        ASSERT_EQ(CompareTuples(got[i], want[i]), 0);
    }
}

// ---------------------------------------------------------------------------------- limit

TEST(Operators, LimitAndOffsetOverEveryChunkAlignment) {
    Rng rng(5);
    Data d = MakeData(rng, {LogicalType::Integer()}, 12, Gen(0.0));
    const std::vector<LogicalType> t = {LogicalType::Integer()};
    const int64_t total = static_cast<int64_t>(d.rows.size());
    const std::vector<std::pair<std::optional<int64_t>, int64_t>> cases = {{std::nullopt, 0},
                                                                           {0, 0},
                                                                           {1, 0},
                                                                           {5, 0},
                                                                           {total, 0},
                                                                           {total + 10, 0},
                                                                           {std::nullopt, 7},
                                                                           {3, 7},
                                                                           {10, total},
                                                                           {10, total + 5},
                                                                           {1, total - 1},
                                                                           {2048, 2047},
                                                                           {2049, 1},
                                                                           {100, 2048}};
    for (const auto& [limit, offset] : cases) {
        Simple s(t, &d.chunks);
        s.Add<PhysicalLimit>(t, limit, offset);
        const Rows got = s.Run(t);
        const int64_t begin = std::min(offset, total);
        const int64_t end = limit ? std::min(total, begin + *limit) : total;
        ASSERT_EQ(static_cast<int64_t>(got.size()), end - begin)
            << "limit " << (limit ? *limit : -1) << " offset " << offset;
        for (int64_t i = 0; i < end - begin; i++) {
            ASSERT_EQ(CompareTuples(got[i], d.rows[begin + i]), 0);
        }
    }
}

TEST(Operators, LimitSpanningSeveralSmallChunksCountsRowsAlreadyEmitted) {
    // Chunk sizes chosen so that each limit ends inside a LATER chunk: the operator must subtract
    // the rows it emitted from earlier ones (a limit that fits in the first chunk never shows it).
    const std::vector<LogicalType> t = {LogicalType::Integer()};
    Data d;
    int32_t next = 0;
    for (const idx_t n : {3, 4, 5, 2048, 7, 1}) {
        DataChunk c;
        c.Initialize(t);
        for (idx_t i = 0; i < n; i++) {
            c.SetValue(0, i, Value::Integer(next));
            d.rows.push_back({Value::Integer(next)});
            next++;
        }
        c.SetCardinality(n);
        d.chunks.push_back(std::move(c));
    }
    const int64_t total = static_cast<int64_t>(d.rows.size());
    for (const auto& [limit, offset] : std::vector<std::pair<int64_t, int64_t>>{{10, 0},
                                                                                {6, 0},
                                                                                {4, 0},
                                                                                {8, 2},
                                                                                {7, 3},
                                                                                {2060, 5},
                                                                                {3, 10},
                                                                                {13, 0},
                                                                                {12, 1},
                                                                                {2063, 0}}) {
        Simple s(t, &d.chunks);
        s.Add<PhysicalLimit>(t, limit, offset);
        const Rows got = s.Run(t);
        const int64_t begin = std::min(offset, total);
        const int64_t end = std::min(total, begin + limit);
        ASSERT_EQ(static_cast<int64_t>(got.size()), end - begin)
            << "limit " << limit << " offset " << offset;
        for (int64_t i = 0; i < end - begin; i++) {
            ASSERT_EQ(got[static_cast<size_t>(i)][0], d.rows[static_cast<size_t>(begin + i)][0]);
        }
    }
}

TEST(Operators, LimitStopsReadingTheSource) {
    // A source that counts how many chunks were requested: LIMIT 1 must not drain it.
    class Counting final : public PhysicalOperator {
      public:
        explicit Counting(int* pulled)
            : PhysicalOperator({LogicalType::Integer()}), pulled_(pulled) {}
        std::string Name() const override { return "COUNTING"; }
        struct G final : GlobalSourceState {};
        struct L final : LocalSourceState {};
        std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override {
            return std::make_unique<G>();
        }
        std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override {
            return std::make_unique<L>();
        }
        bool GetData(GlobalSourceState&, LocalSourceState&, DataChunk& out) override {
            if (*pulled_ >= 1000) {
                return false;
            }
            ++*pulled_;
            out.SetValue(0, 0, Value::Integer(1));
            out.SetCardinality(1);
            return true;
        }

      private:
        int* pulled_;
    };
    int pulled = 0;
    PhysicalPlan plan;
    auto& src = plan.Make<Counting>(&pulled);
    auto& limit = plan.Make<PhysicalLimit>(std::vector<LogicalType>{LogicalType::Integer()}, 3, 0);
    auto& sink =
        plan.Make<PhysicalResultCollector>(std::vector<LogicalType>{LogicalType::Integer()});
    Pipeline p;
    p.source = &src;
    p.operators = {&limit};
    p.sink = &sink;
    plan.pipelines.push_back(p);
    plan.root = &sink;
    EXPECT_EQ(Collect(plan).size(), 3U);
    EXPECT_EQ(pulled, 3) << "the pipeline must stop as soon as the limit is satisfied";
}

// ---------------------------------------------------------------------------------- aggregate

TEST(Operators, HashAggregateMatchesReferenceAndHandlesEmptyInput) {
    Rng rng(6);
    const std::vector<LogicalType> in = {LogicalType::Varchar(), LogicalType::Integer(),
                                         LogicalType::BigInt()};
    for (const bool grouped : {true, false}) {
        for (const int nchunks : {0, 1, 6}) {
            Data d = MakeData(rng, in, nchunks, Gen());
            std::vector<BoundExprPtr> groups;
            std::vector<LogicalType> out;
            if (grouped) {
                groups.push_back(Col(0, in[0]));
                out.push_back(in[0]);
            }
            std::vector<BoundExprPtr> aggs;
            auto agg = [&](AggregateKind k, BoundExprPtr arg, LogicalType result) {
                std::vector<BoundExprPtr> a;
                if (arg) {
                    a.push_back(std::move(arg));
                }
                aggs.push_back(BoundExpr::Aggregate(k, std::move(a), false, result));
                out.push_back(result);
            };
            agg(AggregateKind::CountStar, nullptr, LogicalType::BigInt());
            agg(AggregateKind::Count, Col(1, in[1]), LogicalType::BigInt());
            agg(AggregateKind::Sum, Col(2, in[2]), LogicalType::BigInt());
            agg(AggregateKind::Max, Col(1, in[1]), in[1]);
            Simple s(in, &d.chunks);
            PhysicalPlan& plan = s.plan;
            // aggregate is a pipeline breaker: P0 source -> aggregate (sink); P1 aggregate ->
            // result
            auto& ha = plan.Make<PhysicalHashAggregate>(out, std::move(groups), std::move(aggs));
            auto& result = plan.Make<PhysicalResultCollector>(out);
            Pipeline p0;
            p0.source = s.source;
            p0.sink = &ha;
            Pipeline p1;
            p1.source = &ha;
            p1.sink = &result;
            p1.dependencies = {0};
            plan.pipelines = {p0, p1};
            plan.root = &result;
            const Rows got = Collect(plan);

            std::map<std::vector<Value>, std::vector<size_t>, TupleLess> ref;
            for (size_t r = 0; r < d.rows.size(); r++) {
                ref[grouped ? std::vector<Value>{d.rows[r][0]} : std::vector<Value>{}].push_back(r);
            }
            if (!grouped && ref.empty()) {
                ref[{}] = {};
            }
            ASSERT_EQ(got.size(), ref.size());
            for (const auto& row : got) {
                const std::vector<Value> key =
                    grouped ? std::vector<Value>{row[0]} : std::vector<Value>{};
                const auto& members = ref.at(key);
                const size_t base = grouped ? 1 : 0;
                int64_t cnt1 = 0;
                int64_t sum = 0;
                bool any_sum = false;
                std::optional<Value> mx;
                for (const size_t r : members) {
                    const Value& v1 = d.rows[r][1];
                    const Value& v2 = d.rows[r][2];
                    if (!v1.IsNull()) {
                        cnt1++;
                        if (!mx || Value::Compare(v1, *mx) > 0) {
                            mx = v1;
                        }
                    }
                    if (!v2.IsNull()) {
                        sum += v2.GetBigInt();
                        any_sum = true;
                    }
                }
                ASSERT_EQ(row[base], Value::BigInt(static_cast<int64_t>(members.size())));
                ASSERT_EQ(row[base + 1], Value::BigInt(cnt1));
                ASSERT_TRUE(any_sum ? row[base + 2] == Value::BigInt(sum) : row[base + 2].IsNull());
                ASSERT_TRUE(mx ? row[base + 3] == *mx : row[base + 3].IsNull());
            }
        }
    }
}

TEST(Operators, DistinctIsAGroupByOverEveryColumn) {
    Rng rng(7);
    Data d = MakeData(rng, kTypes, 6, Gen());
    Simple s(kTypes, &d.chunks);
    PhysicalPlan& plan = s.plan;
    std::vector<BoundExprPtr> groups;
    for (idx_t c = 0; c < kTypes.size(); c++) {
        groups.push_back(Col(c, kTypes[c]));
    }
    auto& ha =
        plan.Make<PhysicalHashAggregate>(kTypes, std::move(groups), std::vector<BoundExprPtr>{});
    auto& result = plan.Make<PhysicalResultCollector>(kTypes);
    Pipeline p0;
    p0.source = s.source;
    p0.sink = &ha;
    Pipeline p1;
    p1.source = &ha;
    p1.sink = &result;
    p1.dependencies = {0};
    plan.pipelines = {p0, p1};
    plan.root = &result;
    const Rows got = Collect(plan);
    std::set<std::vector<Value>, TupleLess> uniq(d.rows.begin(), d.rows.end());
    Rows want(uniq.begin(), uniq.end());
    ExpectSameMultiset(got, want, "distinct");
}

// ---------------------------------------------------------------------------------- sort / top-n

namespace {

Rows ReferenceSort(Rows rows, const std::vector<SortKey>& keys,
                   const std::vector<SortSpec>& specs) {
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        for (size_t k = 0; k < keys.size(); k++) {
            const Value& x = a[keys[k].expr->ordinal];
            const Value& y = b[keys[k].expr->ordinal];
            if (x.IsNull() || y.IsNull()) {
                if (x.IsNull() && y.IsNull()) {
                    continue;
                }
                return x.IsNull() == specs[k].nulls_first;
            }
            int c = Value::Compare(x, y);
            if (specs[k].descending) {
                c = -c;
            }
            if (c != 0) {
                return c < 0;
            }
        }
        return false;
    });
    return rows;
}

std::vector<SortKey> RandomKeys(Rng& rng, std::vector<SortSpec>& specs) {
    std::vector<SortKey> keys;
    const size_t n = 1 + RandBelow(rng, 3);
    for (size_t k = 0; k < n; k++) {
        const idx_t col = RandBelow(rng, kTypes.size());
        SortKey key;
        key.expr = Col(col, kTypes[col]);
        key.descending = Chance(rng, 0.5);
        key.nulls_first = Chance(rng, 0.5);
        specs.push_back({kTypes[col], key.descending, key.nulls_first});
        keys.push_back(std::move(key));
    }
    return keys;
}

Rows RunBreaker(Data& d, const std::function<PhysicalOperator&(PhysicalPlan&)>& make) {
    Simple s(kTypes, &d.chunks);
    PhysicalOperator& op = make(s.plan);
    auto& result = s.plan.Make<PhysicalResultCollector>(kTypes);
    Pipeline p0;
    p0.source = s.source;
    p0.sink = &op;
    Pipeline p1;
    p1.source = &op;
    p1.sink = &result;
    p1.dependencies = {0};
    s.plan.pipelines = {p0, p1};
    s.plan.root = &result;
    return Collect(s.plan);
}

} // namespace

TEST(Operators, OrderByIsStableAndHonoursDirectionAndNullPlacement) {
    Rng rng(8);
    for (int round = 0; round < 40; round++) {
        Data d = MakeData(rng, kTypes, 1 + static_cast<int>(RandBelow(rng, 6)), Gen());
        std::vector<SortSpec> specs;
        std::vector<SortKey> keys = RandomKeys(rng, specs);
        std::vector<SortKey> keys_copy;
        for (const SortKey& k : keys) {
            keys_copy.push_back({k.expr->Clone(), k.descending, k.nulls_first});
        }
        const Rows got = RunBreaker(d, [&](PhysicalPlan& plan) -> PhysicalOperator& {
            return plan.Make<PhysicalOrder>(kTypes, std::move(keys_copy));
        });
        const Rows want = ReferenceSort(d.rows, keys, specs);
        ASSERT_EQ(got.size(), want.size());
        for (size_t i = 0; i < got.size(); i++) {
            for (size_t c = 0; c < kTypes.size(); c++) {
                ASSERT_EQ(test::CompareWithNulls(got[i][c], want[i][c]), 0)
                    << "round " << round << " row " << i;
            }
        }
    }
}

TEST(Operators, TopNEqualsSortThenLimitIncludingPruning) {
    Rng rng(9);
    const std::pair<int64_t, int64_t> shapes[] = {{1, 0},   {5, 0}, {5, 3},      {100, 0},
                                                  {10, 90}, {0, 0}, {3, 100000}, {1000000, 0}};
    for (int round = 0; round < 24; round++) {
        // enough rows that the buffer is pruned several times
        Data d = MakeData(rng, kTypes, 24, Gen(), /*allow_big=*/true);
        std::vector<SortSpec> specs;
        std::vector<SortKey> keys = RandomKeys(rng, specs);
        const auto [limit, offset] = shapes[round % std::size(shapes)];
        std::vector<SortKey> keys_copy;
        for (const SortKey& k : keys) {
            keys_copy.push_back({k.expr->Clone(), k.descending, k.nulls_first});
        }
        const Rows got = RunBreaker(d, [&](PhysicalPlan& plan) -> PhysicalOperator& {
            return plan.Make<PhysicalTopN>(kTypes, std::move(keys_copy), limit, offset);
        });
        Rows sorted = ReferenceSort(d.rows, keys, specs);
        const size_t begin = std::min<size_t>(static_cast<size_t>(offset), sorted.size());
        const size_t end = std::min<size_t>(sorted.size(), begin + static_cast<size_t>(limit));
        ASSERT_EQ(got.size(), end - begin)
            << "round " << round << " limit " << limit << " offset " << offset;
        for (size_t i = 0; i < got.size(); i++) {
            for (size_t c = 0; c < kTypes.size(); c++) {
                ASSERT_EQ(test::CompareWithNulls(got[i][c], sorted[begin + i][c]), 0)
                    << "round " << round << " row " << i;
            }
        }
    }
}

TEST(Operators, OrderByOnComputedKeyAndEmptyInput) {
    Rng rng(10);
    {
        Data d;
        std::vector<SortKey> keys;
        keys.push_back({Col(0, kTypes[0]), false, false});
        EXPECT_TRUE(RunBreaker(d, [&](PhysicalPlan& plan) -> PhysicalOperator& {
                        return plan.Make<PhysicalOrder>(kTypes, std::move(keys));
                    }).empty());
    }
    Data d = MakeData(rng, kTypes, 5, Gen(0.0));
    std::vector<SortKey> keys; // ORDER BY -col0 (a computed key not among the payload columns)
    keys.push_back(
        {BoundExpr::Unary(OperatorKind::Negate, Col(0, kTypes[0]), kTypes[0]), false, false});
    const Rows got = RunBreaker(d, [&](PhysicalPlan& plan) -> PhysicalOperator& {
        return plan.Make<PhysicalOrder>(kTypes, std::move(keys));
    });
    for (size_t i = 1; i < got.size(); i++) {
        ASSERT_GE(got[i - 1][0].GetInteger(), got[i][0].GetInteger()) << "descending in col0";
    }
}

// ---------------------------------------------------------------------------------- join

namespace {

struct JoinCase {
    PhysicalJoinType type;
    bool with_keys;
    bool with_residual;
};

// Reference: for each left row, the right rows that match it (SQL semantics: a NULL key never
// matches). Nested loops, computed once per case and shared by every join type.
std::vector<std::vector<size_t>> ReferenceMatches(const Rows& left, const Rows& right,
                                                  const std::vector<std::pair<idx_t, idx_t>>& keys,
                                                  const BoundExpr* residual) {
    std::vector<std::vector<size_t>> matches(left.size());
    for (size_t li = 0; li < left.size(); li++) {
        const auto& l = left[li];
        for (size_t ri = 0; ri < right.size(); ri++) {
            const auto& r = right[ri];
            bool match = true;
            for (const auto& [lk, rk] : keys) {
                if (l[lk].IsNull() || r[rk].IsNull() || Value::Compare(l[lk], r[rk]) != 0) {
                    match = false;
                    break;
                }
            }
            if (match && residual) {
                std::vector<Value> row = l;
                row.insert(row.end(), r.begin(), r.end());
                const Value v = EvaluateScalar(*residual, row);
                match = !v.IsNull() && v.GetBoolean();
            }
            if (match) {
                matches[li].push_back(ri);
            }
        }
    }
    return matches;
}

Rows DeriveJoin(const Rows& left, const Rows& right,
                const std::vector<std::vector<size_t>>& matches, PhysicalJoinType type,
                const std::vector<LogicalType>& right_types) {
    Rows out;
    for (size_t li = 0; li < left.size(); li++) {
        const bool any = !matches[li].empty();
        if (type == PhysicalJoinType::Inner || type == PhysicalJoinType::Left) {
            for (const size_t ri : matches[li]) {
                std::vector<Value> row = left[li];
                row.insert(row.end(), right[ri].begin(), right[ri].end());
                out.push_back(std::move(row));
            }
        }
        if (type == PhysicalJoinType::Left && !any) {
            std::vector<Value> row = left[li];
            for (const LogicalType& t : right_types) {
                row.push_back(Value::Null(t));
            }
            out.push_back(std::move(row));
        } else if (type == PhysicalJoinType::Semi && any) {
            out.push_back(left[li]);
        } else if (type == PhysicalJoinType::Anti && !any) {
            out.push_back(left[li]);
        }
    }
    return out;
}

Rows RunJoin(const Data& left, const Data& right, const std::vector<LogicalType>& lt,
             const std::vector<LogicalType>& rt, const std::vector<std::pair<idx_t, idx_t>>& keys,
             const BoundExpr* residual, PhysicalJoinType type) {
    PhysicalPlan plan;
    auto& lsrc = plan.Make<ChunkListSource>(lt, &left.chunks);
    auto& rsrc = plan.Make<ChunkListSource>(rt, &right.chunks);
    std::vector<LogicalType> out = lt;
    if (type == PhysicalJoinType::Inner || type == PhysicalJoinType::Left) {
        out.insert(out.end(), rt.begin(), rt.end());
    }
    std::vector<BoundExprPtr> lk, rk;
    for (const auto& [l, r] : keys) {
        lk.push_back(Col(l, lt[l]));
        rk.push_back(Col(r, rt[r]));
    }
    auto& join = plan.Make<PhysicalHashJoin>(out, type, lt, rt, std::move(lk), std::move(rk),
                                             residual ? residual->Clone() : nullptr);
    auto& result = plan.Make<PhysicalResultCollector>(out);
    Pipeline build;
    build.source = &rsrc;
    build.sink = &join;
    Pipeline probe;
    probe.source = &lsrc;
    probe.operators = {&join};
    probe.sink = &result;
    probe.dependencies = {0};
    plan.pipelines = {build, probe};
    plan.root = &result;
    return Collect(plan);
}

} // namespace

TEST(Operators, HashJoinMatchesNestedLoopReferenceForEveryJoinType) {
    Rng rng(11);
    const std::vector<LogicalType> lt = {LogicalType::Integer(), LogicalType::Varchar(),
                                         LogicalType::Double()};
    const std::vector<LogicalType> rt = {LogicalType::Varchar(), LogicalType::Integer(),
                                         LogicalType::BigInt()};
    for (int round = 0; round < 100; round++) {
        const Data left = MakeData(rng, lt, 1 + static_cast<int>(RandBelow(rng, 2)), Gen(), false);
        const Data right = MakeData(rng, rt, static_cast<int>(RandBelow(rng, 3)), Gen(), false);
        // pick 0-2 equi keys of matching types: (int, int), (varchar, varchar)
        std::vector<std::pair<idx_t, idx_t>> keys;
        const size_t shape = RandBelow(rng, 5);
        if (shape == 1 || shape == 3) {
            keys.push_back({0, 1});
        }
        if (shape == 2 || shape == 3) {
            keys.push_back({1, 0});
        }
        if (shape == 4) {
            keys.push_back({1, 0});
            keys.push_back({0, 1});
        }
        // optional residual over left ++ right: l.double < 3  OR  r.bigint IS NULL
        BoundExprPtr residual;
        if (Chance(rng, 0.4)) {
            residual = BoundExpr::Binary(
                OperatorKind::Or,
                Cmp(OperatorKind::Lt, Col(2, lt[2]), BoundExpr::Constant(Value::Double(3.0))),
                BoundExpr::IsNull(Col(3 + 2, rt[2]), false), LogicalType::Boolean());
        }
        const auto matches = ReferenceMatches(left.rows, right.rows, keys, residual.get());
        for (const PhysicalJoinType type : {PhysicalJoinType::Inner, PhysicalJoinType::Left,
                                            PhysicalJoinType::Semi, PhysicalJoinType::Anti}) {
            const Rows got = RunJoin(left, right, lt, rt, keys, residual.get(), type);
            const Rows want = DeriveJoin(left.rows, right.rows, matches, type, rt);
            ExpectSameMultiset(got, want,
                               "round " + std::to_string(round) + " type " +
                                   std::to_string(static_cast<int>(type)) + " keys " +
                                   std::to_string(keys.size()) + (residual ? " residual" : ""));
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

TEST(Operators, JoinWithManyMatchesPerProbeRowSpansSeveralOutputChunks) {
    // 10 probe rows x 1000 build rows with one key = 10,000 output rows from a single probe chunk.
    const std::vector<LogicalType> t = {LogicalType::Integer(), LogicalType::Integer()};
    Data left, right;
    DataChunk lc, rc;
    lc.Initialize(t);
    rc.Initialize(t);
    for (idx_t i = 0; i < 10; i++) {
        lc.SetValue(0, i, Value::Integer(7));
        lc.SetValue(1, i, Value::Integer(static_cast<int32_t>(i)));
    }
    lc.SetCardinality(10);
    for (idx_t i = 0; i < 1000; i++) {
        rc.SetValue(0, i, Value::Integer(7));
        rc.SetValue(1, i, Value::Integer(static_cast<int32_t>(i)));
    }
    rc.SetCardinality(1000);
    left.chunks.push_back(std::move(lc));
    right.chunks.push_back(std::move(rc));
    const Rows got = RunJoin(left, right, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Inner);
    ASSERT_EQ(got.size(), 10000U);
    // probe order, then build order within each probe row
    for (size_t i = 0; i < got.size(); i++) {
        ASSERT_EQ(got[i][1], Value::Integer(static_cast<int32_t>(i / 1000)));
        ASSERT_EQ(got[i][3], Value::Integer(static_cast<int32_t>(i % 1000)));
    }
}

TEST(Operators, JoinHandlesEmptySidesAndAllNullKeys) {
    const std::vector<LogicalType> t = {LogicalType::Integer()};
    Data some;
    DataChunk c;
    c.Initialize(t);
    for (idx_t i = 0; i < 5; i++) {
        c.SetValue(0, i, i % 2 ? Value::Null(t[0]) : Value::Integer(1));
    }
    c.SetCardinality(5);
    some.chunks.push_back(std::move(c));
    for (idx_t i = 0; i < 5; i++) {
        some.rows.push_back({i % 2 ? Value::Null(t[0]) : Value::Integer(1)});
    }
    const Data none;
    EXPECT_TRUE(RunJoin(some, none, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Inner).empty());
    EXPECT_EQ(RunJoin(some, none, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Left).size(), 5U);
    EXPECT_EQ(RunJoin(some, none, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Anti).size(), 5U);
    EXPECT_TRUE(RunJoin(some, none, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Semi).empty());
    EXPECT_TRUE(RunJoin(none, some, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Left).empty());
    // NULL = NULL is not a match: rows 1 and 3 on both sides only pair with the non-NULL key rows
    const Rows inner = RunJoin(some, some, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Inner);
    EXPECT_EQ(inner.size(), 9U) << "3 x 3 matches on key 1, none for the two NULL keys";
    const Rows left = RunJoin(some, some, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Left);
    EXPECT_EQ(left.size(), 9U + 2U) << "the two NULL-key left rows are kept, padded with NULLs";
}

TEST(Operators, JoinKeysIncludeNaNAndSignedZero) {
    const std::vector<LogicalType> t = {LogicalType::Double()};
    auto make = [&](std::vector<double> values) {
        Data d;
        DataChunk c;
        c.Initialize(t);
        for (idx_t i = 0; i < values.size(); i++) {
            c.SetValue(0, i, Value::Double(values[i]));
            d.rows.push_back({Value::Double(values[i])});
        }
        c.SetCardinality(values.size());
        d.chunks.push_back(std::move(c));
        return d;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Data left = make({0.0, nan, 1.0});
    const Data right = make({-0.0, nan, 2.0});
    const Rows got = RunJoin(left, right, t, t, {{0, 0}}, nullptr, PhysicalJoinType::Inner);
    EXPECT_EQ(got.size(), 2U) << "0.0 = -0.0 and NaN = NaN, as in the = operator";
}

// ---------------------------------------------------------------------------------- values /
// insert

TEST(Operators, ValuesWithAndWithoutColumns) {
    {
        PhysicalPlan plan;
        std::vector<std::vector<BoundExprPtr>> rows(1);
        auto& values = plan.Make<PhysicalValues>(std::vector<LogicalType>{}, std::move(rows));
        auto& result = plan.Make<PhysicalResultCollector>(std::vector<LogicalType>{});
        Pipeline p;
        p.source = &values;
        p.sink = &result;
        plan.pipelines.push_back(p);
        plan.root = &result;
        Executor ex(plan);
        ex.Run();
        const auto chunks = PhysicalResultCollector::TakeChunks(*ex.SinkState(result));
        ASSERT_EQ(chunks.size(), 1U);
        EXPECT_EQ(chunks[0].size(), 1U) << "SELECT 1 reads one row with no columns";
    }
    {
        PhysicalPlan plan;
        std::vector<std::vector<BoundExprPtr>> rows;
        for (int i = 0; i < 3000; i++) {
            std::vector<BoundExprPtr> row;
            row.push_back(BoundExpr::Constant(Value::Integer(i)));
            row.push_back(BoundExpr::Constant(i % 2 ? Value::Varchar("odd")
                                                    : Value::Null(LogicalType::Varchar())));
            rows.push_back(std::move(row));
        }
        const std::vector<LogicalType> t = {LogicalType::Integer(), LogicalType::Varchar()};
        auto& values = plan.Make<PhysicalValues>(t, std::move(rows));
        auto& result = plan.Make<PhysicalResultCollector>(t);
        Pipeline p;
        p.source = &values;
        p.sink = &result;
        plan.pipelines.push_back(p);
        plan.root = &result;
        const Rows got = Collect(plan);
        ASSERT_EQ(got.size(), 3000U);
        EXPECT_EQ(got[2999][0], Value::Integer(2999));
        EXPECT_TRUE(got[2998][1].IsNull());
        EXPECT_EQ(got[2999][1], Value::Varchar("odd"));
    }
}

TEST(Operators, InsertPublishesAtomicallyAndAFailureLeavesNoTrace) {
    const std::vector<ColumnDefinition> schema = {{"a", LogicalType::Integer()},
                                                  {"b", LogicalType::Varchar()}};
    auto target = std::make_shared<Table>("t", schema);
    Rng rng(12);
    const std::vector<LogicalType> t = {LogicalType::Integer(), LogicalType::Varchar()};
    Data d = MakeData(rng, t, 6, Gen());
    {
        PhysicalPlan plan;
        auto& src = plan.Make<ChunkListSource>(t, &d.chunks);
        auto& ins = plan.Make<PhysicalInsert>(target);
        Pipeline p;
        p.source = &src;
        p.sink = &ins;
        plan.pipelines.push_back(p);
        plan.root = &ins;
        Executor ex(plan);
        ex.Run();
        EXPECT_EQ(PhysicalInsert::InsertedRows(*ex.SinkState(ins)), d.rows.size());
    }
    EXPECT_EQ(target->RowCount(), d.rows.size());
    // A failing pipeline (integer overflow in a projection) must not publish anything.
    std::vector<BoundExprPtr> exprs;
    exprs.push_back(BoundExpr::Binary(
        OperatorKind::Add, Col(0, t[0]),
        BoundExpr::Constant(Value::Integer(std::numeric_limits<int32_t>::max())), t[0]));
    exprs.push_back(Col(1, t[1]));
    Data big;
    DataChunk c;
    c.Initialize(t);
    for (idx_t i = 0; i < 10; i++) {
        c.SetValue(0, i, Value::Integer(5));
        c.SetValue(1, i, Value::Varchar("x"));
    }
    c.SetCardinality(10);
    big.chunks.push_back(std::move(c));
    PhysicalPlan plan;
    auto& src = plan.Make<ChunkListSource>(t, &big.chunks);
    auto& proj = plan.Make<PhysicalProjection>(t, std::move(exprs));
    auto& ins = plan.Make<PhysicalInsert>(target);
    Pipeline p;
    p.source = &src;
    p.operators = {&proj};
    p.sink = &ins;
    plan.pipelines.push_back(p);
    plan.root = &ins;
    Executor ex(plan);
    EXPECT_THROW(ex.Run(), Error);
    EXPECT_EQ(target->RowCount(), d.rows.size())
        << "the failed insert must leave the table untouched";
}

TEST(Operators, NotNullViolationDuringInsertLeavesNoTrace) {
    const std::vector<ColumnDefinition> schema = {{"a", LogicalType::Integer(), true}};
    auto target = std::make_shared<Table>("t", schema);
    const std::vector<LogicalType> t = {LogicalType::Integer()};
    Data d;
    DataChunk c;
    c.Initialize(t);
    c.SetValue(0, 0, Value::Integer(1));
    c.SetValue(0, 1, Value::Null(t[0]));
    c.SetCardinality(2);
    d.chunks.push_back(std::move(c));
    PhysicalPlan plan;
    auto& src = plan.Make<ChunkListSource>(t, &d.chunks);
    auto& ins = plan.Make<PhysicalInsert>(target);
    Pipeline p;
    p.source = &src;
    p.sink = &ins;
    plan.pipelines.push_back(p);
    plan.root = &ins;
    Executor ex(plan);
    EXPECT_THROW(ex.Run(), Error);
    EXPECT_EQ(target->RowCount(), 0U);
}

TEST(Pipeline, PlanDescribesItsPipelines) {
    Rng rng(13);
    Data d = MakeData(rng, kTypes, 1, Gen());
    Simple s(kTypes, &d.chunks);
    s.Add<PhysicalFilter>(kTypes, BoundExpr::Constant(Value::Boolean(true)));
    s.Add<PhysicalLimit>(kTypes, 5, 2);
    s.Run(kTypes);
    const std::string text = s.plan.ToString();
    EXPECT_NE(text.find("Pipeline 0"), std::string::npos);
    EXPECT_NE(text.find("FILTER"), std::string::npos);
    EXPECT_NE(text.find("LIMIT 5 OFFSET 2"), std::string::npos);
    EXPECT_NE(text.find("RESULT"), std::string::npos);
}

// ---------------------------------------------------------------------------------- local states

// The Phase 6 scheduler will give every worker thread its own local sink state and call Combine
// once per local state, then Finalize. These tests drive that protocol by hand with several local
// states (the pipeline executor itself uses one) and require the same answers as a single pass.
namespace {

// Splits `chunks` randomly over `locals` local sink states of `op`, Combines them all in a random
// order, Finalizes, and returns the global state.
std::unique_ptr<GlobalSinkState> SinkThroughLocals(PhysicalOperator& op,
                                                   const std::vector<DataChunk>& chunks,
                                                   size_t locals, Rng& rng) {
    auto global = op.GetGlobalSinkState();
    std::vector<std::unique_ptr<LocalSinkState>> states;
    for (size_t i = 0; i < locals; i++) {
        states.push_back(op.GetLocalSinkState(*global));
    }
    for (const DataChunk& c : chunks) {
        if (c.size() > 0) {
            op.Sink(*global, *states[RandBelow(rng, locals)], c);
        }
    }
    std::vector<size_t> order(locals);
    for (size_t i = 0; i < locals; i++) {
        order[i] = i;
    }
    std::shuffle(order.begin(), order.end(), rng);
    for (const size_t i : order) {
        op.Combine(*global, *states[i]);
    }
    op.Finalize(*global);
    return global;
}

Rows DrainSource(PhysicalOperator& op, GlobalSinkState& sink) {
    auto global = op.GetGlobalSourceState(&sink);
    auto local = op.GetLocalSourceState(*global);
    DataChunk chunk;
    chunk.Initialize(op.types());
    Rows rows;
    for (;;) {
        chunk.Reset();
        if (!op.GetData(*global, *local, chunk)) {
            break;
        }
        for (auto& r : RowsOf(chunk)) {
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

} // namespace

TEST(OperatorLocalStates, HashAggregateMergesLocalStatesIntoOneAnswer) {
    Rng rng(21);
    const std::vector<LogicalType> in = {LogicalType::Varchar(), LogicalType::Integer(),
                                         LogicalType::BigInt()};
    for (int round = 0; round < 12; round++) {
        Data d = MakeData(rng, in, 8, Gen());
        std::vector<BoundExprPtr> groups, aggs;
        groups.push_back(Col(0, in[0]));
        for (const auto& [kind, arg, type] :
             {std::tuple{AggregateKind::CountStar, -1, LogicalType::BigInt()},
              std::tuple{AggregateKind::Sum, 2, LogicalType::BigInt()},
              std::tuple{AggregateKind::Max, 1, LogicalType::Integer()}}) {
            std::vector<BoundExprPtr> a;
            if (arg >= 0) {
                a.push_back(Col(static_cast<idx_t>(arg), in[static_cast<size_t>(arg)]));
            }
            aggs.push_back(BoundExpr::Aggregate(kind, std::move(a), false, type));
        }
        const std::vector<LogicalType> out = {in[0], LogicalType::BigInt(), LogicalType::BigInt(),
                                              in[1]};
        PhysicalHashAggregate op(out, std::move(groups), std::move(aggs));
        const auto global = SinkThroughLocals(op, d.chunks, 1 + RandBelow(rng, 4), rng);
        const Rows got = DrainSource(op, *global);

        std::map<std::vector<Value>, std::vector<size_t>, TupleLess> ref;
        for (size_t r = 0; r < d.rows.size(); r++) {
            ref[{d.rows[r][0]}].push_back(r);
        }
        ASSERT_EQ(got.size(), ref.size()) << "round " << round;
        for (const auto& row : got) {
            const auto& members = ref.at({row[0]});
            int64_t sum = 0;
            bool any = false;
            std::optional<Value> mx;
            for (const size_t r : members) {
                if (!d.rows[r][2].IsNull()) {
                    sum += d.rows[r][2].GetBigInt();
                    any = true;
                }
                if (!d.rows[r][1].IsNull() && (!mx || Value::Compare(d.rows[r][1], *mx) > 0)) {
                    mx = d.rows[r][1];
                }
            }
            ASSERT_EQ(row[1], Value::BigInt(static_cast<int64_t>(members.size())))
                << "round " << round;
            ASSERT_TRUE(any ? row[2] == Value::BigInt(sum) : row[2].IsNull());
            ASSERT_TRUE(mx ? row[3] == *mx : row[3].IsNull());
        }
    }
}

TEST(OperatorLocalStates, UngroupedAggregateWithNoLocalStateInputStillYieldsItsRow) {
    PhysicalPlan unused;
    std::vector<BoundExprPtr> aggs;
    aggs.push_back(
        BoundExpr::Aggregate(AggregateKind::CountStar, {}, false, LogicalType::BigInt()));
    PhysicalHashAggregate op({LogicalType::BigInt()}, {}, std::move(aggs));
    Rng rng(22);
    const auto global = SinkThroughLocals(op, {}, 3, rng); // three empty local states
    const Rows rows = DrainSource(op, *global);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0][0], Value::BigInt(0));
}

TEST(OperatorLocalStates, JoinBuildSideMergedFromLocalStatesProbesCorrectly) {
    Rng rng(23);
    const std::vector<LogicalType> lt = {LogicalType::Integer(), LogicalType::Varchar()};
    const std::vector<LogicalType> rt = {LogicalType::Integer(), LogicalType::Double()};
    for (int round = 0; round < 15; round++) {
        const Data left = MakeData(rng, lt, 3, Gen(), false);
        const Data right = MakeData(rng, rt, 6, Gen(), false);
        std::vector<LogicalType> out = lt;
        out.insert(out.end(), rt.begin(), rt.end());
        std::vector<BoundExprPtr> lk, rk;
        lk.push_back(Col(0, lt[0]));
        rk.push_back(Col(0, rt[0]));
        PhysicalHashJoin join(out, PhysicalJoinType::Left, lt, rt, std::move(lk), std::move(rk),
                              nullptr);
        const auto build = SinkThroughLocals(join, right.chunks, 1 + RandBelow(rng, 4), rng);
        auto state = join.GetOperatorState(build.get());
        DataChunk output;
        output.Initialize(out);
        Rows got;
        for (const DataChunk& chunk : left.chunks) {
            if (chunk.size() == 0) {
                continue;
            }
            OperatorResult r;
            do {
                output.Reset();
                r = join.Execute(*state, chunk, output);
                for (auto& row : RowsOf(output)) {
                    got.push_back(std::move(row));
                }
            } while (r == OperatorResult::HaveMoreOutput);
        }
        const auto matches = ReferenceMatches(left.rows, right.rows, {{0, 0}}, nullptr);
        ExpectSameMultiset(got,
                           DeriveJoin(left.rows, right.rows, matches, PhysicalJoinType::Left, rt),
                           "round " + std::to_string(round));
    }
}

TEST(OperatorLocalStates, OrderByAndTopNMergeLocalBuffers) {
    Rng rng(24);
    for (int round = 0; round < 12; round++) {
        Data d = MakeData(rng, kTypes, 10, Gen());
        std::vector<SortSpec> specs;
        std::vector<SortKey> keys = RandomKeys(rng, specs);
        auto clone = [&] {
            std::vector<SortKey> k;
            for (const SortKey& x : keys) {
                k.push_back({x.expr->Clone(), x.descending, x.nulls_first});
            }
            return k;
        };
        const Rows sorted = ReferenceSort(d.rows, keys, specs);
        {
            PhysicalOrder op(kTypes, clone());
            const auto global = SinkThroughLocals(op, d.chunks, 1 + RandBelow(rng, 4), rng);
            const Rows got = DrainSource(op, *global);
            ASSERT_EQ(got.size(), sorted.size());
            for (size_t i = 0; i < got.size(); i++) {
                // equal sort keys may arrive from different local buffers in any relative order, so
                // compare the key columns only (the multiset equality of rows is checked below)
                for (size_t k = 0; k < keys.size(); k++) {
                    const idx_t col = keys[k].expr->ordinal;
                    ASSERT_EQ(test::CompareWithNulls(got[i][col], sorted[i][col]), 0)
                        << "round " << round;
                }
            }
            ExpectSameMultiset(got, sorted, "order by multiset");
        }
        {
            const int64_t limit = static_cast<int64_t>(1 + RandBelow(rng, 20)),
                          offset = static_cast<int64_t>(RandBelow(rng, 10));
            PhysicalTopN op(kTypes, clone(), limit, offset);
            const auto global = SinkThroughLocals(op, d.chunks, 1 + RandBelow(rng, 4), rng);
            const Rows got = DrainSource(op, *global);
            const size_t begin = std::min<size_t>(static_cast<size_t>(offset), sorted.size());
            const size_t end = std::min<size_t>(sorted.size(), begin + static_cast<size_t>(limit));
            ASSERT_EQ(got.size(), end - begin) << "round " << round;
            for (size_t i = 0; i < got.size(); i++) {
                for (size_t k = 0; k < keys.size(); k++) {
                    const idx_t col = keys[k].expr->ordinal;
                    ASSERT_EQ(test::CompareWithNulls(got[i][col], sorted[begin + i][col]), 0)
                        << "round " << round;
                }
            }
        }
    }
}

TEST(OperatorLocalStates, ResultCollectorKeepsEveryRowFromEveryLocalState) {
    Rng rng(25);
    Data d = MakeData(rng, kTypes, 9, Gen());
    PhysicalResultCollector op(kTypes);
    const auto global = SinkThroughLocals(op, d.chunks, 4, rng);
    Rows got;
    for (const DataChunk& c : PhysicalResultCollector::TakeChunks(*global)) {
        for (auto& r : RowsOf(c)) {
            got.push_back(std::move(r));
        }
    }
    ExpectSameMultiset(got, d.rows, "collector");
}

} // namespace cdb
