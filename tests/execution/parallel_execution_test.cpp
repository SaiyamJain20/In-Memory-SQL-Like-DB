// Parallel pipelines: the same plans run on 1..8 threads must give the answers of a naive
// reference, and the executor must really run them on several threads (or on one, where an
// operator forbids it).

#include "execution/basic_operators.h"
#include "execution/hash_aggregate.h"
#include "execution/pipeline.h"
#include "execution/task_scheduler.h"

#include "exec_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <thread>

namespace cdb {

namespace {

using test::Chance;
using test::RandBelow;
using test::Rng;
using test::RowsOf;
using test::SameValue;
using test::TupleLess;
using Rows = std::vector<std::vector<Value>>;

constexpr size_t kThreadCounts[] = {1, 2, 4, 8};

// Hands out prepared chunks through an atomic cursor, tagging each with its index as the batch.
// The parallel counterpart of a table scan.
class ParallelChunkSource final : public PhysicalOperator {
  public:
    ParallelChunkSource(std::vector<LogicalType> types, const std::vector<DataChunk>* chunks)
        : PhysicalOperator(std::move(types)), chunks_(chunks) {}
    std::string Name() const override { return "PARALLEL_CHUNKS"; }

    struct Global final : GlobalSourceState {
        std::atomic<size_t> next{0};
    };
    struct Local final : LocalSourceState {};

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override {
        return std::make_unique<Global>();
    }
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override {
        locals_++;
        return std::make_unique<Local>();
    }
    bool ParallelSource() const override { return true; }
    idx_t MaxSourceThreads(GlobalSourceState&) const override { return chunks_->size(); }
    bool GetData(GlobalSourceState& g, LocalSourceState& l, DataChunk& out) override {
        const size_t i = static_cast<Global&>(g).next.fetch_add(1);
        if (i >= chunks_->size()) {
            return false;
        }
        const DataChunk& in = (*chunks_)[i];
        for (idx_t c = 0; c < in.ColumnCount(); c++) {
            out.column(c).Reference(in.column(c));
        }
        out.SetCardinality(in.size());
        l.batch_index = i;
        return true;
    }

    // How many threads took part in the last run (one local source state each).
    int participants() const { return locals_.load(); }

  private:
    const std::vector<DataChunk>* chunks_;
    std::atomic<int> locals_{0};
};

// A streaming operator that passes chunks through but, on each thread's first chunk, waits until
// `want` threads are inside it at once: it only completes if the pipeline really runs in parallel.
class Rendezvous final : public PhysicalOperator {
  public:
    Rendezvous(std::vector<LogicalType> types, int want)
        : PhysicalOperator(std::move(types)), want_(want) {}
    std::string Name() const override { return "RENDEZVOUS"; }
    struct State final : OperatorState {
        bool arrived = false;
    };
    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState*) override {
        return std::make_unique<State>();
    }
    bool ParallelOperator() const override { return true; }
    OperatorResult Execute(OperatorState& state, const DataChunk& input,
                           DataChunk& output) override {
        auto& s = static_cast<State&>(state);
        if (!s.arrived) {
            s.arrived = true;
            inside_++;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (inside_.load() < want_ && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            max_inside_ = std::max(max_inside_.load(), inside_.load());
        }
        for (idx_t c = 0; c < input.ColumnCount(); c++) {
            output.column(c).Reference(input.column(c));
        }
        output.SetCardinality(input.size());
        return OperatorResult::NeedMoreInput;
    }
    int max_inside() const { return max_inside_.load(); }

  private:
    int want_;
    std::atomic<int> inside_{0};
    std::atomic<int> max_inside_{0};
};

// An operator that is correct per chunk but declares itself not parallel (like LIMIT would).
class SerialOnly final : public PhysicalOperator {
  public:
    explicit SerialOnly(std::vector<LogicalType> types) : PhysicalOperator(std::move(types)) {}
    std::string Name() const override { return "SERIAL_ONLY"; }
    struct State final : OperatorState {};
    std::unique_ptr<OperatorState> GetOperatorState(GlobalSinkState*) override {
        return std::make_unique<State>();
    }
    OperatorResult Execute(OperatorState&, const DataChunk& input, DataChunk& output) override {
        for (idx_t c = 0; c < input.ColumnCount(); c++) {
            output.column(c).Reference(input.column(c));
        }
        output.SetCardinality(input.size());
        return OperatorResult::NeedMoreInput;
    }
};

struct Data {
    std::vector<DataChunk> chunks;
    Rows rows;
};

// `nchunks` chunks of random size (some empty, some a full vector), over `types`.
Data MakeData(Rng& rng, const std::vector<LogicalType>& types, int nchunks,
              const test::ValueGen& gen, idx_t max_rows = 300) {
    Data d;
    for (int k = 0; k < nchunks; k++) {
        const idx_t n = Chance(rng, 0.05)   ? 0
                        : Chance(rng, 0.05) ? kVectorSize
                                            : 1 + RandBelow(rng, max_rows);
        d.chunks.push_back(test::RandomVariedChunk(rng, types, n, gen));
        for (auto& r : RowsOf(d.chunks.back())) {
            d.rows.push_back(std::move(r));
        }
    }
    return d;
}

test::ValueGen SmallGen(double nulls = 0.15) {
    return [nulls](Rng& rng, LogicalType t) { return test::SmallDomainValue(rng, t, nulls); };
}

// Runs the plan on `threads` threads (a null scheduler for 1: the plain serial path) and returns
// the collected rows in output order.
Rows RunPlan(PhysicalPlan& plan, size_t threads, size_t* chunk_count = nullptr) {
    TaskScheduler scheduler(threads);
    Executor ex(plan, threads > 1 ? &scheduler : nullptr);
    ex.Run();
    const std::vector<DataChunk> chunks =
        PhysicalResultCollector::TakeChunks(*ex.SinkState(*plan.root));
    if (chunk_count != nullptr) {
        *chunk_count = chunks.size();
    }
    Rows rows;
    for (const DataChunk& c : chunks) {
        c.Verify();
        for (auto& r : RowsOf(c)) {
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

// source -> operators -> collector, in one pipeline.
struct Pipe {
    PhysicalPlan plan;
    ParallelChunkSource* source = nullptr;
    Pipeline pipeline;

    Pipe(const std::vector<LogicalType>& types, const std::vector<DataChunk>* chunks) {
        source = &plan.Make<ParallelChunkSource>(types, chunks);
        pipeline.source = source;
    }
    template <class T, class... A> T& Add(A&&... args) {
        T& op = plan.Make<T>(std::forward<A>(args)...);
        pipeline.operators.push_back(&op);
        return op;
    }
    void Finish(const std::vector<LogicalType>& out_types) {
        auto& sink = plan.Make<PhysicalResultCollector>(out_types);
        pipeline.sink = &sink;
        plan.root = &sink;
        plan.pipelines.push_back(pipeline);
    }
};

BoundExprPtr Col(idx_t i, LogicalType t) {
    return BoundExpr::ColumnRef(i, t);
}
BoundExprPtr Cmp(OperatorKind op, BoundExprPtr a, BoundExprPtr b) {
    return BoundExpr::Binary(op, std::move(a), std::move(b), LogicalType::Boolean());
}

// Process-wide knob: merge by partition from `groups` groups on (1 = always, huge = never).
class ScopedMinGroups {
  public:
    explicit ScopedMinGroups(idx_t groups) {
        PhysicalHashAggregate::SetMinGroupsToPartition(groups);
    }
    ~ScopedMinGroups() { PhysicalHashAggregate::SetMinGroupsToPartition(0); }
    ScopedMinGroups(const ScopedMinGroups&) = delete;
    ScopedMinGroups& operator=(const ScopedMinGroups&) = delete;
};

} // namespace

// ---------------------------------------------------------------------------------- pipelines

TEST(ParallelExecution, FilterAndProjectionGiveTheSingleThreadedRowsInTheSameOrder) {
    Rng rng(1);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar(),
                                         LogicalType::Double()};
    const Data d = MakeData(rng, in, 120, SmallGen());
    // WHERE col0 > 0, SELECT col1, col0 + 1
    Rows want;
    for (const auto& r : d.rows) {
        if (!r[0].IsNull() && r[0].GetInteger() > 0) {
            want.push_back({r[1], Value::Integer(r[0].GetInteger() + 1)});
        }
    }
    ASSERT_GT(want.size(), 100U);
    const std::vector<LogicalType> out = {LogicalType::Varchar(), LogicalType::Integer()};
    for (const size_t threads : kThreadCounts) {
        for (int round = 0; round < 3; round++) {
            Pipe p(in, &d.chunks);
            p.Add<PhysicalFilter>(
                in, Cmp(OperatorKind::Gt, Col(0, in[0]), BoundExpr::Constant(Value::Integer(0))));
            std::vector<BoundExprPtr> exprs;
            exprs.push_back(Col(1, in[1]));
            exprs.push_back(BoundExpr::Binary(OperatorKind::Add, Col(0, in[0]),
                                              BoundExpr::Constant(Value::Integer(1)),
                                              LogicalType::Integer()));
            p.Add<PhysicalProjection>(out, std::move(exprs));
            p.Finish(out);
            const Rows got = RunPlan(p.plan, threads);
            ASSERT_EQ(got.size(), want.size()) << "threads " << threads;
            for (size_t i = 0; i < got.size(); i++) {
                ASSERT_TRUE(test::CompareTuples(got[i], want[i]) == 0)
                    << "threads " << threads << " row " << i << ": rows must come back in the "
                    << "order a single thread produces (batch order)";
            }
        }
    }
}

TEST(ParallelExecution, ThePipelineReallyRunsOnSeveralThreads) {
    Rng rng(2);
    const std::vector<LogicalType> in = {LogicalType::Integer()};
    const Data d = MakeData(rng, in, 200, SmallGen());
    for (const size_t threads : {size_t{2}, size_t{4}}) {
        Pipe p(in, &d.chunks);
        auto& rendezvous = p.Add<Rendezvous>(in, static_cast<int>(threads));
        p.Finish(in);
        const Rows got = RunPlan(p.plan, threads);
        EXPECT_EQ(got.size(), d.rows.size());
        EXPECT_EQ(rendezvous.max_inside(), static_cast<int>(threads))
            << "all " << threads << " threads must have been inside the operator at once";
        EXPECT_EQ(p.source->participants(), static_cast<int>(threads));
    }
}

TEST(ParallelExecution, AnOperatorThatIsNotParallelKeepsThePipelineOnOneThread) {
    Rng rng(3);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar()};
    const Data d = MakeData(rng, in, 80, SmallGen());
    Pipe p(in, &d.chunks);
    p.Add<SerialOnly>(in);
    p.Finish(in);
    const Rows got = RunPlan(p.plan, 8);
    EXPECT_EQ(p.source->participants(), 1);
    ASSERT_EQ(got.size(), d.rows.size());
    for (size_t i = 0; i < got.size(); i++) {
        ASSERT_TRUE(test::CompareTuples(got[i], d.rows[i]) == 0) << i;
    }
}

TEST(ParallelExecution, LimitRunsOnOneThreadAndReturnsTheFirstRowsInOrder) {
    Rng rng(4);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar()};
    const Data d = MakeData(rng, in, 100, SmallGen());
    for (const auto& [limit, offset] :
         std::vector<std::pair<int64_t, int64_t>>{{5, 0}, {50, 7}, {100000, 0}, {0, 0}}) {
        Pipe p(in, &d.chunks);
        p.Add<PhysicalLimit>(in, limit, offset);
        p.Finish(in);
        const Rows got = RunPlan(p.plan, 8);
        EXPECT_EQ(p.source->participants(), 1) << "LIMIT counts rows across chunks";
        const size_t begin = std::min<size_t>(static_cast<size_t>(offset), d.rows.size());
        const size_t end = std::min(d.rows.size(), begin + static_cast<size_t>(limit));
        ASSERT_EQ(got.size(), end - begin);
        for (size_t i = 0; i < got.size(); i++) {
            ASSERT_TRUE(test::CompareTuples(got[i], d.rows[begin + i]) == 0) << i;
        }
    }
}

TEST(ParallelExecution, ASingleChunkSourceNeverUsesMoreThanOneParticipant) {
    Rng rng(5);
    const std::vector<LogicalType> in = {LogicalType::Integer()};
    const Data d = MakeData(rng, in, 1, SmallGen());
    Pipe p(in, &d.chunks);
    p.Finish(in);
    RunPlan(p.plan, 8);
    EXPECT_EQ(p.source->participants(), 1) << "MaxSourceThreads bounds the participants";
}

TEST(ParallelExecution, EmptyInputGivesEmptyOutputOnEveryThreadCount) {
    const std::vector<LogicalType> in = {LogicalType::Integer()};
    const std::vector<DataChunk> none;
    for (const size_t threads : kThreadCounts) {
        Pipe p(in, &none);
        p.Finish(in);
        EXPECT_TRUE(RunPlan(p.plan, threads).empty());
    }
}

TEST(ParallelExecution, AnErrorInOneThreadFailsTheQueryAndLeavesThePoolUsable) {
    Rng rng(6);
    const std::vector<LogicalType> in = {LogicalType::Integer()};
    const Data d = MakeData(rng, in, 150, SmallGen());
    TaskScheduler scheduler(4);
    for (int round = 0; round < 5; round++) {
        // WHERE col0 + INT_MAX > 0 overflows on every positive col0 (the data has plenty)
        Pipe bad(in, &d.chunks);
        bad.Add<PhysicalFilter>(in, Cmp(OperatorKind::Gt,
                                        BoundExpr::Binary(OperatorKind::Add, Col(0, in[0]),
                                                          BoundExpr::Constant(Value::Integer(
                                                              std::numeric_limits<int32_t>::max())),
                                                          LogicalType::Integer()),
                                        BoundExpr::Constant(Value::Integer(0))));
        bad.Finish(in);
        Executor failing(bad.plan, &scheduler);
        EXPECT_THROW(failing.Run(), Error);

        Pipe good(in, &d.chunks);
        good.Finish(in);
        Executor ok(good.plan, &scheduler);
        ASSERT_NO_THROW(ok.Run());
        size_t rows = 0;
        for (const DataChunk& c :
             PhysicalResultCollector::TakeChunks(*ok.SinkState(*good.plan.root))) {
            rows += c.size();
        }
        ASSERT_EQ(rows, d.rows.size()) << "the scheduler must work after a failed query";
    }
}

// ---------------------------------------------------------------------------------- aggregate

namespace {

// Columns: k Integer (group key, `domain` values), tag Varchar (small pool), v BigInt, d Double
// (quarters: sums are exact in any order), dt Date.
const std::vector<LogicalType> kAggTypes = {LogicalType::Integer(), LogicalType::Varchar(),
                                            LogicalType::BigInt(), LogicalType::Double(),
                                            LogicalType::Date()};

test::ValueGen AggGen(int domain) {
    return [domain](Rng& rng, LogicalType t) {
        switch (t.id()) {
        case TypeId::Integer:
            return Chance(rng, 0.04) ? Value::Null(t)
                                     : Value::Integer(static_cast<int32_t>(
                                           RandBelow(rng, static_cast<uint64_t>(domain))));
        case TypeId::BigInt:
            return Chance(rng, 0.1)
                       ? Value::Null(t)
                       : Value::BigInt(static_cast<int64_t>(RandBelow(rng, 2001)) - 1000);
        case TypeId::Double:
            return Chance(rng, 0.1)
                       ? Value::Null(t)
                       : Value::Double(
                             static_cast<double>(static_cast<int64_t>(RandBelow(rng, 65)) - 32) *
                             0.25);
        default:
            return test::SmallDomainValue(rng, t, 0.1);
        }
    };
}

struct AggRef {
    int64_t count_star = 0, count_v = 0;
    __extension__ typedef __int128 Int128;
    Int128 sum_v = 0;
    bool any_v = false;
    double sum_d = 0;
    bool any_d = false;
    std::optional<Value> min_tag, max_tag, min_d, max_dt, min_k;
};

void Fold(AggRef& a, const std::vector<Value>& row) {
    a.count_star++;
    if (!row[2].IsNull()) {
        a.count_v++;
        a.sum_v += row[2].GetBigInt();
        a.any_v = true;
    }
    if (!row[1].IsNull()) {
        if (!a.min_tag || Value::Compare(row[1], *a.min_tag) < 0) {
            a.min_tag = row[1];
        }
        if (!a.max_tag || Value::Compare(row[1], *a.max_tag) > 0) {
            a.max_tag = row[1];
        }
    }
    if (!row[3].IsNull()) {
        a.sum_d += row[3].GetDouble();
        a.any_d = true;
        if (!a.min_d || Value::Compare(row[3], *a.min_d) < 0) {
            a.min_d = row[3];
        }
    }
    if (!row[4].IsNull() && (!a.max_dt || Value::Compare(row[4], *a.max_dt) > 0)) {
        a.max_dt = row[4];
    }
    if (!row[0].IsNull() && (!a.min_k || Value::Compare(row[0], *a.min_k) < 0)) {
        a.min_k = row[0];
    }
}

Value OrNull(const std::optional<Value>& v, LogicalType t) {
    return v ? *v : Value::Null(t);
}

// The group-by columns of an aggregate plan: indexes into kAggTypes.
using GroupCols = std::vector<idx_t>;

// P0: chunks -> aggregate; P1: aggregate -> collector.
struct AggPlan {
    PhysicalPlan plan;
    ParallelChunkSource* source = nullptr;
    std::vector<LogicalType> out_types;
};

std::unique_ptr<AggPlan> MakeAggPlan(const std::vector<DataChunk>* chunks,
                                     const GroupCols& group_cols) {
    auto ap = std::make_unique<AggPlan>();
    PhysicalPlan& plan = ap->plan;
    ap->source = &plan.Make<ParallelChunkSource>(kAggTypes, chunks);
    std::vector<BoundExprPtr> groups;
    for (const idx_t g : group_cols) {
        groups.push_back(Col(g, kAggTypes[g]));
        ap->out_types.push_back(kAggTypes[g]);
    }
    std::vector<BoundExprPtr> aggs;
    const auto agg = [&](AggregateKind k, BoundExprPtr arg, LogicalType result) {
        std::vector<BoundExprPtr> a;
        if (arg) {
            a.push_back(std::move(arg));
        }
        aggs.push_back(BoundExpr::Aggregate(k, std::move(a), false, result));
        ap->out_types.push_back(result);
    };
    agg(AggregateKind::CountStar, nullptr, LogicalType::BigInt());
    agg(AggregateKind::Count, Col(2, kAggTypes[2]), LogicalType::BigInt());
    agg(AggregateKind::Sum, Col(2, kAggTypes[2]), LogicalType::BigInt());
    agg(AggregateKind::Avg, Col(2, kAggTypes[2]), LogicalType::Double());
    agg(AggregateKind::Min, Col(1, kAggTypes[1]), kAggTypes[1]);
    agg(AggregateKind::Max, Col(1, kAggTypes[1]), kAggTypes[1]);
    agg(AggregateKind::Sum, Col(3, kAggTypes[3]), LogicalType::Double());
    agg(AggregateKind::Min, Col(3, kAggTypes[3]), kAggTypes[3]);
    agg(AggregateKind::Max, Col(4, kAggTypes[4]), kAggTypes[4]);
    agg(AggregateKind::Min, Col(0, kAggTypes[0]), kAggTypes[0]);
    auto& ha = plan.Make<PhysicalHashAggregate>(ap->out_types, std::move(groups), std::move(aggs));
    auto& result = plan.Make<PhysicalResultCollector>(ap->out_types);
    Pipeline p0;
    p0.source = ap->source;
    p0.sink = &ha;
    Pipeline p1;
    p1.source = &ha;
    p1.sink = &result;
    p1.dependencies = {0};
    plan.pipelines = {p0, p1};
    plan.root = &result;
    return ap;
}

// Compares the plan's output with a reference computed from `rows`.
void ExpectAggregateMatches(const Rows& got, const Rows& rows, const GroupCols& group_cols,
                            const std::string& what) {
    std::map<std::vector<Value>, AggRef, TupleLess> ref;
    for (const auto& r : rows) {
        std::vector<Value> key;
        for (const idx_t g : group_cols) {
            key.push_back(r[g]);
        }
        Fold(ref[key], r);
    }
    if (group_cols.empty() && ref.empty()) {
        ref[{}] = AggRef{}; // a global aggregate over no rows still yields one row
    }
    ASSERT_EQ(got.size(), ref.size()) << what;
    std::map<std::vector<Value>, const std::vector<Value>*, TupleLess> by_key;
    for (const auto& row : got) {
        const std::vector<Value> key(row.begin(),
                                     row.begin() + static_cast<long>(group_cols.size()));
        ASSERT_TRUE(by_key.emplace(key, &row).second) << what << ": a group appeared twice";
    }
    for (const auto& [key, a] : ref) {
        const auto it = by_key.find(key);
        ASSERT_NE(it, by_key.end()) << what << ": group missing";
        const std::vector<Value>& row = *it->second;
        size_t c = group_cols.size();
        const auto expect = [&](const Value& want, const char* name) {
            ASSERT_TRUE(SameValue(row[c], want))
                << what << ": " << name << " got " << row[c].ToString() << " want "
                << want.ToString();
            c++;
        };
        expect(Value::BigInt(a.count_star), "count(*)");
        expect(Value::BigInt(a.count_v), "count(v)");
        expect(a.any_v ? Value::BigInt(static_cast<int64_t>(a.sum_v))
                       : Value::Null(LogicalType::BigInt()),
               "sum(v)");
        expect(a.count_v > 0 ? Value::Double(static_cast<double>(static_cast<int64_t>(a.sum_v)) /
                                             static_cast<double>(a.count_v))
                             : Value::Null(LogicalType::Double()),
               "avg(v)");
        expect(OrNull(a.min_tag, kAggTypes[1]), "min(tag)");
        expect(OrNull(a.max_tag, kAggTypes[1]), "max(tag)");
        expect(a.any_d ? Value::Double(a.sum_d) : Value::Null(LogicalType::Double()), "sum(d)");
        expect(OrNull(a.min_d, kAggTypes[3]), "min(d)");
        expect(OrNull(a.max_dt, kAggTypes[4]), "max(dt)");
        expect(OrNull(a.min_k, kAggTypes[0]), "min(k)");
    }
}

} // namespace

TEST(ParallelExecution, HashAggregateMatchesTheReferenceOnEveryThreadCountAndMergeStrategy) {
    Rng rng(7);
    for (const int domain : {3, 40, 3000}) {
        const Data d = MakeData(rng, kAggTypes, 70, AggGen(domain));
        for (const GroupCols& groups : {GroupCols{0}, GroupCols{1}, GroupCols{0, 1}, GroupCols{}}) {
            for (const idx_t min_groups : {idx_t{1}, idx_t{1} << 40}) {
                const ScopedMinGroups merge(min_groups);
                for (const size_t threads : kThreadCounts) {
                    const auto ap = MakeAggPlan(&d.chunks, groups);
                    const Rows got = RunPlan(ap->plan, threads);
                    ExpectAggregateMatches(
                        got, d.rows, groups,
                        "domain " + std::to_string(domain) + " groups " +
                            std::to_string(groups.size()) +
                            (min_groups == 1 ? " partitioned" : " serial merge") + " threads " +
                            std::to_string(threads));
                    if (::testing::Test::HasFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

TEST(ParallelExecution, TheSameAggregateRunTwiceGivesTheSameGroupsAndValues) {
    Rng rng(8);
    const Data d = MakeData(rng, kAggTypes, 60, AggGen(500));
    const ScopedMinGroups merge(1);
    for (int round = 0; round < 25; round++) {
        const auto ap = MakeAggPlan(&d.chunks, {0, 1});
        ExpectAggregateMatches(RunPlan(ap->plan, 8), d.rows, {0, 1},
                               "round " + std::to_string(round));
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(ParallelExecution, ALargeResultIsMergedByPartitionAndASmallOneIntoOneTable) {
    Rng rng(9);
    const Data d = MakeData(rng, kAggTypes, 60, AggGen(600));
    size_t partitioned_chunks = 0, serial_chunks = 0;
    {
        const ScopedMinGroups merge(1);
        const auto ap = MakeAggPlan(&d.chunks, {0});
        RunPlan(ap->plan, 4, &partitioned_chunks);
    }
    {
        const ScopedMinGroups merge(idx_t{1} << 40);
        const auto ap = MakeAggPlan(&d.chunks, {0});
        RunPlan(ap->plan, 4, &serial_chunks);
    }
    EXPECT_EQ(serial_chunks, 1U) << "~600 groups fit one output chunk";
    EXPECT_GT(partitioned_chunks, 8U)
        << "one output table (so at least one chunk) per hash partition";
}

TEST(ParallelExecution, DistinctAggregatesFallBackToMergingWholeTables) {
    Rng rng(10);
    const Data d = MakeData(rng, kAggTypes, 50, AggGen(30));
    for (const size_t threads : kThreadCounts) {
        const ScopedMinGroups merge(1);
        PhysicalPlan plan;
        auto& src = plan.Make<ParallelChunkSource>(kAggTypes, &d.chunks);
        std::vector<BoundExprPtr> groups;
        groups.push_back(Col(0, kAggTypes[0]));
        std::vector<BoundExprPtr> aggs;
        {
            std::vector<BoundExprPtr> a;
            a.push_back(Col(1, kAggTypes[1]));
            aggs.push_back(BoundExpr::Aggregate(AggregateKind::Count, std::move(a), true,
                                                LogicalType::BigInt()));
        }
        const std::vector<LogicalType> out = {kAggTypes[0], LogicalType::BigInt()};
        auto& ha = plan.Make<PhysicalHashAggregate>(out, std::move(groups), std::move(aggs));
        auto& result = plan.Make<PhysicalResultCollector>(out);
        Pipeline p0;
        p0.source = &src;
        p0.sink = &ha;
        Pipeline p1;
        p1.source = &ha;
        p1.sink = &result;
        p1.dependencies = {0};
        plan.pipelines = {p0, p1};
        plan.root = &result;
        const Rows got = RunPlan(plan, threads);

        std::map<std::vector<Value>, std::set<std::string>, TupleLess> ref;
        for (const auto& r : d.rows) {
            auto& s = ref[{r[0]}];
            if (!r[1].IsNull()) {
                s.insert(r[1].GetVarchar());
            }
        }
        ASSERT_EQ(got.size(), ref.size()) << "threads " << threads;
        for (const auto& row : got) {
            ASSERT_EQ(row[1], Value::BigInt(static_cast<int64_t>(ref.at({row[0]}).size())))
                << "threads " << threads << " key " << row[0].ToString();
        }
    }
}

TEST(ParallelExecution, AnIntegerSumOverflowIsReportedWhateverTheThreadCount) {
    // The total of the column is MAX + 5: no thread's partial sum overflows by itself for most
    // splits, but the merged total does.
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    std::vector<DataChunk> chunks;
    const std::vector<LogicalType> in = {LogicalType::BigInt()};
    for (int c = 0; c < 40; c++) {
        DataChunk chunk;
        chunk.Initialize(in);
        chunk.SetValue(0, 0, Value::BigInt(c == 0 ? kMax : (c == 1 ? 5 : 0)));
        chunk.SetCardinality(1);
        chunks.push_back(std::move(chunk));
    }
    for (const size_t threads : kThreadCounts) {
        PhysicalPlan plan;
        auto& src = plan.Make<ParallelChunkSource>(in, &chunks);
        std::vector<BoundExprPtr> aggs;
        {
            std::vector<BoundExprPtr> a;
            a.push_back(Col(0, in[0]));
            aggs.push_back(BoundExpr::Aggregate(AggregateKind::Sum, std::move(a), false,
                                                LogicalType::BigInt()));
        }
        const std::vector<LogicalType> out = {LogicalType::BigInt()};
        auto& ha =
            plan.Make<PhysicalHashAggregate>(out, std::vector<BoundExprPtr>{}, std::move(aggs));
        auto& result = plan.Make<PhysicalResultCollector>(out);
        Pipeline p0;
        p0.source = &src;
        p0.sink = &ha;
        Pipeline p1;
        p1.source = &ha;
        p1.sink = &result;
        p1.dependencies = {0};
        plan.pipelines = {p0, p1};
        plan.root = &result;
        EXPECT_THROW(RunPlan(plan, threads), Error) << "threads " << threads;
    }
}

} // namespace cdb
