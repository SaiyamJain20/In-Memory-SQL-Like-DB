#include "execution/basic_operators.h"

#include "execution/expression_executor.h"
#include "planner/scalar_eval.h"

#include <algorithm>
#include <atomic>
#include <numeric>

namespace cdb {

namespace {

struct EmptyLocalSource final : LocalSourceState {};
struct EmptyLocalSink final : LocalSinkState {};

// Makes `output` show the rows sel[0..n) of `input` without copying: shared column buffers plus a
// selection. n == input.size() with no selection means "all rows".
void ShareRows(const DataChunk& input, DataChunk& output, const SelectionVector* sel, idx_t n) {
    for (idx_t c = 0; c < input.ColumnCount(); c++) {
        output.column(c).Reference(input.column(c));
    }
    if (sel) {
        output.Slice(*sel, n);
    } else {
        output.SetCardinality(n);
    }
}

} // namespace

// ---------------------------------------------------------------------------------- scan

PhysicalTableScan::PhysicalTableScan(std::string table_name,
                                     std::shared_ptr<const TableSnapshot> snapshot,
                                     std::vector<idx_t> column_ids,
                                     std::vector<TableFilter> filters,
                                     std::vector<LogicalType> types)
    : PhysicalOperator(std::move(types)), table_name_(std::move(table_name)),
      snapshot_(std::move(snapshot)), column_ids_(std::move(column_ids)),
      filters_(std::move(filters)) {}

std::string PhysicalTableScan::Describe() const {
    std::string out =
        "TABLE_SCAN " + table_name_ + " (" + std::to_string(column_ids_.size()) + " columns";
    if (!filters_.empty()) {
        out += ", " + std::to_string(filters_.size()) + " pruning filters";
    }
    return out + ")";
}

namespace {
struct ScanGlobalState final : GlobalSourceState {
    ScanGlobalState(std::shared_ptr<const TableSnapshot> snapshot, std::vector<idx_t> columns,
                    std::vector<TableFilter> filters, size_t threads)
        : scan(std::move(snapshot), std::move(columns), std::move(filters), 0, threads) {}
    MorselScan scan; // the cursor over morsels is shared by every thread
};
// The morsel a thread is working through (it reads one vector per GetData call).
struct ScanLocalState final : LocalSourceState {
    ScanMorsel morsel;
    bool has_morsel = false;
    idx_t next_vector = 0;
    idx_t vectors = 0;
};
} // namespace

std::unique_ptr<GlobalSourceState> PhysicalTableScan::GetGlobalSourceState(GlobalSinkState*) {
    return std::make_unique<ScanGlobalState>(snapshot_, column_ids_, filters_, threads_);
}
std::unique_ptr<LocalSourceState> PhysicalTableScan::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<ScanLocalState>();
}
idx_t PhysicalTableScan::MaxSourceThreads(GlobalSourceState& global) const {
    return static_cast<ScanGlobalState&>(global).scan.MorselCount();
}
bool PhysicalTableScan::GetData(GlobalSourceState& global, LocalSourceState& local,
                                DataChunk& out) {
    const MorselScan& scan = static_cast<ScanGlobalState&>(global).scan;
    auto& l = static_cast<ScanLocalState&>(local);
    if (!l.has_morsel) {
        if (!static_cast<ScanGlobalState&>(global).scan.Next(l.morsel)) {
            out.SetCardinality(0);
            return false;
        }
        l.has_morsel = true;
        l.next_vector = 0;
        l.vectors = MorselScan::VectorCount(l.morsel);
    }
    scan.ReadVector(l.morsel, l.next_vector++, out);
    l.batch_index = l.morsel.index;
    l.has_morsel = l.next_vector < l.vectors;
    return true;
}

// ---------------------------------------------------------------------------------- values

PhysicalValues::PhysicalValues(std::vector<LogicalType> types,
                               std::vector<std::vector<BoundExprPtr>> rows)
    : PhysicalOperator(std::move(types)), rows_(std::move(rows)) {}

std::string PhysicalValues::Describe() const {
    return "VALUES (" + std::to_string(rows_.size()) + " rows)";
}

namespace {
struct ValuesGlobalState final : GlobalSourceState {
    idx_t next = 0;
};
} // namespace

std::unique_ptr<GlobalSourceState> PhysicalValues::GetGlobalSourceState(GlobalSinkState*) {
    return std::make_unique<ValuesGlobalState>();
}
std::unique_ptr<LocalSourceState> PhysicalValues::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<EmptyLocalSource>();
}
bool PhysicalValues::GetData(GlobalSourceState& global, LocalSourceState&, DataChunk& out) {
    auto& g = static_cast<ValuesGlobalState&>(global);
    if (g.next >= rows_.size()) {
        return false;
    }
    const idx_t n = std::min<idx_t>(kVectorSize, rows_.size() - g.next);
    for (idx_t r = 0; r < n; r++) {
        const auto& row = rows_[g.next + r];
        for (idx_t c = 0; c < row.size(); c++) {
            out.SetValue(c, r, EvaluateConstant(*row[c]));
        }
    }
    out.SetCardinality(n);
    g.next += n;
    return true;
}

// ---------------------------------------------------------------------------------- filter

PhysicalFilter::PhysicalFilter(std::vector<LogicalType> types, BoundExprPtr predicate)
    : PhysicalOperator(std::move(types)), predicate_(std::move(predicate)) {}

std::string PhysicalFilter::Describe() const {
    return "FILTER " + predicate_->ToString();
}

namespace {
struct FilterState final : OperatorState {
    explicit FilterState(const BoundExpr& e) : executor(e) {}
    ExpressionExecutor executor;
    SelectionVector selection;
};
} // namespace

std::unique_ptr<OperatorState> PhysicalFilter::GetOperatorState(GlobalSinkState*) {
    return std::make_unique<FilterState>(*predicate_);
}

OperatorResult PhysicalFilter::Execute(OperatorState& state, const DataChunk& input,
                                       DataChunk& output) {
    auto& s = static_cast<FilterState&>(state);
    const idx_t n = s.executor.Select(input, s.selection);
    if (n == 0) {
        output.SetCardinality(0);
    } else if (n == input.size()) {
        ShareRows(input, output, nullptr, n);
    } else {
        ShareRows(input, output, &s.selection, n);
    }
    return OperatorResult::NeedMoreInput;
}

// ---------------------------------------------------------------------------------- projection

PhysicalProjection::PhysicalProjection(std::vector<LogicalType> types,
                                       std::vector<BoundExprPtr> exprs)
    : PhysicalOperator(std::move(types)), exprs_(std::move(exprs)) {
    CDB_CHECK(this->types().size() == exprs_.size());
}

std::string PhysicalProjection::Describe() const {
    std::string out = "PROJECTION [";
    for (size_t i = 0; i < exprs_.size(); i++) {
        out += (i ? ", " : "") + exprs_[i]->ToString();
    }
    return out + "]";
}

namespace {
struct ProjectionState final : OperatorState {
    std::vector<ExpressionExecutor> executors;
};
} // namespace

std::unique_ptr<OperatorState> PhysicalProjection::GetOperatorState(GlobalSinkState*) {
    auto state = std::make_unique<ProjectionState>();
    for (const auto& e : exprs_) {
        state->executors.emplace_back(*e);
    }
    return state;
}

OperatorResult PhysicalProjection::Execute(OperatorState& state, const DataChunk& input,
                                           DataChunk& output) {
    auto& s = static_cast<ProjectionState&>(state);
    for (size_t i = 0; i < s.executors.size(); i++) {
        s.executors[i].Execute(input, output.column(i));
    }
    output.SetCardinality(input.size());
    return OperatorResult::NeedMoreInput;
}

// ---------------------------------------------------------------------------------- limit

std::string PhysicalLimit::Describe() const {
    return "LIMIT " + (limit_ ? std::to_string(*limit_) : std::string("ALL")) +
           (offset_ ? " OFFSET " + std::to_string(offset_) : "");
}

namespace {
struct LimitState final : OperatorState {
    int64_t seen = 0;    // input rows consumed so far (including skipped ones)
    int64_t emitted = 0; // output rows produced so far
};
} // namespace

std::unique_ptr<OperatorState> PhysicalLimit::GetOperatorState(GlobalSinkState*) {
    return std::make_unique<LimitState>();
}

OperatorResult PhysicalLimit::Execute(OperatorState& state, const DataChunk& input,
                                      DataChunk& output) {
    auto& s = static_cast<LimitState&>(state);
    const auto n = static_cast<int64_t>(input.size());
    const int64_t begin = std::clamp<int64_t>(offset_ - s.seen, 0, n); // rows still to skip
    int64_t take = n - begin;
    if (limit_) {
        take = std::min(take, *limit_ - s.emitted);
    }
    s.seen += n;
    if (take <= 0) {
        output.SetCardinality(0);
    } else if (begin == 0 && take == n) {
        ShareRows(input, output, nullptr, input.size());
    } else {
        SelectionVector sel = SelectionVector::Uninitialized(static_cast<idx_t>(take));
        for (int64_t i = 0; i < take; i++) {
            sel.Set(static_cast<idx_t>(i), static_cast<sel_t>(begin + i));
        }
        ShareRows(input, output, &sel, static_cast<idx_t>(take));
    }
    s.emitted += std::max<int64_t>(take, 0);
    return limit_ && s.emitted >= *limit_ ? OperatorResult::Finished
                                          : OperatorResult::NeedMoreInput;
}

// ---------------------------------------------------------------------------------- result

namespace {
// A chunk and the batch it came from; sinks that must reproduce the single-threaded order sort by
// it.
using TaggedChunks = std::vector<std::pair<idx_t, DataChunk>>;

void MoveInto(TaggedChunks& from, TaggedChunks& to) {
    for (auto& c : from) {
        to.push_back(std::move(c));
    }
    from.clear();
}

// Stable: chunks of one batch come from one thread in order, and stay in that order.
void SortByBatch(TaggedChunks& chunks) {
    std::stable_sort(chunks.begin(), chunks.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
}

// A flat, owned copy of `input` (the pipeline reuses its buffers).
DataChunk CopyChunk(const std::vector<LogicalType>& types, const DataChunk& input) {
    DataChunk copy;
    copy.Initialize(types, input.size());
    for (idx_t c = 0; c < input.ColumnCount(); c++) {
        VectorOps::Copy(input.column(c), copy.column(c), nullptr, input.size());
    }
    copy.SetCardinality(input.size());
    return copy;
}

struct CollectGlobalState final : GlobalSinkState {
    std::mutex mutex;
    TaggedChunks chunks;
};
struct CollectLocalState final : LocalSinkState {
    TaggedChunks chunks;
};
} // namespace

std::unique_ptr<GlobalSinkState> PhysicalResultCollector::GetGlobalSinkState() {
    return std::make_unique<CollectGlobalState>();
}
std::unique_ptr<LocalSinkState> PhysicalResultCollector::GetLocalSinkState(GlobalSinkState&) {
    return std::make_unique<CollectLocalState>();
}
SinkResult PhysicalResultCollector::Sink(GlobalSinkState&, LocalSinkState& local,
                                         const DataChunk& input) {
    auto& l = static_cast<CollectLocalState&>(local);
    l.chunks.emplace_back(l.batch_index, CopyChunk(types(), input));
    return SinkResult::NeedMoreInput;
}
void PhysicalResultCollector::Combine(GlobalSinkState& global, LocalSinkState& local) {
    auto& g = static_cast<CollectGlobalState&>(global);
    auto& l = static_cast<CollectLocalState&>(local);
    const std::lock_guard<std::mutex> lock(g.mutex);
    MoveInto(l.chunks, g.chunks);
}
std::vector<DataChunk> PhysicalResultCollector::TakeChunks(GlobalSinkState& global) {
    TaggedChunks& tagged = static_cast<CollectGlobalState&>(global).chunks;
    SortByBatch(tagged);
    std::vector<DataChunk> out;
    out.reserve(tagged.size());
    for (auto& c : tagged) {
        out.push_back(std::move(c.second));
    }
    tagged.clear();
    return out;
}

// ---------------------------------------------------------------------------------- insert

namespace {
struct InsertGlobalState final : GlobalSinkState {
    std::unique_ptr<Table> staging;
    std::mutex mutex;
    TaggedChunks chunks; // every thread's rows, appended to `staging` in batch order by Finalize
    std::atomic<idx_t> rows{0};
};
struct InsertLocalState final : LocalSinkState {
    TaggedChunks chunks;
};
} // namespace

std::unique_ptr<GlobalSinkState> PhysicalInsert::GetGlobalSinkState() {
    auto g = std::make_unique<InsertGlobalState>();
    g->staging =
        std::make_unique<Table>("insert_staging", target_->schema(), target_->row_group_size());
    return g;
}
std::unique_ptr<LocalSinkState> PhysicalInsert::GetLocalSinkState(GlobalSinkState&) {
    return std::make_unique<InsertLocalState>();
}
SinkResult PhysicalInsert::Sink(GlobalSinkState& global, LocalSinkState& local,
                                const DataChunk& input) {
    auto& g = static_cast<InsertGlobalState&>(global);
    auto& l = static_cast<InsertLocalState&>(local);
    l.chunks.emplace_back(l.batch_index, CopyChunk(input.types(), input));
    g.rows += input.size();
    return SinkResult::NeedMoreInput;
}
void PhysicalInsert::Combine(GlobalSinkState& global, LocalSinkState& local) {
    auto& g = static_cast<InsertGlobalState&>(global);
    auto& l = static_cast<InsertLocalState&>(local);
    const std::lock_guard<std::mutex> lock(g.mutex);
    MoveInto(l.chunks, g.chunks);
}
void PhysicalInsert::Finalize(GlobalSinkState& global) {
    auto& g = static_cast<InsertGlobalState&>(global);
    SortByBatch(g.chunks);
    for (auto& c : g.chunks) {
        g.staging->Append(c.second); // enforces NOT NULL
        c.second = DataChunk();      // free the copy as soon as it is staged
    }
    g.chunks.clear();
    target_->Merge(std::move(g.staging));
}
idx_t PhysicalInsert::InsertedRows(GlobalSinkState& global) {
    return static_cast<InsertGlobalState&>(global).rows.load();
}

} // namespace cdb
