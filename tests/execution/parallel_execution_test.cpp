// Parallel pipelines: the same plans run on 1..8 threads must give the answers of a naive
// reference, and the executor must really run them on several threads (or on one, where an
// operator forbids it).

#include "execution/basic_operators.h"
#include "execution/hash_aggregate.h"
#include "execution/hash_join.h"
#include "execution/pipeline.h"
#include "execution/sort.h"
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

// Hands out prepared chunks through an atomic cursor, `chunks_per_batch` at a time: a thread claims
// a whole batch and returns its chunks in order, all tagged with the batch number (as a table scan
// does with a morsel). The parallel counterpart of a table scan.
class ParallelChunkSource final : public PhysicalOperator {
  public:
    ParallelChunkSource(std::vector<LogicalType> types, const std::vector<DataChunk>* chunks,
                        size_t chunks_per_batch = 1)
        : PhysicalOperator(std::move(types)), chunks_(chunks), per_batch_(chunks_per_batch) {}
    std::string Name() const override { return "PARALLEL_CHUNKS"; }

    struct Global final : GlobalSourceState {
        std::atomic<size_t> next_batch{0};
    };
    struct Local final : LocalSourceState {
        size_t next = 0, end = 0; // the chunks of the batch in hand
        size_t batch = 0;
    };

    std::unique_ptr<GlobalSourceState> GetGlobalSourceState(GlobalSinkState*) override {
        return std::make_unique<Global>();
    }
    std::unique_ptr<LocalSourceState> GetLocalSourceState(GlobalSourceState&) override {
        locals_++;
        return std::make_unique<Local>();
    }
    bool ParallelSource() const override { return true; }
    idx_t MaxSourceThreads(GlobalSourceState&) const override {
        return (chunks_->size() + per_batch_ - 1) / per_batch_;
    }
    bool GetData(GlobalSourceState& g, LocalSourceState& ls, DataChunk& out) override {
        auto& l = static_cast<Local&>(ls);
        if (l.next == l.end) {
            const size_t batch = static_cast<Global&>(g).next_batch.fetch_add(1);
            if (batch * per_batch_ >= chunks_->size()) {
                return false;
            }
            l.batch = batch;
            l.next = batch * per_batch_;
            l.end = std::min(chunks_->size(), (batch + 1) * per_batch_);
        }
        const DataChunk& in = (*chunks_)[l.next++];
        for (idx_t c = 0; c < in.ColumnCount(); c++) {
            out.column(c).Reference(in.column(c));
        }
        out.SetCardinality(in.size());
        l.batch_index = l.batch;
        return true;
    }

    // How many threads took part in the last run (one local source state each).
    int participants() const { return locals_.load(); }

    // What the executor last told the source about how many threads will read it (0: never told).
    void SetThreadHint(size_t threads) override { hint_ = threads; }
    size_t thread_hint() const { return hint_; }

  private:
    const std::vector<DataChunk>* chunks_;
    size_t per_batch_;
    std::atomic<int> locals_{0};
    size_t hint_ = 0;
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
              const test::ValueGen& gen, idx_t max_rows = 300, bool allow_full_chunks = true) {
    Data d;
    for (int k = 0; k < nchunks; k++) {
        const idx_t n = Chance(rng, 0.05)                        ? 0
                        : allow_full_chunks && Chance(rng, 0.05) ? kVectorSize
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

TEST(ParallelExecution, TheExecutorTellsTheSourceHowManyThreadsWillReadIt) {
    Rng rng(6);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar()};
    const Data d = MakeData(rng, in, 40, SmallGen());
    for (const size_t threads : {size_t{1}, size_t{2}, size_t{4}, size_t{8}}) {
        Pipe parallel(in, &d.chunks);
        parallel.Finish(in);
        RunPlan(parallel.plan, threads);
        EXPECT_EQ(parallel.source->thread_hint(), threads);

        // a pipeline that must stay on one thread is not sized for more
        Pipe limited(in, &d.chunks);
        limited.Add<PhysicalLimit>(in, 5, 0);
        limited.Finish(in);
        RunPlan(limited.plan, threads);
        EXPECT_EQ(limited.source->thread_hint(), 1U) << "LIMIT: serial pipeline, " << threads;

        Pipe serial(in, &d.chunks);
        serial.Add<SerialOnly>(in);
        serial.Finish(in);
        RunPlan(serial.plan, threads);
        EXPECT_EQ(serial.source->thread_hint(), 1U);
    }
}

TEST(ParallelExecution, ATableScanCutsItsMorselsByTheThreadHint) {
    const idx_t saved = MorselScan::DefaultMorselRows();
    MorselScan::SetDefaultMorselRows(0); // the built-in size, which only the hint can shrink
    Table table("t", {{"x", LogicalType::BigInt()}});
    for (idx_t at = 0; at < 15000; at += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, 15000 - at);
        DataChunk chunk;
        chunk.Initialize({LogicalType::BigInt()}, kVectorSize);
        for (idx_t i = 0; i < n; i++) {
            chunk.SetValue(0, i, Value::BigInt(static_cast<int64_t>(at + i)));
        }
        chunk.SetCardinality(n);
        table.Append(chunk);
    }
    PhysicalTableScan scan("t", table.Snapshot(), {0}, {}, {LogicalType::BigInt()});
    const auto morsels = [&](size_t hint) {
        scan.SetThreadHint(hint);
        const auto global = scan.GetGlobalSourceState(nullptr);
        return scan.MaxSourceThreads(*global);
    };
    EXPECT_EQ(morsels(1), 1U) << "no hint: the built-in 16,384-row morsel holds the whole table";
    EXPECT_EQ(morsels(2), 8U);
    EXPECT_EQ(morsels(16), 8U) << "never smaller than one vector";
    EXPECT_EQ(morsels(1), 1U) << "the hint can be changed again";
    MorselScan::SetDefaultMorselRows(saved == MorselScan::kDefaultMorselRows ? 0 : saved);
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

// A sink that only counts rows and does not declare itself parallel.
class SerialCountSink final : public PhysicalOperator {
  public:
    SerialCountSink() : PhysicalOperator({LogicalType::BigInt()}) {}
    std::string Name() const override { return "SERIAL_COUNT"; }
    struct Global final : GlobalSinkState {
        idx_t rows = 0;
    };
    struct Local final : LocalSinkState {};
    std::unique_ptr<GlobalSinkState> GetGlobalSinkState() override {
        return std::make_unique<Global>();
    }
    std::unique_ptr<LocalSinkState> GetLocalSinkState(GlobalSinkState&) override {
        return std::make_unique<Local>();
    }
    SinkResult Sink(GlobalSinkState& g, LocalSinkState&, const DataChunk& input) override {
        static_cast<Global&>(g).rows += input.size(); // unsynchronised: only correct on one thread
        return SinkResult::NeedMoreInput;
    }
    void Combine(GlobalSinkState&, LocalSinkState&) override {}
    void Finalize(GlobalSinkState&) override {}
};

TEST(ParallelExecution, ASerialOperatorAmongParallelOnesKeepsThePipelineOnOneThread) {
    Rng rng(31);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar()};
    const Data d = MakeData(rng, in, 80, SmallGen());
    Pipe p(in, &d.chunks);
    p.Add<PhysicalFilter>(in, Cmp(OperatorKind::Ge, Col(0, in[0]),
                                  BoundExpr::Constant(Value::Integer(-100)))); // parallel
    p.Add<SerialOnly>(in);                                                     // not
    p.Add<PhysicalProjection>(in, [&] {
        std::vector<BoundExprPtr> e;
        e.push_back(Col(0, in[0]));
        e.push_back(Col(1, in[1]));
        return e;
    }()); // parallel again
    p.Finish(in);
    RunPlan(p.plan, 8);
    EXPECT_EQ(p.source->participants(), 1) << "one operator that is not parallel decides";
}

TEST(ParallelExecution, ASinkThatIsNotParallelKeepsThePipelineOnOneThread) {
    Rng rng(32);
    const std::vector<LogicalType> in = {LogicalType::Integer()};
    const Data d = MakeData(rng, in, 120, SmallGen());
    PhysicalPlan plan;
    auto& src = plan.Make<ParallelChunkSource>(in, &d.chunks);
    auto& sink = plan.Make<SerialCountSink>();
    Pipeline p;
    p.source = &src;
    p.sink = &sink;
    plan.pipelines = {p};
    plan.root = &sink;
    TaskScheduler scheduler(8);
    Executor ex(plan, &scheduler);
    ex.Run();
    EXPECT_EQ(src.participants(), 1);
    EXPECT_EQ(static_cast<SerialCountSink::Global*>(ex.SinkState(sink))->rows, d.rows.size());
}

TEST(ParallelExecution, ChunksOfOneBatchStayTogetherAndInOrderAndBatchesComeInOrder) {
    Rng rng(33);
    const std::vector<LogicalType> in = {LogicalType::Integer(), LogicalType::Varchar()};
    const Data d = MakeData(rng, in, 400, SmallGen(), 40, false);
    for (const size_t per_batch : {size_t{1}, size_t{4}, size_t{7}}) {
        for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}}) {
            PhysicalPlan plan;
            auto& src = plan.Make<ParallelChunkSource>(in, &d.chunks, per_batch);
            auto& sink = plan.Make<PhysicalResultCollector>(in);
            Pipeline p;
            p.source = &src;
            p.sink = &sink;
            plan.pipelines = {p};
            plan.root = &sink;
            const Rows got = RunPlan(plan, threads);
            ASSERT_EQ(got.size(), d.rows.size());
            for (size_t i = 0; i < got.size(); i++) {
                ASSERT_TRUE(test::CompareTuples(got[i], d.rows[i]) == 0)
                    << "per batch " << per_batch << " threads " << threads << " row " << i;
            }
        }
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

// ---------------------------------------------------------------------------------- join

namespace {

class ScopedMinJoinRows {
  public:
    explicit ScopedMinJoinRows(idx_t rows) { PhysicalHashJoin::SetMinRowsToParallelize(rows); }
    ~ScopedMinJoinRows() { PhysicalHashJoin::SetMinRowsToParallelize(0); }
    ScopedMinJoinRows(const ScopedMinJoinRows&) = delete;
    ScopedMinJoinRows& operator=(const ScopedMinJoinRows&) = delete;
};

const std::vector<LogicalType> kLeftTypes = {LogicalType::Integer(), LogicalType::Varchar(),
                                             LogicalType::Double()};
const std::vector<LogicalType> kRightTypes = {LogicalType::Integer(), LogicalType::Varchar(),
                                              LogicalType::BigInt()};

// Join keys are column 0 (Integer, `domain` values) and column 1 (Varchar, small pool).
test::ValueGen JoinGen(int domain) {
    return [domain](Rng& rng, LogicalType t) {
        if (t.id() == TypeId::Integer) {
            return Chance(rng, 0.05) ? Value::Null(t)
                                     : Value::Integer(static_cast<int32_t>(
                                           RandBelow(rng, static_cast<uint64_t>(domain))));
        }
        return test::SmallDomainValue(rng, t, 0.1);
    };
}

// Reference: right rows indexed by key (a NULL in any key column never matches).
Rows ReferenceJoin(const Rows& left, const Rows& right, size_t key_count, PhysicalJoinType type) {
    std::map<std::vector<Value>, std::vector<size_t>, TupleLess> index;
    const auto key_of = [&](const std::vector<Value>& row) -> std::optional<std::vector<Value>> {
        std::vector<Value> key;
        for (size_t k = 0; k < key_count; k++) {
            if (row[k].IsNull()) {
                return std::nullopt;
            }
            key.push_back(row[k]);
        }
        return key;
    };
    for (size_t r = 0; r < right.size(); r++) {
        if (const auto key = key_of(right[r])) {
            index[*key].push_back(r);
        }
    }
    Rows out;
    for (const auto& l : left) {
        std::vector<size_t> matches;
        if (const auto key = key_of(l)) {
            if (const auto it = index.find(*key); it != index.end()) {
                matches = it->second;
            }
        }
        const bool any = !matches.empty();
        if (type == PhysicalJoinType::Inner || type == PhysicalJoinType::Left) {
            for (const size_t r : matches) {
                std::vector<Value> row = l;
                row.insert(row.end(), right[r].begin(), right[r].end());
                out.push_back(std::move(row));
            }
        }
        if (type == PhysicalJoinType::Left && !any) {
            std::vector<Value> row = l;
            for (const LogicalType& t : kRightTypes) {
                row.push_back(Value::Null(t));
            }
            out.push_back(std::move(row));
        } else if (type == PhysicalJoinType::Semi && any) {
            out.push_back(l);
        } else if (type == PhysicalJoinType::Anti && !any) {
            out.push_back(l);
        }
    }
    return out;
}

// build: right chunks -> join; probe: left chunks -> join -> collector. Returns the output rows.
Rows RunParallelJoin(const Data& left, const Data& right, size_t key_count, PhysicalJoinType type,
                     size_t threads) {
    PhysicalPlan plan;
    auto& lsrc = plan.Make<ParallelChunkSource>(kLeftTypes, &left.chunks);
    auto& rsrc = plan.Make<ParallelChunkSource>(kRightTypes, &right.chunks);
    std::vector<LogicalType> out = kLeftTypes;
    if (type == PhysicalJoinType::Inner || type == PhysicalJoinType::Left) {
        out.insert(out.end(), kRightTypes.begin(), kRightTypes.end());
    }
    std::vector<BoundExprPtr> lk, rk;
    for (size_t k = 0; k < key_count; k++) {
        lk.push_back(Col(k, kLeftTypes[k]));
        rk.push_back(Col(k, kRightTypes[k]));
    }
    auto& join = plan.Make<PhysicalHashJoin>(out, type, kLeftTypes, kRightTypes, std::move(lk),
                                             std::move(rk), nullptr);
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
    return RunPlan(plan, threads);
}

void ExpectSameRows(Rows got, Rows want, const std::string& what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    std::sort(got.begin(), got.end(), TupleLess());
    std::sort(want.begin(), want.end(), TupleLess());
    for (size_t i = 0; i < got.size(); i++) {
        for (size_t c = 0; c < got[i].size(); c++) {
            // equality consistent with the sort order (0.0 == -0.0), or ties could pair up
            // differently
            ASSERT_TRUE(test::CompareWithNulls(got[i][c], want[i][c]) == 0)
                << what << ": row " << i << " column " << c << ": " << got[i][c].ToString()
                << " vs " << want[i][c].ToString();
        }
    }
}

} // namespace

TEST(ParallelExecution, HashJoinMatchesTheReferenceForEveryJoinTypeThreadCountAndBuildStrategy) {
    Rng rng(11);
    for (const int domain : {60, 1500}) {
        const Data left = MakeData(rng, kLeftTypes, 16, JoinGen(domain), 150, false);
        const Data right = MakeData(rng, kRightTypes, 16, JoinGen(domain), 150, false);
        for (const size_t key_count : {size_t{1}, size_t{2}}) {
            for (const PhysicalJoinType type : {PhysicalJoinType::Inner, PhysicalJoinType::Left,
                                                PhysicalJoinType::Semi, PhysicalJoinType::Anti}) {
                const Rows want = ReferenceJoin(left.rows, right.rows, key_count, type);
                for (const idx_t min_rows : {idx_t{1}, idx_t{1} << 40}) {
                    const ScopedMinJoinRows build(min_rows);
                    for (const size_t threads : kThreadCounts) {
                        ExpectSameRows(RunParallelJoin(left, right, key_count, type, threads), want,
                                       "domain " + std::to_string(domain) + " keys " +
                                           std::to_string(key_count) + " type " +
                                           std::to_string(static_cast<int>(type)) +
                                           (min_rows == 1 ? " parallel build" : " serial build") +
                                           " threads " + std::to_string(threads));
                        if (::testing::Test::HasFailure()) {
                            return;
                        }
                    }
                }
            }
        }
    }
}

TEST(ParallelExecution, JoinWithEmptyAndAllNullBuildSides) {
    Rng rng(12);
    const Data left = MakeData(rng, kLeftTypes, 20, JoinGen(50), 150, false);
    Data empty;
    Data all_null;
    for (int c = 0; c < 5; c++) {
        DataChunk chunk;
        chunk.Initialize(kRightTypes);
        for (idx_t i = 0; i < 100; i++) {
            chunk.SetValue(0, i, Value::Null(kRightTypes[0]));
            chunk.SetValue(1, i, Value::Varchar("x"));
            chunk.SetValue(2, i, Value::BigInt(1));
        }
        chunk.SetCardinality(100);
        for (auto& r : RowsOf(chunk)) {
            all_null.rows.push_back(std::move(r));
        }
        all_null.chunks.push_back(std::move(chunk));
    }
    const ScopedMinJoinRows build(1);
    for (const Data* right : {&empty, &all_null}) {
        for (const PhysicalJoinType type : {PhysicalJoinType::Inner, PhysicalJoinType::Left,
                                            PhysicalJoinType::Semi, PhysicalJoinType::Anti}) {
            for (const size_t threads : kThreadCounts) {
                ExpectSameRows(RunParallelJoin(left, *right, 1, type, threads),
                               ReferenceJoin(left.rows, right->rows, 1, type),
                               "type " + std::to_string(static_cast<int>(type)) + " threads " +
                                   std::to_string(threads));
            }
        }
    }
}

// The parallel build splits the buckets into partitions: 4 per thread, rounded up to a power of
// two (64 for 16 threads), and a build of a handful of rows has 16 buckets. A partition is then
// less than one bucket wide, and the shift that maps a bucket to its partition had a negative
// amount (found by UBSan on a scalar subquery's one-row build, forced parallel by one-row
// morsels; the hardware shifts modulo 64, so the result was right anyway and only a sanitizer
// build sees it).
TEST(ParallelExecution, HashJoinBuildsOfAFewRowsOnManyThreads) {
    Rng rng(14);
    const ScopedMinJoinRows build(1);
    const Data left = MakeData(rng, kLeftTypes, 8, JoinGen(6), 150, false);
    for (const size_t rows : {1, 2, 3, 7, 8, 9, 16, 17, 40}) {
        Data right;
        right.chunks.push_back(test::RandomVariedChunk(rng, kRightTypes, rows, JoinGen(6)));
        right.rows = RowsOf(right.chunks.back());
        for (const size_t key_count : {size_t{1}, size_t{2}}) {
            for (const PhysicalJoinType type : {PhysicalJoinType::Inner, PhysicalJoinType::Left,
                                                PhysicalJoinType::Semi, PhysicalJoinType::Anti}) {
                const Rows want = ReferenceJoin(left.rows, right.rows, key_count, type);
                for (const size_t threads : {size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
                    ExpectSameRows(RunParallelJoin(left, right, key_count, type, threads), want,
                                   "build rows " + std::to_string(rows) + " keys " +
                                       std::to_string(key_count) + " type " +
                                       std::to_string(static_cast<int>(type)) + " threads " +
                                       std::to_string(threads));
                    if (::testing::Test::HasFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

TEST(ParallelExecution, TheSameJoinRunRepeatedlyGivesTheSameRows) {
    Rng rng(13);
    const Data left = MakeData(rng, kLeftTypes, 20, JoinGen(400), 150, false);
    const Data right = MakeData(rng, kRightTypes, 20, JoinGen(400), 150, false);
    const Rows want = ReferenceJoin(left.rows, right.rows, 1, PhysicalJoinType::Left);
    const ScopedMinJoinRows build(1);
    for (int round = 0; round < 15; round++) {
        ExpectSameRows(RunParallelJoin(left, right, 1, PhysicalJoinType::Left, 8), want,
                       "round " + std::to_string(round));
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

// ---------------------------------------------------------------------------------- sort

namespace {

class ScopedMinSortRows {
  public:
    explicit ScopedMinSortRows(idx_t rows) { SortBuffer::SetMinRowsToSortInParallel(rows); }
    ~ScopedMinSortRows() { SortBuffer::SetMinRowsToSortInParallel(0); }
    ScopedMinSortRows(const ScopedMinSortRows&) = delete;
    ScopedMinSortRows& operator=(const ScopedMinSortRows&) = delete;
};

// Columns: id BigInt (unique, so any order over (keys..., id) is total), k Integer (few values, so
// many ties), s Varchar, d Double (with NaN and -0.0).
const std::vector<LogicalType> kSortTypes = {LogicalType::BigInt(), LogicalType::Integer(),
                                             LogicalType::Varchar(), LogicalType::Double()};

Data MakeSortData(Rng& rng, int nchunks, idx_t max_rows, int key_domain) {
    Data d;
    int64_t next_id = 0;
    const test::ValueGen gen = [key_domain](Rng& r, LogicalType t) {
        if (t.id() == TypeId::Integer) {
            return Chance(r, 0.1) ? Value::Null(t)
                                  : Value::Integer(static_cast<int32_t>(
                                        RandBelow(r, static_cast<uint64_t>(key_domain))));
        }
        return test::SmallDomainValue(r, t, 0.1);
    };
    for (int k = 0; k < nchunks; k++) {
        const idx_t n = Chance(rng, 0.05) ? 0 : 1 + RandBelow(rng, max_rows);
        const DataChunk other =
            test::RandomVariedChunk(rng, {kSortTypes[1], kSortTypes[2], kSortTypes[3]}, n, gen);
        DataChunk chunk;
        chunk.Initialize(kSortTypes);
        for (idx_t i = 0; i < n; i++) {
            chunk.SetValue(0, i, Value::BigInt(next_id++));
        }
        for (idx_t c = 0; c < 3; c++) {
            chunk.column(c + 1).Reference(other.column(c));
        }
        chunk.SetCardinality(n);
        for (auto& r : RowsOf(chunk)) {
            d.rows.push_back(std::move(r));
        }
        d.chunks.push_back(std::move(chunk));
    }
    return d;
}

struct KeySpec {
    idx_t column;
    bool descending;
    bool nulls_first;
};

std::vector<SortKey> MakeKeys(const std::vector<KeySpec>& specs) {
    std::vector<SortKey> keys;
    for (const KeySpec& k : specs) {
        SortKey key;
        key.expr = Col(k.column, kSortTypes[k.column]);
        key.descending = k.descending;
        key.nulls_first = k.nulls_first;
        keys.push_back(std::move(key));
    }
    return keys;
}

bool RowBefore(const std::vector<Value>& a, const std::vector<Value>& b,
               const std::vector<KeySpec>& keys) {
    for (const KeySpec& k : keys) {
        const Value &x = a[k.column], &y = b[k.column];
        if (x.IsNull() || y.IsNull()) {
            if (x.IsNull() && y.IsNull()) {
                continue;
            }
            return x.IsNull() == k.nulls_first;
        }
        int c = Value::Compare(x, y);
        if (k.descending) {
            c = -c;
        }
        if (c != 0) {
            return c < 0;
        }
    }
    return false;
}

const std::vector<std::vector<KeySpec>> kKeyShapes = {
    {{1, false, false}, {0, false, false}},                  // k asc, id
    {{2, true, true}, {1, false, true}, {0, true, false}},   // s desc nulls first, k asc, id desc
    {{3, true, false}, {2, false, false}, {0, false, false}} // d desc (NaN, -0.0), s, id
};

// No unique last key: rows that compare equal must keep their input order (the order is stable),
// and the id column of the output shows whether they did.
const std::vector<std::vector<KeySpec>> kTieShapes = {
    {{1, false, false}},                 // k only: a handful of values, huge groups of ties
    {{2, true, true}, {1, false, true}}, // s desc nulls first, k asc
    {{3, false, true}},                  // d only: NaN and -0.0 tie with 0.0 groups
};

} // namespace

TEST(SortBuffer, ParallelSortGivesExactlyTheSerialStableOrder) {
    Rng rng(21);
    const ScopedMinSortRows parallel(1);
    std::vector<std::vector<KeySpec>> shapes = kKeyShapes;
    shapes.insert(shapes.end(), kTieShapes.begin(), kTieShapes.end());
    for (const auto& shape : shapes) {
        for (const idx_t rows_hint : {idx_t{0}, idx_t{1}, idx_t{3}, idx_t{2000}, idx_t{40000}}) {
            std::vector<SortSpec> specs;
            for (const KeySpec& k : shape) {
                specs.push_back({kSortTypes[k.column], k.descending, k.nulls_first});
            }
            const Data d = MakeSortData(
                rng, rows_hint == 0 ? 0 : 1 + static_cast<int>(rows_hint / 100), 200, 7);
            auto fill = [&](SortBuffer& buffer) {
                for (const DataChunk& chunk : d.chunks) {
                    DataChunk keys;
                    std::vector<LogicalType> key_types;
                    for (const KeySpec& k : shape) {
                        key_types.push_back(kSortTypes[k.column]);
                    }
                    keys.Initialize(key_types);
                    for (size_t i = 0; i < shape.size(); i++) {
                        keys.column(i).Reference(chunk.column(shape[i].column));
                    }
                    keys.SetCardinality(chunk.size());
                    buffer.Append(chunk, keys);
                }
            };
            SortBuffer serial(kSortTypes, specs);
            fill(serial);
            serial.Sort();
            DataChunk want_chunk;
            want_chunk.Initialize(kSortTypes);
            Rows want;
            for (idx_t at = 0; at < serial.Count(); at += kVectorSize) {
                serial.Scan(at, std::min<idx_t>(kVectorSize, serial.Count() - at), want_chunk);
                for (auto& r : RowsOf(want_chunk)) {
                    want.push_back(std::move(r));
                }
            }
            for (const size_t threads : {size_t{2}, size_t{3}, size_t{4}, size_t{8}, size_t{16}}) {
                SortBuffer buffer(kSortTypes, specs);
                fill(buffer);
                TaskScheduler scheduler(threads);
                const ExecutionContext context{&scheduler};
                buffer.SortParallel(context);
                ASSERT_EQ(buffer.Count(), serial.Count());
                DataChunk chunk;
                chunk.Initialize(kSortTypes);
                size_t at_row = 0;
                for (idx_t at = 0; at < buffer.Count(); at += kVectorSize) {
                    buffer.Scan(at, std::min<idx_t>(kVectorSize, buffer.Count() - at), chunk);
                    for (const auto& r : RowsOf(chunk)) {
                        for (size_t c = 0; c < r.size(); c++) {
                            ASSERT_TRUE(SameValue(r[c], want[at_row][c]))
                                << "rows " << d.rows.size() << " threads " << threads << " row "
                                << at_row << " column " << c;
                        }
                        at_row++;
                    }
                }
            }
        }
    }
}

TEST(ParallelExecution, OrderByMatchesTheReferenceOnEveryThreadCount) {
    Rng rng(22);
    const Data d = MakeSortData(rng, 80, 200, 6);
    for (const idx_t min_rows : {idx_t{1}, idx_t{1} << 40}) {
        const ScopedMinSortRows parallel(min_rows);
        for (const auto& shape : kKeyShapes) {
            Rows want = d.rows;
            std::sort(want.begin(), want.end(),
                      [&](const auto& a, const auto& b) { return RowBefore(a, b, shape); });
            for (const size_t threads : kThreadCounts) {
                PhysicalPlan plan;
                auto& src = plan.Make<ParallelChunkSource>(kSortTypes, &d.chunks);
                auto& order = plan.Make<PhysicalOrder>(kSortTypes, MakeKeys(shape));
                auto& result = plan.Make<PhysicalResultCollector>(kSortTypes);
                Pipeline p0;
                p0.source = &src;
                p0.sink = &order;
                Pipeline p1;
                p1.source = &order;
                p1.sink = &result;
                p1.dependencies = {0};
                plan.pipelines = {p0, p1};
                plan.root = &result;
                const Rows got = RunPlan(plan, threads);
                ASSERT_EQ(got.size(), want.size());
                for (size_t i = 0; i < got.size(); i++) {
                    ASSERT_TRUE(SameValue(got[i][0], want[i][0]))
                        << "threads " << threads << " row " << i << ": ids must come out in "
                        << "the order the (total) sort key defines";
                }
            }
        }
    }
}

TEST(ParallelExecution, OrderByWithTiesSortsByKeyAndKeepsEveryRow) {
    // No tie-breaking column: rows with equal keys may come out in any order, but never lost,
    // duplicated or interleaved with other keys.
    Rng rng(23);
    const Data d = MakeSortData(rng, 60, 200, 4);
    const std::vector<KeySpec> shape = {{1, false, false}};
    const ScopedMinSortRows parallel(1);
    for (const size_t threads : kThreadCounts) {
        PhysicalPlan plan;
        auto& src = plan.Make<ParallelChunkSource>(kSortTypes, &d.chunks);
        auto& order = plan.Make<PhysicalOrder>(kSortTypes, MakeKeys(shape));
        auto& result = plan.Make<PhysicalResultCollector>(kSortTypes);
        Pipeline p0;
        p0.source = &src;
        p0.sink = &order;
        Pipeline p1;
        p1.source = &order;
        p1.sink = &result;
        p1.dependencies = {0};
        plan.pipelines = {p0, p1};
        plan.root = &result;
        const Rows got = RunPlan(plan, threads);
        ASSERT_EQ(got.size(), d.rows.size());
        for (size_t i = 1; i < got.size(); i++) {
            ASSERT_FALSE(RowBefore(got[i], got[i - 1], shape))
                << "threads " << threads << " row " << i;
        }
        std::vector<int64_t> ids;
        for (const auto& r : got) {
            ids.push_back(r[0].GetBigInt());
        }
        std::sort(ids.begin(), ids.end());
        for (size_t i = 0; i < ids.size(); i++) {
            ASSERT_EQ(ids[i], static_cast<int64_t>(i)) << "a row was lost or duplicated";
        }
    }
}

TEST(ParallelExecution, TopNMatchesSortThenLimitOnEveryThreadCount) {
    Rng rng(24);
    const Data d = MakeSortData(rng, 80, 200, 6);
    const ScopedMinSortRows parallel(1);
    for (const auto& shape : kKeyShapes) {
        Rows sorted = d.rows;
        std::sort(sorted.begin(), sorted.end(),
                  [&](const auto& a, const auto& b) { return RowBefore(a, b, shape); });
        for (const auto& [limit, offset] : std::vector<std::pair<int64_t, int64_t>>{
                 {1, 0}, {10, 0}, {10, 7}, {500, 3}, {100000, 0}, {5, 100000}}) {
            const size_t begin = std::min<size_t>(static_cast<size_t>(offset), sorted.size());
            const size_t end = std::min(sorted.size(), begin + static_cast<size_t>(limit));
            for (const size_t threads : kThreadCounts) {
                PhysicalPlan plan;
                auto& src = plan.Make<ParallelChunkSource>(kSortTypes, &d.chunks);
                auto& top = plan.Make<PhysicalTopN>(kSortTypes, MakeKeys(shape), limit, offset);
                auto& result = plan.Make<PhysicalResultCollector>(kSortTypes);
                Pipeline p0;
                p0.source = &src;
                p0.sink = &top;
                Pipeline p1;
                p1.source = &top;
                p1.sink = &result;
                p1.dependencies = {0};
                plan.pipelines = {p0, p1};
                plan.root = &result;
                const Rows got = RunPlan(plan, threads);
                ASSERT_EQ(got.size(), end - begin) << "limit " << limit << " offset " << offset;
                for (size_t i = 0; i < got.size(); i++) {
                    ASSERT_TRUE(SameValue(got[i][0], sorted[begin + i][0]))
                        << "limit " << limit << " offset " << offset << " threads " << threads
                        << " row " << i;
                }
            }
        }
    }
}

} // namespace cdb
