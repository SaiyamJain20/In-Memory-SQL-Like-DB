#include "execution/hash_join.h"

#include "execution/chunk_store.h"
#include "execution/expression_executor.h"
#include "execution/hashing.h"
#include "execution/key_index.h"

#include <algorithm>
#include <mutex>
#include <optional>

namespace cdb {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFU; // end of a chain / no candidate
constexpr uint32_t kNotStarted =
    0xFFFFFFFEU; // the current probe row has not looked up its chain yet

struct JoinGlobalState final : GlobalSinkState {
    JoinGlobalState(std::vector<LogicalType> payload_types, std::vector<LogicalType> key_types)
        : payload(std::move(payload_types)), keys(std::move(key_types)) {}
    std::mutex mutex;
    ChunkStore payload; // build rows (all right columns)
    ChunkStore keys;    // their evaluated join keys, same row ids (no columns for key-less joins)
    // One entry per build row: its hash and the next row of its bucket's chain, side by side so
    // that following a chain touches one cache line per row, not two.
    struct Entry {
        uint64_t hash;
        uint32_t next; // next build row in the chain, or kNone
        uint32_t pad;
    };
    std::vector<Entry> entries;
    std::vector<uint32_t> heads; // bucket -> first build row, or kNone
    uint64_t mask = 0;
};

struct BuildLocalState final : LocalSinkState {
    BuildLocalState(std::vector<LogicalType> payload_types, std::vector<LogicalType> key_types)
        : payload(std::move(payload_types)), keys(std::move(key_types)) {}
    ChunkStore payload;
    ChunkStore keys;
    std::vector<ExpressionExecutor> key_executors;
    DataChunk key_chunk;
};

struct ProbeState final : OperatorState {
    const JoinGlobalState* build = nullptr;
    std::vector<ExpressionExecutor> key_executors;
    std::optional<ExpressionExecutor> residual;
    DataChunk key_chunk;
    std::optional<KeyComparator> comparator;
    std::vector<uint64_t> hashes;
    std::vector<uint32_t>
        first; // per probe row: head of its bucket's chain (kNone if none/NULL key)
    std::vector<uint8_t> key_ok; // probe row has no NULL key
    std::vector<uint8_t> matched;

    bool active = false; // an input chunk is being processed
    enum class Phase { Matching, Unmatched } phase = Phase::Matching;
    idx_t pos = 0;
    uint32_t chain = kNotStarted;

    std::vector<uint32_t> probe_rows, build_rows; // the current batch of candidate pairs
    DataChunk joined;                             // left ++ right columns of a batch
    SelectionVector selection;
};

// Makes columns [0, input.ColumnCount()) of `out` show rows sel[0..n) of `input`, zero-copy.
void ShareLeft(const DataChunk& input, DataChunk& out, const SelectionVector& sel, idx_t n) {
    for (idx_t c = 0; c < input.ColumnCount(); c++) {
        out.column(c).Reference(input.column(c));
        out.column(c).Slice(sel, n);
    }
}

} // namespace

PhysicalHashJoin::PhysicalHashJoin(std::vector<LogicalType> types, PhysicalJoinType join_type,
                                   std::vector<LogicalType> left_types,
                                   std::vector<LogicalType> right_types,
                                   std::vector<BoundExprPtr> left_keys,
                                   std::vector<BoundExprPtr> right_keys, BoundExprPtr residual)
    : PhysicalOperator(std::move(types)), join_type_(join_type), left_types_(std::move(left_types)),
      right_types_(std::move(right_types)), left_keys_(std::move(left_keys)),
      right_keys_(std::move(right_keys)), residual_(std::move(residual)) {
    CDB_CHECK(left_keys_.size() == right_keys_.size());
    for (size_t k = 0; k < left_keys_.size(); k++) {
        CDB_CHECK(left_keys_[k]->type ==
                  right_keys_[k]->type); // the planner casts to a common type
    }
    const bool keep_right =
        join_type_ == PhysicalJoinType::Inner || join_type_ == PhysicalJoinType::Left;
    CDB_CHECK(this->types().size() == left_types_.size() + (keep_right ? right_types_.size() : 0));
}

std::string PhysicalHashJoin::Name() const {
    return left_keys_.empty() ? "NESTED_LOOP_JOIN" : "HASH_JOIN";
}

std::string PhysicalHashJoin::Describe() const {
    static const char* const kNames[] = {"INNER", "LEFT", "SEMI", "ANTI"};
    std::string out = Name() + " " + kNames[static_cast<int>(join_type_)];
    for (size_t k = 0; k < left_keys_.size(); k++) {
        out +=
            (k ? " AND " : " ON ") + left_keys_[k]->ToString() + " = " + right_keys_[k]->ToString();
    }
    if (residual_) {
        out += " WHERE " + residual_->ToString();
    }
    return out;
}

// ---------------------------------------------------------------------------------- build

namespace {
std::vector<LogicalType> KeyTypes(const std::vector<BoundExprPtr>& keys) {
    std::vector<LogicalType> t;
    for (const auto& k : keys) {
        t.push_back(k->type);
    }
    return t;
}
} // namespace

std::unique_ptr<GlobalSinkState> PhysicalHashJoin::GetGlobalSinkState() {
    return std::make_unique<JoinGlobalState>(right_types_, KeyTypes(right_keys_));
}

std::unique_ptr<LocalSinkState> PhysicalHashJoin::GetLocalSinkState(GlobalSinkState&) {
    auto local = std::make_unique<BuildLocalState>(right_types_, KeyTypes(right_keys_));
    for (const auto& k : right_keys_) {
        local->key_executors.emplace_back(*k);
    }
    local->key_chunk.Initialize(KeyTypes(right_keys_));
    return local;
}

SinkResult PhysicalHashJoin::Sink(GlobalSinkState&, LocalSinkState& state, const DataChunk& input) {
    auto& l = static_cast<BuildLocalState&>(state);
    const idx_t n = input.size();
    if (right_keys_.empty()) {
        l.payload.Append(input);
        return SinkResult::NeedMoreInput;
    }
    for (size_t k = 0; k < l.key_executors.size(); k++) {
        l.key_executors[k].Execute(input, l.key_chunk.column(k));
    }
    l.key_chunk.SetCardinality(n);
    // Rows with a NULL in any key can never match: leave them out of the build side.
    std::vector<UnifiedFormat> formats(right_keys_.size());
    for (size_t k = 0; k < formats.size(); k++) {
        l.key_chunk.column(k).ToUnified(formats[k]);
    }
    SelectionVector keep(n);
    idx_t kept = 0;
    for (idx_t i = 0; i < n; i++) {
        bool ok = true;
        for (const UnifiedFormat& f : formats) {
            ok = ok && f.IsValid(i);
        }
        if (ok) {
            keep.Set(kept++, static_cast<sel_t>(i));
        }
    }
    if (kept == n) {
        l.payload.Append(input);
        l.keys.Append(l.key_chunk);
    } else if (kept > 0) {
        l.payload.Append(input, &keep, kept);
        l.keys.Append(l.key_chunk, &keep, kept);
    }
    return SinkResult::NeedMoreInput;
}

void PhysicalHashJoin::Combine(GlobalSinkState& global, LocalSinkState& local) {
    auto& g = static_cast<JoinGlobalState&>(global);
    auto& l = static_cast<BuildLocalState&>(local);
    const std::lock_guard<std::mutex> lock(g.mutex);
    if (g.payload.Count() == 0 && g.keys.Count() == 0) {
        g.payload = std::move(l.payload);
        g.keys = std::move(l.keys);
    } else {
        g.payload.AppendStore(l.payload);
        if (!right_keys_.empty()) {
            g.keys.AppendStore(l.keys);
        }
    }
}

void PhysicalHashJoin::Finalize(GlobalSinkState& global) {
    auto& g = static_cast<JoinGlobalState&>(global);
    const idx_t n = g.payload.Count();
    if (n >= kNotStarted) {
        throw Error(ErrorCode::NotImplemented, "join build side has more than 4 billion rows");
    }
    if (right_keys_.empty()) {
        return; // nested loop: every build row is a candidate, no index needed
    }
    CDB_CHECK(g.keys.Count() == n);
    g.entries.resize(n);
    std::vector<const Vector*> cols(right_keys_.size());
    uint64_t chunk_hashes[kVectorSize];
    for (idx_t c = 0; c < g.keys.ChunkCount(); c++) {
        const DataChunk& chunk = g.keys.chunk(c);
        for (idx_t k = 0; k < cols.size(); k++) {
            cols[k] = &chunk.column(k);
        }
        HashColumns(cols.data(), cols.size(), chunk.size(), chunk_hashes);
        for (idx_t i = 0; i < chunk.size(); i++) {
            g.entries[c * kVectorSize + i].hash = chunk_hashes[i];
            g.entries[c * kVectorSize + i].pad = 0;
        }
    }
    idx_t buckets = 16;
    while (buckets < 2 * n) {
        buckets *= 2;
    }
    g.mask = buckets - 1;
    g.heads.assign(buckets, kNone);
    // Insert in reverse so each chain lists rows in build order.
    for (idx_t row = n; row-- > 0;) {
        uint32_t& head = g.heads[g.entries[row].hash & g.mask];
        g.entries[row].next = head;
        head = static_cast<uint32_t>(row);
    }
}

// ---------------------------------------------------------------------------------- probe

std::unique_ptr<OperatorState> PhysicalHashJoin::GetOperatorState(GlobalSinkState* sink_state) {
    auto s = std::make_unique<ProbeState>();
    s->build = static_cast<const JoinGlobalState*>(sink_state);
    for (const auto& k : left_keys_) {
        s->key_executors.emplace_back(*k);
    }
    s->key_chunk.Initialize(KeyTypes(left_keys_));
    if (residual_) {
        s->residual.emplace(*residual_);
    }
    std::vector<LogicalType> joined = left_types_;
    joined.insert(joined.end(), right_types_.begin(), right_types_.end());
    s->joined.Initialize(joined);
    return s;
}

// The probe loop's expensive part is memory latency: bucket head -> chain entry -> key cells are
// dependent loads from tables much larger than the cache, one probe row after another. Looking
// every row's bucket up front, and prefetching what the candidates need, lets those misses overlap
// (the CPU keeps ~10-20 in flight) instead of being paid one at a time.
void Prefetch(ProbeState& st, const JoinGlobalState& build, idx_t n) {
    constexpr idx_t kAhead = 16;
    st.first.resize(n);
    for (idx_t i = 0; i < n; i++) {
        if (i + kAhead < n && st.key_ok[i + kAhead]) {
            __builtin_prefetch(&build.heads[st.hashes[i + kAhead] & build.mask]);
        }
        st.first[i] = st.key_ok[i] ? build.heads[st.hashes[i] & build.mask] : kNone;
    }
    const idx_t key_columns = build.keys.ColumnCount();
    for (idx_t i = 0; i < n; i++) {
        const uint32_t b = st.first[i];
        if (b == kNone) {
            continue;
        }
        __builtin_prefetch(&build.entries[b]);
        for (idx_t c = 0; c < key_columns; c++) {
            __builtin_prefetch(build.keys.CellAddress(c, b));
        }
    }
}

OperatorResult PhysicalHashJoin::Execute(OperatorState& state, const DataChunk& input,
                                         DataChunk& output) {
    auto& st = static_cast<ProbeState&>(state);
    const JoinGlobalState& build = *st.build;
    const idx_t n = input.size();
    const idx_t left_w = left_types_.size();
    const bool emits_pairs =
        join_type_ == PhysicalJoinType::Inner || join_type_ == PhysicalJoinType::Left;

    if (!st.active) { // first call for this input chunk
        st.active = true;
        st.phase = ProbeState::Phase::Matching;
        st.pos = 0;
        st.chain = kNotStarted;
        st.matched.assign(n, 0);
        st.key_ok.assign(n, 1);
        st.hashes.assign(n, 0);
        if (!left_keys_.empty()) {
            for (size_t k = 0; k < st.key_executors.size(); k++) {
                st.key_executors[k].Execute(input, st.key_chunk.column(k));
            }
            st.key_chunk.SetCardinality(n);
            std::vector<const Vector*> cols(left_keys_.size());
            for (size_t k = 0; k < cols.size(); k++) {
                cols[k] = &st.key_chunk.column(k);
                UnifiedFormat f;
                cols[k]->ToUnified(f);
                for (idx_t i = 0; i < n; i++) {
                    if (!f.IsValid(i)) {
                        st.key_ok[i] = 0;
                    }
                }
            }
            HashColumns(cols.data(), cols.size(), n, st.hashes.data());
            st.comparator.emplace(cols, /*nulls_equal=*/false);
            Prefetch(st, build, n);
        }
    }

    if (st.phase == ProbeState::Phase::Matching) {
        // ---- collect up to one vector of candidate pairs ------------------------------------
        st.probe_rows.clear();
        st.build_rows.clear();
        const idx_t build_rows = build.payload.Count();
        while (st.pos < n && st.probe_rows.size() < kVectorSize) {
            if (st.chain == kNotStarted) {
                if (left_keys_.empty()) {
                    st.chain = build_rows > 0 ? 0 : kNone;
                } else {
                    st.chain = st.first[st.pos];
                }
            }
            while (st.chain != kNone && st.probe_rows.size() < kVectorSize) {
                const uint32_t b = st.chain;
                if (left_keys_.empty()) {
                    st.chain = b + 1 < build_rows ? b + 1 : kNone;
                    st.probe_rows.push_back(static_cast<uint32_t>(st.pos));
                    st.build_rows.push_back(b);
                    continue;
                }
                const JoinGlobalState::Entry& entry = build.entries[b];
                st.chain = entry.next;
                if (entry.hash == st.hashes[st.pos] &&
                    st.comparator->StoredEqualsInput(build.keys, b, st.pos)) {
                    st.probe_rows.push_back(static_cast<uint32_t>(st.pos));
                    st.build_rows.push_back(b);
                }
            }
            if (st.chain != kNone) {
                break; // the batch is full in the middle of this row's chain
            }
            st.pos++;
            st.chain = kNotStarted;
        }
        const bool finished_matching = st.pos >= n;

        // ---- turn the pairs into output / match flags -----------------------------------------
        const idx_t pairs = st.probe_rows.size();
        if (pairs > 0 && (emits_pairs || residual_)) {
            DataChunk& target = emits_pairs ? output : st.joined;
            if (!emits_pairs) {
                st.joined
                    .Reset(); // Gather() only ever marks rows invalid: start from a clean chunk
            }
            SelectionVector left_sel = SelectionVector::Uninitialized(pairs);
            for (idx_t j = 0; j < pairs; j++) {
                left_sel.Set(j, st.probe_rows[j]);
            }
            ShareLeft(input, target, left_sel, pairs);
            for (idx_t c = 0; c < right_types_.size(); c++) {
                build.payload.Gather(c, st.build_rows.data(), pairs, target.column(left_w + c));
            }
            target.SetCardinality(pairs);
            if (residual_) {
                const idx_t kept = st.residual->Select(target, st.selection);
                for (idx_t j = 0; j < kept; j++) {
                    st.matched[st.probe_rows[st.selection[j]]] = 1;
                }
                if (emits_pairs) {
                    if (kept == 0) {
                        output.SetCardinality(0);
                    } else if (kept < pairs) {
                        output.Slice(st.selection, kept);
                    }
                }
            } else {
                for (idx_t j = 0; j < pairs; j++) {
                    st.matched[st.probe_rows[j]] = 1;
                }
            }
        } else {
            for (const uint32_t r : st.probe_rows) {
                st.matched[r] = 1; // SEMI/ANTI without a residual need only the flags
            }
        }

        if (!finished_matching) {
            return OperatorResult::HaveMoreOutput;
        }
        if (join_type_ == PhysicalJoinType::Inner) {
            st.active = false;
            return OperatorResult::NeedMoreInput;
        }
        st.phase = ProbeState::Phase::Unmatched;
        if (output.size() > 0) {
            return OperatorResult::HaveMoreOutput; // flush these pairs; the leftovers come next
                                                   // call
        }
    }

    // ---- left rows without a partner (LEFT / ANTI), or with one (SEMI) -------------------------
    const bool want_matched = join_type_ == PhysicalJoinType::Semi;
    SelectionVector sel(std::max<idx_t>(n, 1));
    idx_t count = 0;
    for (idx_t i = 0; i < n; i++) {
        if ((st.matched[i] != 0) == want_matched) {
            sel.Set(count++, static_cast<sel_t>(i));
        }
    }
    if (count > 0) {
        ShareLeft(input, output, sel, count);
        if (join_type_ == PhysicalJoinType::Left) {
            for (idx_t c = 0; c < right_types_.size(); c++) {
                output.column(left_w + c)
                    .Reference(Vector::MakeConstant(Value::Null(right_types_[c])));
            }
        }
        output.SetCardinality(count);
    } else {
        output.SetCardinality(0);
    }
    st.active = false;
    return OperatorResult::NeedMoreInput;
}

} // namespace cdb
