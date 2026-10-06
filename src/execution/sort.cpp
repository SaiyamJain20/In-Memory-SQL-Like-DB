#include "execution/sort.h"

#include "execution/expression_executor.h"
#include "execution/type_dispatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <numeric>

namespace cdb {

namespace {

template <class T> int ThreeWay(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, double>) {
        const bool an = std::isnan(a), bn = std::isnan(b);
        if (an || bn) { // NaN sorts after every number and equals itself
            return an == bn ? 0 : (an ? 1 : -1);
        }
        return a < b ? -1 : (a > b ? 1 : 0);
    } else if constexpr (std::is_same_v<T, string_t>) {
        return string_t::Compare(a, b);
    } else {
        return a < b ? -1 : (a > b ? 1 : 0);
    }
}

std::vector<LogicalType> Concat(std::vector<LogicalType> a, const std::vector<SortSpec>& keys) {
    for (const SortSpec& k : keys) {
        a.push_back(k.type);
    }
    return a;
}

} // namespace

SortBuffer::SortBuffer(std::vector<LogicalType> payload_types, std::vector<SortSpec> keys)
    : payload_types_(std::move(payload_types)), keys_(std::move(keys)),
      store_(Concat(payload_types_, keys_)) {}

void SortBuffer::Append(const DataChunk& payload, const DataChunk& key_columns) {
    CDB_CHECK(payload.size() == key_columns.size());
    DataChunk combined;
    combined.Initialize(store_.types(), std::max<idx_t>(payload.size(), 1));
    for (idx_t c = 0; c < payload.ColumnCount(); c++) {
        combined.column(c).Reference(payload.column(c));
    }
    for (idx_t k = 0; k < key_columns.ColumnCount(); k++) {
        combined.column(payload.ColumnCount() + k).Reference(key_columns.column(k));
    }
    combined.SetCardinality(payload.size());
    store_.Append(combined);
}

void SortBuffer::AppendBuffer(const SortBuffer& other) {
    store_.AppendStore(other.store_);
}

void SortBuffer::AdoptFullChunks(SortBuffer& other) {
    store_.AdoptFullChunks(other.store_);
}

int SortBuffer::CompareRows(uint32_t a, uint32_t b) const {
    for (size_t k = 0; k < keys_.size(); k++) {
        const idx_t col = payload_types_.size() + k;
        const Vector& va = store_.chunk(a >> ChunkStore::kShift).column(col);
        const Vector& vb = store_.chunk(b >> ChunkStore::kShift).column(col);
        const idx_t oa = a & (kVectorSize - 1), ob = b & (kVectorSize - 1);
        const bool a_null = !va.Validity().IsValid(oa), b_null = !vb.Validity().IsValid(ob);
        if (a_null || b_null) {
            if (a_null && b_null) {
                continue;
            }
            return (a_null == keys_[k].nulls_first) ? -1 : 1;
        }
        int c = DispatchPhysical(keys_[k].type.physical(), [&](auto tag) {
            using T = decltype(tag);
            return ThreeWay<T>(va.FlatData<T>()[oa], vb.FlatData<T>()[ob]);
        });
        if (keys_[k].descending) {
            c = -c;
        }
        if (c != 0) {
            return c;
        }
    }
    return 0;
}

void SortBuffer::Sort() {
    order_.resize(store_.Count());
    for (idx_t i = 0; i < order_.size(); i++) {
        order_[i] = static_cast<uint32_t>(i);
    }
    std::stable_sort(order_.begin(), order_.end(),
                     [this](uint32_t a, uint32_t b) { return CompareRows(a, b) < 0; });
}

namespace {
constexpr idx_t kDefaultMinRowsToSortInParallel = 32768;

std::atomic<idx_t>& SortMinRowsSetting() {
    static std::atomic<idx_t> rows{[] {
        const char* env = std::getenv("CDB_SORT_PARALLEL_MIN_ROWS");
        const long long v = env != nullptr ? std::atoll(env) : 0;
        return v > 0 ? static_cast<idx_t>(v) : idx_t{0};
    }()};
    return rows;
}
} // namespace

idx_t SortBuffer::MinRowsToSortInParallel() noexcept {
    const idx_t set = SortMinRowsSetting().load(std::memory_order_relaxed);
    return set != 0 ? set : kDefaultMinRowsToSortInParallel;
}

void SortBuffer::SetMinRowsToSortInParallel(idx_t rows) noexcept {
    SortMinRowsSetting().store(rows, std::memory_order_relaxed);
}

void SortBuffer::SortParallel(const ExecutionContext& context) {
    const idx_t n = store_.Count();
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), uint32_t{0});
    const auto less = [this](uint32_t a, uint32_t b) { return CompareRows(a, b) < 0; };
    const size_t threads = context.threads();
    if (threads <= 1 || n < MinRowsToSortInParallel()) {
        std::stable_sort(order_.begin(), order_.end(), less);
        return;
    }

    // 1. Cut the rows into runs (a few per thread, to even out the work) and sort each run.
    size_t runs = threads * 2;
    runs = std::min<size_t>(runs, std::max<idx_t>(1, n / 1024));
    std::vector<idx_t> bounds(runs + 1);
    for (size_t r = 0; r <= runs; r++) {
        bounds[r] = static_cast<idx_t>(r * n / runs);
    }
    context.ParallelFor(runs, [&](size_t r) {
        std::stable_sort(order_.begin() + static_cast<long>(bounds[r]),
                         order_.begin() + static_cast<long>(bounds[r + 1]), less);
    });

    // 2. Merge neighbouring runs, round by round. A merge of A and B is cut into slices of the
    //    output: slice [k0, k1) takes the first i0..i1 elements of A and j0..j1 of B, where i is
    //    found by a binary search (the "co-rank" of k: how many of the first k outputs come from A,
    //    with ties going to A so the merge stays stable). Slices are independent tasks.
    std::vector<uint32_t> merged(n);
    struct Slice {
        idx_t a, a_len, b, b_len; // the two input ranges (b == a + a_len)
        idx_t k0, k1;             // the output slice, relative to a
    };
    while (bounds.size() > 2) {
        const size_t run_count = bounds.size() - 1;
        const size_t pairs = run_count / 2;
        const size_t slices_per_pair = std::max<size_t>(1, (threads * 4 + pairs - 1) / pairs);
        std::vector<Slice> slices;
        std::vector<idx_t> next_bounds = {0};
        for (size_t r = 0; r + 1 < run_count; r += 2) {
            const idx_t a = bounds[r], a_len = bounds[r + 1] - bounds[r];
            const idx_t b = bounds[r + 1], b_len = bounds[r + 2] - bounds[r + 1];
            const idx_t total = a_len + b_len;
            const size_t parts =
                std::max<size_t>(1, std::min<size_t>(slices_per_pair, total / 2048));
            for (size_t t = 0; t < parts; t++) {
                slices.push_back({a, a_len, b, b_len, static_cast<idx_t>(t * total / parts),
                                  static_cast<idx_t>((t + 1) * total / parts)});
            }
            next_bounds.push_back(bounds[r + 2]);
        }
        if (run_count % 2 == 1) { // an odd run out is carried over unchanged
            const idx_t a = bounds[run_count - 1];
            slices.push_back(
                {a, bounds[run_count] - a, bounds[run_count], 0, 0, bounds[run_count] - a});
            next_bounds.push_back(bounds[run_count]);
        }
        const auto corank = [&](const Slice& s, idx_t k) {
            idx_t lo = k > s.b_len ? k - s.b_len : 0;
            idx_t hi = std::min(k, s.a_len);
            while (lo < hi) {
                const idx_t i = lo + (hi - lo) / 2;
                const idx_t j = k - i;
                // B[j-1] < A[i]: A[i] must wait for it, so A gives at most i elements; otherwise
                // (A[i] <= B[j-1], ties go to A) A gives more
                if (j > 0 && i < s.a_len && !less(order_[s.b + j - 1], order_[s.a + i])) {
                    lo = i + 1;
                } else {
                    hi = i;
                }
            }
            return lo;
        };
        context.ParallelFor(slices.size(), [&](size_t t) {
            const Slice& s = slices[t];
            const idx_t i0 = corank(s, s.k0), i1 = corank(s, s.k1);
            const idx_t j0 = s.k0 - i0, j1 = s.k1 - i1;
            std::merge(order_.begin() + static_cast<long>(s.a + i0),
                       order_.begin() + static_cast<long>(s.a + i1),
                       order_.begin() + static_cast<long>(s.b + j0),
                       order_.begin() + static_cast<long>(s.b + j1),
                       merged.begin() + static_cast<long>(s.a + s.k0), less);
        });
        order_.swap(merged);
        bounds.swap(next_bounds);
    }
}

void SortBuffer::SortAndKeep(idx_t keep) {
    Sort();
    if (order_.size() <= keep) {
        return;
    }
    // Rebuild the store from the surviving rows so the discarded ones (and their string bytes)
    // are freed.
    ChunkStore kept(store_.types());
    DataChunk chunk;
    chunk.Initialize(store_.types());
    for (idx_t first = 0; first < keep; first += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, keep - first);
        chunk.Reset();
        for (idx_t c = 0; c < store_.ColumnCount(); c++) {
            store_.Gather(c, order_.data() + first, n, chunk.column(c));
        }
        chunk.SetCardinality(n);
        kept.Append(chunk);
    }
    store_ = std::move(kept);
    order_.clear();
    for (idx_t i = 0; i < keep; i++) { // already in sorted order
        order_.push_back(static_cast<uint32_t>(i));
    }
}

void SortBuffer::Scan(idx_t first, idx_t n, DataChunk& out) const {
    CDB_CHECK(order_.size() == store_.Count() && first + n <= order_.size());
    out.Reset();
    for (idx_t c = 0; c < payload_types_.size(); c++) {
        store_.Gather(c, order_.data() + first, n, out.column(c));
    }
    out.SetCardinality(n);
}

// ---------------------------------------------------------------------------------- ORDER BY

namespace {

struct OrderGlobalState final : GlobalSinkState {
    explicit OrderGlobalState(SortBuffer b) : buffer(std::move(b)) {}
    std::mutex mutex;
    std::vector<SortBuffer> locals; // each thread's rows, until Finalize
    SortBuffer buffer;              // all the rows, sorted
};

struct OrderLocalState final : LocalSinkState {
    OrderLocalState(SortBuffer b, const std::vector<SortKey>& keys) : buffer(std::move(b)) {
        for (const SortKey& k : keys) {
            executors.emplace_back(*k.expr);
            key_types.push_back(k.expr->type);
        }
        key_chunk.Initialize(key_types);
    }
    SortBuffer buffer;
    std::vector<ExpressionExecutor> executors;
    std::vector<LogicalType> key_types;
    DataChunk key_chunk;
};

// The sorted rows [first, last) cut into chunk-sized pieces claimed from an atomic cursor.
struct OrderSourceState final : GlobalSourceState {
    SortBuffer* buffer = nullptr;
    idx_t first = 0, last = 0;
    std::atomic<idx_t> next_chunk{0};
    idx_t ChunkCount() const { return (last - first + kVectorSize - 1) / kVectorSize; }
};
struct OrderLocalSource final : LocalSourceState {};

} // namespace

PhysicalOrder::PhysicalOrder(std::vector<LogicalType> types, std::vector<SortKey> keys)
    : PhysicalOperator(std::move(types)), keys_(std::move(keys)) {
    for (const SortKey& k : keys_) {
        specs_.push_back({k.expr->type, k.descending, k.nulls_first});
    }
}

std::string PhysicalOrder::Describe() const {
    std::string out = Name() + " [";
    for (size_t i = 0; i < keys_.size(); i++) {
        out += (i ? ", " : "") + keys_[i].expr->ToString() + (keys_[i].descending ? " DESC" : "");
    }
    return out + "]";
}

std::unique_ptr<GlobalSinkState> PhysicalOrder::GetGlobalSinkState() {
    return std::make_unique<OrderGlobalState>(SortBuffer(types(), specs_));
}

std::unique_ptr<LocalSinkState> PhysicalOrder::GetLocalSinkState(GlobalSinkState&) {
    return std::make_unique<OrderLocalState>(SortBuffer(types(), specs_), keys_);
}

SinkResult PhysicalOrder::Sink(GlobalSinkState&, LocalSinkState& state, const DataChunk& input) {
    auto& l = static_cast<OrderLocalState&>(state);
    for (size_t k = 0; k < l.executors.size(); k++) {
        l.executors[k].Execute(input, l.key_chunk.column(k));
    }
    l.key_chunk.SetCardinality(input.size());
    l.buffer.Append(input, l.key_chunk);
    AfterSink(l.buffer);
    return SinkResult::NeedMoreInput;
}

void PhysicalOrder::Combine(GlobalSinkState& global, LocalSinkState& local) {
    auto& g = static_cast<OrderGlobalState&>(global);
    auto& l = static_cast<OrderLocalState&>(local);
    ReduceLocal(l.buffer); // on this thread, outside the lock
    const std::lock_guard<std::mutex> lock(g.mutex);
    g.locals.push_back(std::move(l.buffer));
}

void PhysicalOrder::Finalize(GlobalSinkState& global) {
    ExecutionContext serial;
    FinalizeParallel(global, serial);
}

void PhysicalOrder::FinalizeParallel(GlobalSinkState& global, ExecutionContext& context) {
    auto& g = static_cast<OrderGlobalState&>(global);
    // One buffer from the threads' buffers: whole chunks are moved, only each partial last chunk
    // is copied.
    for (SortBuffer& local : g.locals) {
        g.buffer.AdoptFullChunks(local);
    }
    for (const SortBuffer& local : g.locals) {
        g.buffer.AppendBuffer(local);
    }
    g.locals.clear();
    SortFinal(g.buffer, context);
}

std::unique_ptr<GlobalSourceState> PhysicalOrder::GetGlobalSourceState(GlobalSinkState* sink) {
    auto s = std::make_unique<OrderSourceState>();
    s->buffer = &static_cast<OrderGlobalState*>(sink)->buffer;
    OutputRange(s->buffer->Count(), s->first, s->last);
    return s;
}

std::unique_ptr<LocalSourceState> PhysicalOrder::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<OrderLocalSource>();
}

idx_t PhysicalOrder::MaxSourceThreads(GlobalSourceState& global) const {
    return static_cast<OrderSourceState&>(global).ChunkCount();
}

bool PhysicalOrder::GetData(GlobalSourceState& global, LocalSourceState& local, DataChunk& out) {
    auto& s = static_cast<OrderSourceState&>(global);
    const idx_t chunk = s.next_chunk.fetch_add(1, std::memory_order_relaxed);
    if (chunk >= s.ChunkCount()) {
        return false;
    }
    const idx_t first = s.first + chunk * kVectorSize;
    s.buffer->Scan(first, std::min<idx_t>(kVectorSize, s.last - first), out);
    local.batch_index = chunk;
    return true;
}

// ---------------------------------------------------------------------------------- TOP N

PhysicalTopN::PhysicalTopN(std::vector<LogicalType> types, std::vector<SortKey> keys, int64_t limit,
                           int64_t offset)
    : PhysicalOrder(std::move(types), std::move(keys)), limit_(limit), offset_(offset) {}

std::string PhysicalTopN::Describe() const {
    return PhysicalOrder::Describe() + " LIMIT " + std::to_string(limit_) +
           (offset_ ? " OFFSET " + std::to_string(offset_) : "");
}

idx_t PhysicalTopN::Keep() const {
    // limit + offset rows can ever be needed; saturate instead of overflowing.
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max() / 2;
    return static_cast<idx_t>(std::min<int64_t>(limit_, kMax) + std::min<int64_t>(offset_, kMax));
}

void PhysicalTopN::AfterSink(SortBuffer& buffer) const {
    // Prune once the buffer holds a good deal more than we can ever need, so the cost per input
    // row stays O(log) amortised and memory stays bounded.
    const idx_t keep = Keep();
    if (buffer.Count() >= std::max<idx_t>(2 * keep, 4 * kVectorSize) &&
        buffer.Count() > keep + kVectorSize) {
        buffer.SortAndKeep(keep);
    }
}

void PhysicalTopN::ReduceLocal(SortBuffer& buffer) const {
    if (buffer.Count() > Keep()) {
        buffer.SortAndKeep(Keep());
    }
}

void PhysicalTopN::SortFinal(SortBuffer& buffer, const ExecutionContext&) const {
    buffer.SortAndKeep(Keep()); // at most threads x Keep() rows by now
}

void PhysicalTopN::OutputRange(idx_t total, idx_t& first, idx_t& last) const {
    first = std::min<idx_t>(static_cast<idx_t>(offset_), total);
    last = std::min<idx_t>(total, static_cast<idx_t>(offset_) + static_cast<idx_t>(limit_));
}

} // namespace cdb
