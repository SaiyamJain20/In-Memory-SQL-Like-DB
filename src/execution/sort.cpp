#include "execution/sort.h"

#include "execution/expression_executor.h"
#include "execution/type_dispatch.h"

#include <algorithm>
#include <cmath>
#include <mutex>

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
    SortBuffer buffer;
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

struct OrderSourceState final : GlobalSourceState {
    SortBuffer* buffer = nullptr;
    idx_t next = 0, last = 0;
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
    const std::lock_guard<std::mutex> lock(g.mutex);
    g.buffer.AppendBuffer(l.buffer);
}

void PhysicalOrder::Finalize(GlobalSinkState& global) {
    SortFinal(static_cast<OrderGlobalState&>(global).buffer);
}

std::unique_ptr<GlobalSourceState> PhysicalOrder::GetGlobalSourceState(GlobalSinkState* sink) {
    auto s = std::make_unique<OrderSourceState>();
    s->buffer = &static_cast<OrderGlobalState*>(sink)->buffer;
    OutputRange(s->buffer->Count(), s->next, s->last);
    return s;
}

std::unique_ptr<LocalSourceState> PhysicalOrder::GetLocalSourceState(GlobalSourceState&) {
    return std::make_unique<OrderLocalSource>();
}

bool PhysicalOrder::GetData(GlobalSourceState& global, LocalSourceState&, DataChunk& out) {
    auto& s = static_cast<OrderSourceState&>(global);
    if (s.next >= s.last) {
        return false;
    }
    const idx_t n = std::min<idx_t>(kVectorSize, s.last - s.next);
    s.buffer->Scan(s.next, n, out);
    s.next += n;
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

void PhysicalTopN::SortFinal(SortBuffer& buffer) const {
    buffer.SortAndKeep(Keep());
}

void PhysicalTopN::OutputRange(idx_t total, idx_t& first, idx_t& last) const {
    first = std::min<idx_t>(static_cast<idx_t>(offset_), total);
    last = std::min<idx_t>(total, static_cast<idx_t>(offset_) + static_cast<idx_t>(limit_));
}

} // namespace cdb
