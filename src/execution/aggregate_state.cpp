#include "execution/aggregate_state.h"

#include "common/error.h"
#include "execution/key_index.h"
#include "execution/type_dispatch.h"

#include <cmath>

namespace cdb {

namespace {

[[noreturn]] void SumOverflow() {
    throw Error(ErrorCode::Execution, "Out of Range: overflow in SUM");
}

// Strict "less" in the engine's total order (NaN sorts after every number).
template <class T> bool Less(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, double>) {
        return a < b || (!std::isnan(a) && std::isnan(b));
    } else {
        return a < b;
    }
}

// ---------------------------------------------------------------------------------- COUNT

class CountStarState final : public AggregateState {
  public:
    void Resize(idx_t groups) override { counts_.resize(groups, 0); }
    void Update(const uint32_t* groups, const Vector*, idx_t count) override {
        for (idx_t i = 0; i < count; i++) {
            counts_[groups[i]]++;
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const CountStarState&>(src);
        for (idx_t g = 0; g < n; g++) {
            counts_[dst[g]] += s.counts_[g];
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        int64_t* o = out.FlatData<int64_t>();
        for (idx_t i = 0; i < count; i++) {
            o[i] = counts_[first + i];
        }
    }

  private:
    std::vector<int64_t> counts_;
};

class CountState final : public AggregateState {
  public:
    void Resize(idx_t groups) override { counts_.resize(groups, 0); }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        if (u.validity->AllValid()) {
            for (idx_t i = 0; i < count; i++) {
                counts_[groups[i]]++;
            }
            return;
        }
        for (idx_t i = 0; i < count; i++) {
            if (u.IsValid(i)) {
                counts_[groups[i]]++;
            }
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const CountState&>(src);
        for (idx_t g = 0; g < n; g++) {
            counts_[dst[g]] += s.counts_[g];
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        int64_t* o = out.FlatData<int64_t>();
        for (idx_t i = 0; i < count; i++) {
            o[i] = counts_[first + i];
        }
    }

  private:
    std::vector<int64_t> counts_;
};

// ---------------------------------------------------------------------------------- SUM / AVG

// SUM over integers: 64-bit accumulator, overflow is an error (never a wrapped result).
template <class In> class SumIntState final : public AggregateState {
  public:
    void Resize(idx_t groups) override {
        sums_.resize(groups, 0);
        has_.resize(groups, 0);
    }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        const In* data = u.Data<In>();
        for (idx_t i = 0; i < count; i++) {
            if (!u.IsValid(i)) {
                continue;
            }
            const uint32_t g = groups[i];
            if (__builtin_add_overflow(sums_[g], static_cast<int64_t>(data[u.sel[i]]), &sums_[g])) {
                SumOverflow();
            }
            has_[g] = 1;
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const SumIntState&>(src);
        for (idx_t g = 0; g < n; g++) {
            if (!s.has_[g]) {
                continue;
            }
            if (__builtin_add_overflow(sums_[dst[g]], s.sums_[g], &sums_[dst[g]])) {
                SumOverflow();
            }
            has_[dst[g]] = 1;
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        int64_t* o = out.FlatData<int64_t>();
        for (idx_t i = 0; i < count; i++) {
            if (has_[first + i]) {
                o[i] = sums_[first + i];
            } else {
                out.Validity().SetInvalid(i);
            }
        }
    }

  private:
    std::vector<int64_t> sums_;
    std::vector<uint8_t> has_;
};

class SumDoubleState final : public AggregateState {
  public:
    void Resize(idx_t groups) override {
        sums_.resize(groups, 0.0);
        has_.resize(groups, 0);
    }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        const double* data = u.Data<double>();
        for (idx_t i = 0; i < count; i++) {
            if (u.IsValid(i)) {
                sums_[groups[i]] += data[u.sel[i]];
                has_[groups[i]] = 1;
            }
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const SumDoubleState&>(src);
        for (idx_t g = 0; g < n; g++) {
            if (s.has_[g]) {
                sums_[dst[g]] += s.sums_[g];
                has_[dst[g]] = 1;
            }
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        double* o = out.FlatData<double>();
        for (idx_t i = 0; i < count; i++) {
            if (has_[first + i]) {
                o[i] = sums_[first + i];
            } else {
                out.Validity().SetInvalid(i);
            }
        }
    }

  private:
    std::vector<double> sums_;
    std::vector<uint8_t> has_;
};

template <class In> class AvgState final : public AggregateState {
  public:
    void Resize(idx_t groups) override {
        sums_.resize(groups, 0.0);
        counts_.resize(groups, 0);
    }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        const In* data = u.Data<In>();
        for (idx_t i = 0; i < count; i++) {
            if (u.IsValid(i)) {
                sums_[groups[i]] += static_cast<double>(data[u.sel[i]]);
                counts_[groups[i]]++;
            }
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const AvgState&>(src);
        for (idx_t g = 0; g < n; g++) {
            sums_[dst[g]] += s.sums_[g];
            counts_[dst[g]] += s.counts_[g];
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        double* o = out.FlatData<double>();
        for (idx_t i = 0; i < count; i++) {
            if (counts_[first + i] > 0) {
                o[i] = sums_[first + i] / static_cast<double>(counts_[first + i]);
            } else {
                out.Validity().SetInvalid(i);
            }
        }
    }

  private:
    std::vector<double> sums_;
    std::vector<int64_t> counts_;
};

// ---------------------------------------------------------------------------------- MIN / MAX

// Keeps the first value among equals (so -0.0 vs 0.0 is stable), like a left-to-right scan.
template <class T, bool kMax> class MinMaxState final : public AggregateState {
    using Store = std::conditional_t<std::is_same_v<T, bool>, uint8_t, T>;

  public:
    void Resize(idx_t groups) override {
        values_.resize(groups);
        has_.resize(groups, 0);
    }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        const T* data = u.Data<T>();
        for (idx_t i = 0; i < count; i++) {
            if (u.IsValid(i)) {
                Offer(groups[i], data[u.sel[i]]);
            }
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const MinMaxState&>(src);
        for (idx_t g = 0; g < n; g++) {
            if (s.has_[g]) {
                Offer(dst[g], static_cast<T>(s.values_[g]));
            }
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        T* o = out.FlatData<T>();
        for (idx_t i = 0; i < count; i++) {
            if (has_[first + i]) {
                o[i] = static_cast<T>(values_[first + i]);
            } else {
                out.Validity().SetInvalid(i);
            }
        }
    }

  private:
    void Offer(uint32_t g, const T& v) {
        if (!has_[g] || (kMax ? Less<T>(static_cast<T>(values_[g]), v)
                              : Less<T>(v, static_cast<T>(values_[g])))) {
            values_[g] = v;
            has_[g] = 1;
        }
    }
    std::vector<Store> values_; // (bool is stored as a byte: vector<bool> has no references)
    std::vector<uint8_t> has_;
};

template <bool kMax> class MinMaxStringState final : public AggregateState {
  public:
    void Resize(idx_t groups) override {
        values_.resize(groups);
        has_.resize(groups, 0);
    }
    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        UnifiedFormat u;
        arg->ToUnified(u);
        const string_t* data = u.Data<string_t>();
        for (idx_t i = 0; i < count; i++) {
            if (u.IsValid(i)) {
                Offer(groups[i], data[u.sel[i]].view());
            }
        }
    }
    void Combine(const AggregateState& src, const uint32_t* dst, idx_t n) override {
        const auto& s = static_cast<const MinMaxStringState&>(src);
        for (idx_t g = 0; g < n; g++) {
            if (s.has_[g]) {
                Offer(dst[g], s.values_[g]);
            }
        }
    }
    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        string_t* o = out.FlatData<string_t>();
        for (idx_t i = 0; i < count; i++) {
            if (has_[first + i]) {
                o[i] = out.AddString(values_[first + i]);
            } else {
                out.Validity().SetInvalid(i);
            }
        }
    }

  private:
    void Offer(uint32_t g, std::string_view v) {
        if (!has_[g] || (kMax ? values_[g] < v : v < values_[g])) {
            values_[g].assign(v);
            has_[g] = 1;
        }
    }
    std::vector<std::string> values_;
    std::vector<uint8_t> has_;
};

// ---------------------------------------------------------------------------------- DISTINCT

// agg(DISTINCT x): only the first occurrence of each (group, value) reaches the inner aggregate.
// The (group, value) pairs seen are kept in a KeyIndex; merging two states replays one's pairs
// into the other.
class DistinctState final : public AggregateState {
  public:
    DistinctState(std::unique_ptr<AggregateState> inner, LogicalType arg_type)
        : inner_(std::move(inner)), arg_type_(arg_type), index_({LogicalType::BigInt(), arg_type}) {
        keys_.Initialize({LogicalType::BigInt(), arg_type});
    }

    void Resize(idx_t groups) override { inner_->Resize(groups); }

    void Update(const uint32_t* groups, const Vector* arg, idx_t count) override {
        for (idx_t done = 0; done < count; done += kVectorSize) {
            const idx_t n = std::min(count - done, kVectorSize);
            UpdateBatch(groups + done, *arg, done, n);
        }
    }

    void Combine(const AggregateState& src, const uint32_t* dst, idx_t) override {
        const auto& s = static_cast<const DistinctState&>(src);
        std::vector<uint32_t> groups(kVectorSize);
        for (idx_t c = 0; c < s.index_.keys().ChunkCount(); c++) {
            const DataChunk& chunk = s.index_.keys().chunk(c);
            const int64_t* src_groups = chunk.column(0).FlatData<int64_t>();
            for (idx_t i = 0; i < chunk.size(); i++) {
                groups[i] = dst[src_groups[i]];
            }
            UpdateBatch(groups.data(), chunk.column(1), 0, chunk.size());
        }
    }

    void Finalize(idx_t first, idx_t count, Vector& out) const override {
        inner_->Finalize(first, count, out);
    }

  private:
    // Rows [offset, offset + n) of `arg` belong to groups[0..n).
    void UpdateBatch(const uint32_t* groups, const Vector& arg, idx_t offset, idx_t n) {
        keys_.Reset();
        int64_t* g = keys_.column(0).FlatData<int64_t>();
        for (idx_t i = 0; i < n; i++) {
            g[i] = groups[i];
        }
        SelectionVector window(n);
        for (idx_t i = 0; i < n; i++) {
            window.Set(i, static_cast<sel_t>(offset + i));
        }
        Vector view(arg_type_);
        view.Reference(arg);
        view.Slice(window, n);
        keys_.column(1).Reference(view);
        keys_.SetCardinality(n);

        ids_.resize(n);
        new_rows_.clear();
        if (index_.FindOrInsert(keys_, n, ids_.data(), &new_rows_) == 0) {
            return;
        }
        const idx_t m = new_rows_.size();
        SelectionVector sel(m);
        std::vector<uint32_t> new_groups(m);
        for (idx_t j = 0; j < m; j++) {
            sel.Set(j, new_rows_[j]);
            new_groups[j] = groups[new_rows_[j]];
        }
        Vector fresh(arg_type_, std::max<idx_t>(m, 1));
        VectorOps::Copy(keys_.column(1), fresh, &sel, m);
        inner_->Update(new_groups.data(), &fresh, m);
    }

    std::unique_ptr<AggregateState> inner_;
    LogicalType arg_type_;
    KeyIndex index_;
    DataChunk keys_; // scratch: (group, value) pairs of the batch being processed
    std::vector<uint32_t> ids_;
    std::vector<sel_t> new_rows_;
};

template <bool kMax> std::unique_ptr<AggregateState> MakeMinMax(LogicalType t) {
    switch (t.physical()) {
    case PhysicalType::Bool:
        return std::make_unique<MinMaxState<bool, kMax>>();
    case PhysicalType::Int32:
        return std::make_unique<MinMaxState<int32_t, kMax>>();
    case PhysicalType::Int64:
        return std::make_unique<MinMaxState<int64_t, kMax>>();
    case PhysicalType::Double:
        return std::make_unique<MinMaxState<double, kMax>>();
    case PhysicalType::String:
        return std::make_unique<MinMaxStringState<kMax>>();
    }
    CDB_UNREACHABLE("MakeMinMax");
}

} // namespace

LogicalType AggregateResultType(const AggregateSpec& spec) {
    switch (spec.kind) {
    case AggregateKind::Count:
    case AggregateKind::CountStar:
        return LogicalType::BigInt();
    case AggregateKind::Sum:
        return spec.arg_type.id() == TypeId::Double ? LogicalType::Double() : LogicalType::BigInt();
    case AggregateKind::Avg:
        return LogicalType::Double();
    case AggregateKind::Min:
    case AggregateKind::Max:
        return spec.arg_type;
    }
    CDB_UNREACHABLE("AggregateResultType");
}

std::unique_ptr<AggregateState> MakeAggregateState(const AggregateSpec& spec) {
    std::unique_ptr<AggregateState> state;
    const PhysicalType p = spec.arg_type.physical();
    switch (spec.kind) {
    case AggregateKind::CountStar:
        return std::make_unique<CountStarState>(); // DISTINCT is meaningless for COUNT(*)
    case AggregateKind::Count:
        state = std::make_unique<CountState>();
        break;
    case AggregateKind::Sum:
        if (p == PhysicalType::Double) {
            state = std::make_unique<SumDoubleState>();
        } else if (p == PhysicalType::Int32) {
            state = std::make_unique<SumIntState<int32_t>>();
        } else if (p == PhysicalType::Int64) {
            state = std::make_unique<SumIntState<int64_t>>();
        } else {
            throw Error(ErrorCode::Type, "sum() requires a numeric argument");
        }
        break;
    case AggregateKind::Avg:
        if (p == PhysicalType::Double) {
            state = std::make_unique<AvgState<double>>();
        } else if (p == PhysicalType::Int32) {
            state = std::make_unique<AvgState<int32_t>>();
        } else if (p == PhysicalType::Int64) {
            state = std::make_unique<AvgState<int64_t>>();
        } else {
            throw Error(ErrorCode::Type, "avg() requires a numeric argument");
        }
        break;
    case AggregateKind::Min:
        state = MakeMinMax<false>(spec.arg_type);
        break;
    case AggregateKind::Max:
        state = MakeMinMax<true>(spec.arg_type);
        break;
    }
    if (spec.distinct) {
        return std::make_unique<DistinctState>(std::move(state), spec.arg_type);
    }
    return state;
}

} // namespace cdb
