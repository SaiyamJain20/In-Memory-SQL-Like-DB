#!/usr/bin/env python3
"""Mutation smoke test: proves the test suite can actually fail.

For each mutation below, one targeted bug is injected into the source (exact-text replace), the
project is rebuilt with the given preset, and the test suite is run. The mutation is "killed" if
the suite fails (or the build breaks the tests), "SURVIVED" if everything still passes - which
means a gap in the tests.  The source file is always restored afterwards.

Safety: a mutated source file must never be left behind (or committed). The tool therefore
  * refuses to run unless every file it mutates is clean in git (override: --allow-dirty),
  * keeps an on-disk backup of the file it is mutating and restores it on SIGTERM/SIGHUP/SIGINT,
  * repairs leftovers from a run that died without cleaning up (e.g. SIGKILL) on the next start,
  * verifies at the end that the mutated files are byte-identical to how it found them.

Usage:  tools/mutation_smoke.py [--preset debug] [--only SUBSTRING[|SUBSTRING...]] [--check] [--allow-dirty]
Exit status is non-zero if any mutation survives or does not build.

Add a mutation whenever a new subsystem lands: pick a plausible, subtle bug (off-by-one, wrong
mask, missing null check, aliasing) rather than something that fails to compile.
"""
import argparse
import pathlib
import re
import signal
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (name, file, old text, new text, preset the mutation needs or None for any)
MUTATIONS = [
    ("string_t: compare prefix without byte swap (little-endian order bug)",
     "src/types/string_t.h",
     "return __builtin_bswap32(v);", "return v;", None),
    ("string_t: equality ignores inlined tail bytes",
     "src/types/string_t.h",
     "return ta == tb;", "return true;", None),
    ("string_t: compare skips the bytes after the prefix",
     "src/types/string_t.h",
     "if (min_len > kPrefixLength) {", "if (false) {", None),
    ("date: civil-from-days month adjustment off by one",
     "src/types/date.cpp",
     "y -= m <= 2;", "y -= m < 2;", None),
    ("date: leap-year rule ignores the 400-year exception",
     "src/types/date.h",
     "return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;",
     "return year % 4 == 0 && year % 100 != 0;", None),
    ("value: NaN no longer sorts after every other double",
     "src/types/value.cpp",
     "return an == bn ? 0 : (an ? 1 : -1);", "return an == bn ? 0 : (an ? -1 : 1);", None),
    ("validity: CountValid includes bits beyond `count` in the tail word",
     "src/vector/validity_mask.cpp",
     "const uint64_t tail_mask = (uint64_t{1} << (count & 63)) - 1;",
     "const uint64_t tail_mask = ~uint64_t{0};", None),
    ("arena: bump allocation ignores the requested alignment",
     "src/memory/arena.cpp",
     "const size_t aligned = AlignUp(offset_, alignment);", "const size_t aligned = offset_;",
     None),
    ("vector: Slice of a dictionary forgets to compose with the old selection",
     "src/vector/vector.cpp",
     "new_sel.Set(i, sel_[sel[i]]);", "new_sel.Set(i, sel[i]);", None),
    ("vector: Reset overwrites a buffer another vector still references",
     "src/vector/vector.cpp",
     "if (!data_ || data_.use_count() > 1 || data_->read_only() || data_->size() < needed) {",
     "if (!data_ || data_->read_only() || data_->size() < needed) {", None),
    ("vector: Copy keeps pointers into the source's string heap",
     "src/vector/vector.cpp",
     "out[i] = dst_heap->Add(in[s].view());", "out[i] = in[s];",
     None),
    ("validity: SetRangeValid last-word mask is off by one bit",
     "src/vector/validity_mask.cpp",
     "(uint64_t{1} << (end & 63)) - 1;", "(uint64_t{1} << (end & 63)) - 2;", None),
    ("vector: Copy memcpy fast path copies one element too few",
     "src/vector/vector.cpp",
     "std::memcpy(out, in + src_offset, count * sizeof(T));",
     "std::memcpy(out, in + src_offset, (count - 1) * sizeof(T));",
     None),
    ("vector: Copy forgets to mark overwritten NULL slots valid when the source has no NULLs",
     "src/vector/vector.cpp",
     "dst_validity.SetRangeValid(dst_offset, count);", "(void)0;", None),
    # ---- Phase 2: storage ----
    ("stats: Ge pruning becomes unsound (skips when max == constant)",
     "src/storage/column_stats.cpp",
     "case CompareOp::Ge: return vs_max > 0;", "case CompareOp::Ge: return vs_max >= 0;", None),
    ("stats: Lt pruning is needlessly timid (never skips when min == constant)",
     "src/storage/column_stats.cpp",
     "case CompareOp::Lt: return vs_min <= 0;", "case CompareOp::Lt: return vs_min < 0;", None),
    ("stats: min/max consider values hidden under NULLs",
     "src/storage/column_stats.cpp",
     "if (!validity.IsValid(i)) { continue; }",
     "if (false && !validity.IsValid(i)) { continue; }", None),
    ("column builder: buffer growth loses the last appended row",
     "src/storage/column_builder.cpp",
     "std::memcpy(grown->data(), data_->data(), count_ * type_.width());",
     "std::memcpy(grown->data(), data_->data(), (count_ - 1) * type_.width());", None),
    ("column builder: validity of grown mask loses 'valid' default for new rows",
     "src/vector/validity_mask.cpp",
     "std::memset(grown->data() + old_words * sizeof(uint64_t), 0xFF,",
     "std::memset(grown->data() + old_words * sizeof(uint64_t), 0x00,", None),
    ("segment: Scan hands out the next vector's data view",
     "src/storage/column_segment.cpp",
     "out.ReferenceFlat(data_views_[v], std::move(validity), heap_);",
     "out.ReferenceFlat(data_views_[v + 1 < data_views_.size() ? v + 1 : v], std::move(validity), heap_);",
     None),
    ("segment: Scan hands out the wrong validity window",
     "src/storage/column_segment.cpp",
     "ValidityMask::FromBuffer(validity_views_[v], kVectorSize);",
     "ValidityMask::FromBuffer(validity_views_[0], kVectorSize);", None),
    ("table: append forgets to invalidate the cached tail snapshot",
     "src/storage/table.cpp",
     "    tail_cache_.reset(); // safe: we hold the exclusive lock, so no Snapshot() is running\n", "",
     None),
    ("table: append splits chunks across groups using the wrong remainder",
     "src/storage/table.cpp",
     "const idx_t n = std::min(open_->max_rows() - open_->count(), chunk.size() - pos);",
     "const idx_t n = chunk.size() - pos;", None),
    ("table: scan ignores zone maps entirely (pruning never fires)",
     "src/storage/table.cpp",
     "skip = g.column(f.column_index).stats().CanSkip(f.op, f.constant);",
     "(void)f; skip = false;", None),
    ("table: scan over-prunes (skips groups whose zone map says 'maybe')",
     "src/storage/table.cpp",
     "skip = g.column(f.column_index).stats().CanSkip(f.op, f.constant);",
     "skip = !g.column(f.column_index).stats().CanSkip(f.op, f.constant);", None),
    ("catalog: names are no longer case-insensitive",
     "src/catalog/catalog.cpp",
     "[](unsigned char c) { return static_cast<char>(std::tolower(c)); });",
     "[](unsigned char c) { return static_cast<char>(c); });", None),
    ("vector: Reset reuses a read-only (segment-owned) buffer for writing",
     "src/vector/vector.cpp",
     "if (!data_ || data_.use_count() > 1 || data_->read_only() || data_->size() < needed) {",
     "if (!data_ || data_.use_count() > 1 || data_->size() < needed) {", None),
    ("table (tsan): concurrent Snapshot() calls race on the tail cache",
     "src/storage/table.cpp",
     "std::lock_guard<std::mutex> guard(tail_mutex_);", "", "tsan"),
    ("table (tsan): Append only takes a shared lock, racing with readers",
     "src/storage/table.cpp",
     "    std::unique_lock lock(mutex_);\n    idx_t pos = 0;",
     "    std::shared_lock lock(mutex_);\n    idx_t pos = 0;", "tsan"),
    # ---- Phase 3: SQL front end ----
    ("lexer: '--' comments are no longer recognised",
     "src/parser/lexer.cpp",
     "if (c == '-' && i + 1 < n && sql[i + 1] == '-') {", "if (c == '-' && i + 1 < n && sql[i + 1] == '#') {", None),
    ("lexer: doubled quotes inside string literals are not unescaped",
     "src/parser/lexer.cpp",
     "if (i + 1 < n && sql[i + 1] == '\\'') {", "if (i + 1 < n && sql[i + 1] == '#') {", None),
    ("lexer: uppercase exponent 'E' is not a number",
     "src/parser/lexer.cpp",
     "if (i < n && (sql[i] == 'e' || sql[i] == 'E')) {", "if (i < n && sql[i] == 'e') {", None),
    ("parser: AND no longer binds tighter than OR",
     "src/parser/parser.cpp",
     "while (Peek().IsKeyword(Keyword::AND)) {", "while (Peek().IsKeyword(Keyword::OR)) {", None),
    ("parser: BETWEEN does not consume its AND as part of the predicate",
     "src/parser/parser.cpp",
     "ExprPtr hi = ParseAdditive();", "ExprPtr hi = ParseOr();", None),
    ("parser: 'x NOT IN' parses as 'x IN'",
     "src/parser/parser.cpp",
     "negated = true;\n        }\n        if (AcceptKeyword(Keyword::BETWEEN)) {", "negated = false;\n        }\n        if (AcceptKeyword(Keyword::BETWEEN)) {", None),
    ("ast: printed string literals do not double their quotes",
     "src/parser/ast.cpp",
     "if (ch == '\\'')\n            out += '\\'';\n        out += ch;\n    }\n    out += '\\'';", "out += ch;\n    }\n    out += '\\'';", None),
    ("parser: unary minus is dropped",
     "src/parser/parser.cpp",
     "return Finish(std::make_unique<UnaryExpr>(t.pos, UnaryOp::Negate, std::move(child)), d);", "(void)d; return child;", None),
    # ---- Phase 3: types, evaluation ----
    ("cast: DOUBLE -> integer rounds half away from zero instead of half to even",
     "src/types/cast.cpp",
     "const double r = std::nearbyint(d);", "const double r = std::round(d);", None),
    ("cast: hex integer strings are parsed as decimal",
     "src/types/cast.cpp",
     "base = 16;", "base = 10;", None),
    ("cast: interval month arithmetic does not clamp to the end of the month",
     "src/types/cast.cpp",
     "const int nd = std::min(d, Date::DaysInMonth(ny, nm));", "const int nd = d;", None),
    ("value: doubles switch to scientific notation one decade too late",
     "src/types/value.cpp",
     "if (exponent >= -4 && exponent < 16) {", "if (exponent >= -4 && exponent < 17) {", None),
    ("string_ops: LIKE '_' consumes one byte instead of one UTF-8 character",
     "src/common/string_ops.cpp",
     "t += Utf8CharLength(text, t);\n            p++;", "t += 1;\n            p++;", None),
    ("string_ops: substring treats start 0 like start 1",
     "src/common/string_ops.cpp",
     "long long begin = start > 0 ? start - 1 : (start == 0 ? -1 : n + start);", "long long begin = start > 0 ? start - 1 : (start == 0 ? 0 : n + start);", None),
    ("eval: AND/OR do not short-circuit",
     "src/planner/scalar_eval.cpp",
     "if (!l.IsNull() && l.GetBoolean() == !is_and)\n            return l; // false AND _ / true OR _", "if (false)\n            return l;", None),
    ("eval: integer addition overflow is not detected",
     "src/planner/scalar_eval.cpp",
     "if (__builtin_add_overflow(a, b, &r))\n            Overflow(\"addition\", type, shown);", "r = static_cast<T>(a + b);", None),
    # ---- Phase 3: binder ----
    ("binder: a non-grouped column is accepted next to aggregates",
     "src/planner/binder.cpp",
     "if (e->kind == BoundKind::ColumnRef) {\n            Fail(ErrorCode::Binder,\n                 \"column \\\"\" + e->name", "if (false) {\n            Fail(ErrorCode::Binder,\n                 \"column \\\"\" + e->name", None),
    ("binder: USING keeps both copies of the joined column in `*`",
     "src/planner/binder.cpp",
     "out.scope.columns[left_width + ro].hidden = true;", "out.scope.columns[left_width + ro].hidden = false;", None),
    ("binder: ORDER BY defaults to NULLS FIRST",
     "src/planner/binder.cpp",
     "const bool nulls_first = oi.nulls == NullOrder::First;", "const bool nulls_first = oi.nulls != NullOrder::Last;", None),
    ("binder: string literals are no longer converted in comparisons",
     "src/planner/bind_expression.cpp",
     "if (convert_string_literals && IsVarcharConstant(*l) && r->type.id() != TypeId::Varchar) {", "if (false && IsVarcharConstant(*l) && r->type.id() != TypeId::Varchar) {", None),
    ("binder: BETWEEN lower bound becomes exclusive",
     "src/planner/bind_expression.cpp",
     "MakeComparison(OperatorKind::Ge, x->Clone(), std::move(lo), e.pos);", "MakeComparison(OperatorKind::Gt, x->Clone(), std::move(lo), e.pos);", None),
    ("binder: `/` is no longer forced to floating point",
     "src/planner/bind_expression.cpp",
     "if (op == OperatorKind::Div) { // `/` is always floating point, like DuckDB", "if (false) { // `/`", None),
    ("binder: INTERVAL subtraction adds instead",
     "src/planner/bind_expression.cpp",
     "const int64_t amount = e.op == BinaryOp::Sub ? -interval.amount : interval.amount;", "const int64_t amount = interval.amount;", None),
    ("binder: a negative LIMIT is accepted",
     "src/planner/binder.cpp",
     "if (v < 0)", "if (v < -1000)", None),
    # ---- Phase 3: bulk load, DML ----
    ("table: NOT NULL columns accept NULLs",
     "src/storage/table.cpp",
     "if (!u.IsValid(r)) {", "if (false) {", None),
    ("table: Merge discards the target's partial tail",
     "src/storage/table.cpp",
     "sealed_.push_back(open_->Seal());\n    }\n    open_.reset();", "}\n    open_.reset();", None),
    ("csv: CRLF line endings leave a carriage return in the last field",
     "src/io/csv_reader.cpp",
     "if (record.back() == '\\r') {", "if (false) {", None),
    ("csv: the header line is loaded as data",
     "src/io/csv_reader.cpp",
     "bool skipped_header = !options.header;", "bool skipped_header = true;", None),
    ("vector: Flatten of a dictionary drops the child's string heap (use-after-free)",
     "src/vector/vector.cpp",
     "        heap_ = child_->heap_;\n", "", "asan"),
    # ---- Phase 4: vectorized executor, operators, planner, optimizer ----------------------------
    ("executor: AND/OR decide on the wrong truth value, so the right side is skipped for the wrong rows",
     "src/execution/expression_executor.cpp",
     "ld[lv.Slot(i)] == !is_and", "ld[lv.Slot(i)] == is_and", None),
    ("executor: three-valued AND is FALSE only if both sides are",
     "src/execution/expression_executor.cpp",
     "inline int And3(int a, int b) noexcept {\n    if (a == 0 || b == 0)",
     "inline int And3(int a, int b) noexcept {\n    if (a == 0 && b == 0)", None),
    ("executor: LIKE prefix matcher demands a strictly longer string",
     "src/execution/expression_executor.cpp",
     "return s.size() >= text.size() && s.compare(0, text.size(), text) == 0;",
     "return s.size() > text.size() && s.compare(0, text.size(), text) == 0;", None),
    ("executor: integer addition overflow is never reported",
     "src/execution/expression_executor.cpp",
     "return __builtin_add_overflow(a, b, &out);", "return false;", None),
    ("executor: Select of a negated comparison does not flip the operator",
     "src/execution/expression_executor.cpp",
     "want ? e.op : Negated(e.op)", "want ? e.op : Negated(Negated(e.op))", None),
    ("executor: Select narrows AND/OR the wrong way round",
     "src/execution/expression_executor.cpp",
     "if (is_and == want) {", "if (is_and != want) {", None),
    ("executor: IS NULL in selection position selects the opposite rows",
     "src/execution/expression_executor.cpp",
     "const bool is_true = (!cv.Valid(i)) != e.flag;", "const bool is_true = (!cv.Valid(i)) == e.flag;", None),
    ("hashing: 0.0 and -0.0 hash differently",
     "src/execution/hashing.cpp",
     "} else if (x == 0.0) {", "} else if (false) {", None),
    ("hashing: NaNs hash by their payload bits",
     "src/execution/hashing.cpp",
     "if (std::isnan(x)) {", "if (false) {", None),
    ("chunk store: Gather forgets to mark NULL rows invalid",
     "src/execution/chunk_store.cpp",
     "dv.SetInvalid(i);", "(void)dv;", None),
    ("key index: a pending (not yet stored) key is compared against the stored rows",
     "src/execution/key_index.cpp",
     "(id < stored ? cmp.StoredEqualsInput(keys_, id, i)", "(id <= stored ? cmp.StoredEqualsInput(keys_, id, i)", None),
    ("key comparator: NULL keys compare equal even when they must not (joins)",
     "src/execution/key_index.cpp",
     "if (s_valid != i_valid || !nulls_equal_) {", "if (s_valid != i_valid) {", None),
    ("aggregates: integer SUM wraps instead of reporting overflow",
     "src/execution/aggregate_state.cpp",
     "if (__builtin_add_overflow(sums_[g], static_cast<int64_t>(data[u.sel[i]]), &sums_[g])) {",
     "if (__builtin_add_overflow(sums_[g], static_cast<int64_t>(data[u.sel[i]]), &sums_[g]) && false) {", None),
    ("aggregates: merging two SUM states does not check overflow",
     "src/execution/aggregate_state.cpp",
     "if (__builtin_add_overflow(sums_[dst[g]], s.sums_[g], &sums_[dst[g]])) {", "if (false) {", None),
    ("aggregates: AVG of no non-NULL input is NaN instead of NULL",
     "src/execution/aggregate_state.cpp",
     "if (counts_[first + i] > 0) {", "if (true) {", None),
    ("aggregates: MIN and MAX are swapped",
     "src/execution/aggregate_state.cpp",
     "(kMax ? Less<T>(static_cast<T>(values_[g]), v)", "(!kMax ? Less<T>(static_cast<T>(values_[g]), v)", None),
    ("aggregates: COUNT(x) counts NULLs",
     "src/execution/aggregate_state.cpp",
     "if (u.validity->AllValid()) {", "if (true) {", None),
    ("group table: a global aggregate has no group until it sees a row",
     "src/execution/group_table.cpp",
     "if (group_types_.empty()) {\n        DataChunk none;", "if (false) {\n        DataChunk none;",
     None),
    ("group table: the ungrouped fast path falls through into the hashed path (double counting)",
     "src/execution/group_table.cpp",
     "return;\n    }\n    ids_.resize(count);", "}\n    ids_.resize(count);", None),
    ("group table: merging tables drops the other table's aggregate states",
     "src/execution/group_table.cpp",
     "states_[a]->Combine(*other.states_[a], mapping.data(), n);", "(void)mapping;", None),
    ("hash aggregate: Combine of a second local state discards it",
     "src/execution/hash_aggregate.cpp",
     "g.table->Combine(l.table);", "(void)l;", None),
    ("hash join: bucket chains list build rows in reverse order",
     "src/execution/hash_join.cpp",
     "for (idx_t row = n; row-- > 0;) {", "for (idx_t row = 0; row < n; row++) {", None),
    ("hash join: LEFT join does not remember which probe rows matched",
     "src/execution/hash_join.cpp",
     "st.matched[st.probe_rows[j]] = 1;", "(void)j;", None),
    ("hash join: SEMI and ANTI are swapped",
     "src/execution/hash_join.cpp",
     "const bool want_matched = join_type_ == PhysicalJoinType::Semi;",
     "const bool want_matched = join_type_ == PhysicalJoinType::Anti;", None),
    ("sort: NULLs go to the wrong end",
     "src/execution/sort.cpp",
     "return (a_null == keys_[k].nulls_first) ? -1 : 1;", "return (a_null != keys_[k].nulls_first) ? -1 : 1;", None),
    ("sort: DESC is ignored",
     "src/execution/sort.cpp",
     "if (keys_[k].descending) {", "if (false) {", None),
    ("sort: the sort is no longer stable",
     "src/execution/sort.cpp",
     "std::stable_sort(order_.begin(), order_.end(),", "std::sort(order_.begin(), order_.end(),", None),
    ("top-n: OFFSET is ignored when computing the end of the output range",
     "src/execution/sort.cpp",
     "static_cast<idx_t>(offset_) + static_cast<idx_t>(limit_)", "static_cast<idx_t>(limit_)", None),
    ("limit: the limit is not reduced by rows already emitted from earlier chunks",
     "src/execution/basic_operators.cpp",
     "take = std::min(take, *limit_ - s.emitted);", "take = std::min(take, *limit_);", None),
    ("limit: never reports Finished, so the source is read to the end",
     "src/execution/basic_operators.cpp",
     "return limit_ && s.emitted >= *limit_ ?", "return limit_ && s.emitted > *limit_ ?", None),
    ("planner: ORDER BY ... LIMIT no longer plans as a top-N",
     "src/execution/physical_planner.cpp",
     "if (limit.limit && child.kind == LogicalKind::Order) {", "if (false) {", None),
    ("planner: the residual of a RIGHT join keeps the unswapped column numbering",
     "src/execution/physical_planner.cpp",
     "return o < lw ? o + rw : o - lw;", "return o;", None),
    ("planner: the projection after a swapped RIGHT join reads the wrong columns",
     "src/execution/physical_planner.cpp",
     "exprs.push_back(BoundExpr::ColumnRef(rw + i, left.types[i], left.names[i]));",
     "exprs.push_back(BoundExpr::ColumnRef(i, left.types[i], left.names[i]));", None),
    ("planner: an equality with the sides reversed is not recognised as a hash key",
     "src/execution/physical_planner.cpp",
     "if (a_left && b_right) {", "if (a_left && b_right && false) {", None),
    ("optimizer: WHERE conjuncts of a RIGHT join are pushed to its padded (left) side",
     "src/planner/optimizer.cpp",
     "if (left_only && preserve_left) {", "if (left_only) {", None),
    ("optimizer: ON conjuncts of a RIGHT join are pushed into the preserved side",
     "src/planner/optimizer.cpp",
     "if (preserve_left && right_only) {", "if (right_only) {", None),
    ("optimizer: OR factoring keeps the remainder even when a branch is exactly the common part",
     "src/planner/optimizer.cpp",
     "if (!any_empty && rest) {", "if ((!any_empty || true) && rest) {", None),
    ("optimizer: OR factoring treats every conjunct of the first branch as common",
     "src/planner/optimizer.cpp",
     "in_all = std::any_of(branch_parts[b].begin(), branch_parts[b].end(),", "in_all = true || std::any_of(branch_parts[b].begin(), branch_parts[b].end(),", None),
    ("optimizer: zone-map hint ignores that the constant was on the left",
     "src/planner/optimizer.cpp",
     "op = Flip(op);", "op = Flip(Flip(op));", None),
    ("optimizer: HAVING conjuncts on aggregates are pushed below the aggregate",
     "src/planner/optimizer.cpp",
     "if (!refs.empty() && *refs.rbegin() < agg.groups.size()) {", "if (!refs.empty()) {", None),
    ("optimizer: LIMIT no longer moves below a projection (so never becomes a top-N)",
     "src/planner/optimizer.cpp",
     "if (op->children[0]->kind == LogicalKind::Projection) {", "if (false) {", None),
    ("optimizer: a composite join key is sized as if only one of its columns were a key",
     "src/planner/optimizer.cpp",
     "key_combinations *= max_ndv;", "key_combinations = max_ndv;", None),
    ("string_ops: a truncated multi-byte sequence is read past the end of the string",
     "src/common/string_ops.cpp",
     "if (len > s.size() - pos)", "if (false)", None),
    ("string_ops: a lead byte followed by a non-continuation byte still swallows it",
     "src/common/string_ops.cpp",
     "!= 0x80)", "== 0x80)", None),
    ("vector: Reset keeps a shared buffer instead of detaching (aliasing writes)",
     "src/vector/vector.cpp",
     "data_.reset(); // allocated on first write access (EnsureData): often there is none", ";", None),
    ("vector: ToUnified of a lazily allocated vector reads a null buffer",
     "src/vector/vector.cpp",
     "case VectorFormat::Flat:\n        EnsureData();\n        out.sel = SelectionVector::Identity().data();",
     "case VectorFormat::Flat:\n        out.sel = SelectionVector::Identity().data();", None),
    # ---- Phase 5: bit-packing, segment encodings, SIMD kernels --------------------------------
    ("bitpacking: BitWidth is one too small",
     "src/storage/bitpacking.h",
     "64 - __builtin_clzll(x)", "63 - __builtin_clzll(x)", None),
    ("bitpacking: PackedBytes drops the slack word (unpack reads past the stream)",
     "src/storage/bitpacking.h",
     "/ 64) * 8 + 8;", "/ 64) * 8;", "asan"),
    ("bitpacking: BitUnpack of zero values passes null pointers to memset/memcpy",
     "src/storage/bitpacking.cpp",
     "if (count == 0) {", "if (false) {", "asan"),
    ("bitpacking: BitPack keeps the high bits of the input",
     "src/storage/bitpacking.cpp",
     "const uint64_t v = in[i] & mask;", "const uint64_t v = in[i] & (mask | ~mask);", None),
    ("bitpacking: the bit position after a word boundary is off by one",
     "src/storage/bitpacking.cpp",
     "used = used + width - 64;", "used = used + width - 63;", None),
    ("bitpacking: the generic tail unpack ignores values straddling two words",
     "src/storage/bitpacking.cpp",
     "if (shift + width > 64) {", "if (false) {", None),
    ("bitpacking: the unrolled unpacker shifts the straddling part by the wrong amount",
     "src/storage/bitpacking.cpp",
     "v |= w[word + 1] << (64 - shift);", "v |= w[word + 1] << (63 - shift);", None),
    ("bitpacking: the unrolled unpacker masks one bit too many",
     "src/storage/bitpacking.cpp",
     "constexpr uint64_t mask = W == 64 ? ~uint64_t{0} : (uint64_t{1} << W) - 1;",
     "constexpr uint64_t mask = W == 64 ? ~uint64_t{0} : (uint64_t{1} << W) * 2 - 1;", None),
    ("bitpacking: the unrolled unpacker reads each block one word late",
     "src/storage/bitpacking.cpp",
     "std::memcpy(words, in + (i / 64) * W * 8, W * 8);",
     "std::memcpy(words, in + (i / 64) * W * 8 + 8, W * 8);", None),
    ("encoding: delta chosen for a sequence that is not monotone",
     "src/storage/encoding.cpp",
     "monotone = false;", "monotone = true;", None),
    ("encoding: the first delta of a vector is not zero",
     "src/storage/encoding.cpp",
     "packed[0] = 0;", "packed[0] = 1;", None),
    ("encoding: frame-of-reference base is the first value instead of the minimum",
     "src/storage/encoding.cpp",
     "m.base = lo;\n            m.width = for_width;", "m.base = x[0];\n            m.width = for_width;",
     None),
    ("encoding: a constant bit-packed vector with NULL rows is returned as an all-valid constant",
     "src/storage/encoding.cpp",
     "if (m.width == 0 && !m.delta && validity.CountValid(n) == n) {",
     "if (m.width == 0 && !m.delta && validity.CountValid(n) <= n) {", None),
    ("encoding: an RLE vector covered by one run ignores its NULL rows",
     "src/storage/encoding.cpp",
     "if (ends_[run] >= end && validity.CountValid(n) == n) {", "if (ends_[run] >= end && validity.CountValid(n) <= n) {", None),
    ("encoding: RLE fill runs past the end of the vector",
     "src/storage/encoding.cpp",
     "const idx_t stop = std::min<idx_t>(ends_[run], end);", "const idx_t stop = ends_[run];", None),
    ("encoding: RLE remembers the wrong first run of a vector",
     "src/storage/encoding.cpp",
     "static_cast<uint32_t>(run_values.size() - 1)); // the run holding row i",
     "static_cast<uint32_t>(run_values.size())); // the run holding row i", None),
    ("encoding: a constant double vector with NULL rows is returned as an all-valid constant",
     "src/storage/encoding.cpp",
     "if (validity.CountValid(n) == n) {\n                out.SetConstant(Value::Double(d));",
     "if (validity.CountValid(n) <= n) {\n                out.SetConstant(Value::Double(d));", None),
    ("encoding: scaled doubles compare by value, so -0.0 turns into 0.0",
     "src/storage/encoding.cpp",
     "if (std::memcmp(&back, &v[i], sizeof(double)) != 0) {", "if (back != v[i]) {", None),
    ("encoding: constant detection compares doubles by value (0.0 == -0.0)",
     "src/storage/encoding.cpp",
     "} else if (std::memcmp(&v[i], &v[first], sizeof(double)) != 0) {",
     "} else if (v[i] != v[first]) {", None),
    ("encoding: offsets wider than 52 bits go through the exact-conversion trick",
     "src/storage/encoding.cpp",
     "if (m.width <= 52) {", "if (m.width <= 63) {", None),
    ("encoding: dictionary NULL rows map to the first entry",
     "src/storage/encoding.cpp",
     "packed[i] = row_code[start + i] == kNoCode ? null_code : row_code[start + i];",
     "packed[i] = row_code[start + i] == kNoCode ? 0 : row_code[start + i];", None),
    ("encoding: the dictionary's NULL entry is not marked invalid",
     "src/storage/encoding.cpp",
     "dictionary->Validity().SetInvalid(null_code);", "dictionary->Validity().SetInvalid(0);",
     None),
    ("encoding: a dictionary may fill every slot, leaving none for its NULL entry",
     "src/storage/encoding.cpp",
     "if (entries.size() == kVectorSize - 1) {", "if (entries.size() == kVectorSize) {", None),
    ("encoding: Auto accepts an encoding that saves nothing",
     "src/storage/encoding.cpp",
     "constexpr double kMaxEncodedFraction = 0.7;", "constexpr double kMaxEncodedFraction = 1e9;",
     None),
    ("encoding: the smallest candidate is no longer the one kept",
     "src/storage/encoding.cpp",
     "candidate->MemoryUsage() < best->MemoryUsage()",
     "candidate->MemoryUsage() > best->MemoryUsage()", None),
    ("segment: an encoded scan drops the segment's NULLs for non-constant vectors",
     "src/storage/column_segment.cpp",
     "if (!encoded_->DecodeVector(v, n, validity, out)) {",
     "if (encoded_->DecodeVector(v, n, validity, out)) {", None),
    ("segment: an encoded scan does not reset the reused output vector",
     "src/storage/column_segment.cpp",
     "if (encoded_ != nullptr) {\n        out.Reset();", "if (encoded_ != nullptr) {", None),
    ("simd: SetSimdEnabled(false) has no effect",
     "src/kernels/cpu.cpp",
     "SimdState().load(std::memory_order_relaxed) != 0 && CpuHasAvx2()",
     "SimdState().load(std::memory_order_relaxed) != 0 || CpuHasAvx2()", None),
    ("select kernel: the compaction table lists the positions in reverse order",
     "src/kernels/select.cpp",
     "perm[mask][k++] = bit;", "perm[mask][k++] = 7 - bit;", None),
    ("select kernel: the match count ignores the eighth lane",
     "src/kernels/select.cpp",
     "__builtin_popcount(mask)", "__builtin_popcount(mask & 0x7F)", None),
    ("select kernel: int32 <= excludes equality",
     "src/kernels/select.cpp",
     "case CmpOp::Le:\n            m = _mm256_xor_si256(_mm256_cmpgt_epi32(x, c), _mm256_set1_epi32(-1));",
     "case CmpOp::Le:\n            m = _mm256_cmpgt_epi32(c, x);", None),
    ("select kernel: int32 tail skips its first row",
     "src/kernels/select.cpp",
     "SelectScalar(op, data + 0, i, count, constant, out + n)",
     "SelectScalar(op, data + 0, i + 1, count, constant, out + n)", None),
    ("select kernel: int64 >= is a strict comparison",
     "src/kernels/select.cpp",
     "default: // Ge\n        return _mm256_xor_si256(_mm256_cmpgt_epi64(c, x), ones);",
     "default: // Ge\n        return _mm256_cmpgt_epi64(x, c);", None),
    ("select kernel: double > is false for NaN (NaN must be the largest value)",
     "src/kernels/select.cpp",
     "_CMP_NLE_UQ", "_CMP_GT_OQ", None),
    ("select kernel: double >= is false for NaN",
     "src/kernels/select.cpp",
     "_CMP_NLT_UQ", "_CMP_GE_OQ", None),
    ("select kernel: double != is false for NaN",
     "src/kernels/select.cpp",
     "_CMP_NEQ_UQ", "_CMP_NEQ_OQ", None),
    ("select kernel: a NaN constant reaches the vector comparison",
     "src/kernels/select.cpp",
     "if (UseAvx2() && !std::isnan(constant)) {", "if (UseAvx2()) {", None),
    ("decode kernel: scaled-double conversion uses the wrong magic exponent",
     "src/kernels/decode.cpp",
     "0x4330000000000000LL", "0x4320000000000000LL", None),
    ("decode kernel: scaled-double tail drops its last value",
     "src/kernels/decode.cpp",
     "OffsetsToScaledDoubleScalar(off + i, n - i, base, scale, out + i);",
     "OffsetsToScaledDoubleScalar(off + i, n - i - (n - i > 0 ? 1 : 0), base, scale, out + i);",
     None),
    ("aggregate kernel: int32 sum counts the low half of each register twice",
     "src/kernels/aggregate.cpp",
     "_mm256_extracti128_si256(v, 1)));\n    }\n    alignas(32) int64_t lanes[4];\n    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);\n    return lanes[0]",
     "_mm256_extracti128_si256(v, 0)));\n    }\n    alignas(32) int64_t lanes[4];\n    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);\n    return lanes[0]",
     None),
    ("aggregate kernel: a lane overflow is not reported",
     "src/kernels/aggregate.cpp",
     "_mm256_castsi256_pd(overflow)) != 0", "_mm256_castsi256_pd(overflow)) == 7", None),
    ("aggregate kernel: overflow while combining the four lanes is ignored",
     "src/kernels/aggregate.cpp",
     "if (__builtin_add_overflow(total, lane, &total)) {",
     "if ((void)__builtin_add_overflow(total, lane, &total), false) {", None),
    ("aggregate kernel: the double sum forgets one lane",
     "src/kernels/aggregate.cpp",
     "(lanes[0] + lanes[1]) + (lanes[2] + lanes[3])", "(lanes[0] + lanes[1]) + lanes[2]", None),
    ("aggregate kernel: int32 MIN takes the maximum",
     "src/kernels/aggregate.cpp",
     "lo = _mm256_min_epi32(lo, v);", "lo = _mm256_max_epi32(lo, v);", None),
    ("aggregate kernel: int64 MIN/MAX compare the wrong way round",
     "src/kernels/aggregate.cpp",
     "lo = _mm256_blendv_epi8(lo, v, _mm256_cmpgt_epi64(lo, v));",
     "lo = _mm256_blendv_epi8(lo, v, _mm256_cmpgt_epi64(v, lo));", None),
    ("hash kernel: the vector multiply loses the cross term's shift",
     "src/kernels/hash.cpp",
     "_mm256_slli_epi64(cross, 32)", "_mm256_slli_epi64(cross, 31)", None),
    ("hash kernel: combining uses the wrong multiplier",
     "src/kernels/hash.cpp",
     "Fmix(_mm256_add_epi64(Mul64(seed, kCombineK), h))",
     "Fmix(_mm256_add_epi64(Mul64(seed, kMurmurC1), h))", None),
    ("hash kernel: int32 keys are zero-extended instead of sign-extended",
     "src/kernels/hash.cpp",
     "_mm256_cvtepi32_epi64(_mm_loadu_si128", "_mm256_cvtepu32_epi64(_mm_loadu_si128", None),
    ("executor: `constant < column` is evaluated as `column < constant`",
     "src/execution/expression_executor.cpp",
     "case OperatorKind::Lt:\n        return OperatorKind::Gt;",
     "case OperatorKind::Lt:\n        return OperatorKind::Lt;", None),
    ("executor: comparison with a NULL constant selects every row",
     "src/execution/expression_executor.cpp",
     "matches = 0; // a comparison with NULL is never TRUE", "matches = n;", None),
    ("executor: the kernel selection keeps rows whose column value is NULL",
     "src/execution/expression_executor.cpp",
     "kept += v.Valid(out[j]);", "kept += 1;", None),
    ("aggregate: the ungrouped integer SUM bound forgets that `count` values accumulate",
     "src/execution/aggregate_state.cpp",
     "if (bound <= (room - std::min(acc, room)) / count) {",
     "if (bound <= room - std::min(acc, room)) {", None),
    ("aggregate: the vectorised path is taken for a vector with NULLs",
     "src/execution/aggregate_state.cpp",
     "return v != nullptr && v->format() == VectorFormat::Flat && v->Validity().AllValid()",
     "return v != nullptr && v->format() == VectorFormat::Flat", None),
    ("aggregate: ungrouped MIN/MAX returns the opposite extreme",
     "src/execution/aggregate_state.cpp",
     "Offer(0, kMax ? hi : lo);", "Offer(0, kMax ? lo : hi);", None),
    ("hashing: dictionary strings hash the first row's entry for every row",
     "src/execution/hashing.cpp",
     "const sel_t code = u.sel[i];", "const sel_t code = u.sel[0];", None),
    ("hashing: a NULL dictionary row hashes its (empty) entry instead of the NULL hash",
     "src/execution/hashing.cpp",
     "uint64_t h = kNullHash;\n                    if (u.IsValid(i)) {",
     "uint64_t h = kNullHash;\n                    if (true) {", None),
]


def pattern_for(text):
    """Regex matching `text` with every whitespace run treated as flexible, so mutation anchors
    survive clang-format reflowing the code."""
    return re.compile(r"\s+".join(re.escape(tok) for tok in text.split()))


BACKUP_DIR = ROOT / "build" / "mutation_backup"


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")


def pattern_for(text):
    """Regex matching `text` with every whitespace run treated as flexible, so mutation anchors
    survive clang-format reflowing the code."""
    return re.compile(r"\s+".join(re.escape(tok) for tok in text.split()))


def recover_leftovers():
    """Restore originals left in the backup dir by a run that died mid-mutation."""
    if not BACKUP_DIR.exists():
        return
    for backup in sorted(BACKUP_DIR.rglob("*")):
        if backup.is_file():
            rel = backup.relative_to(BACKUP_DIR)
            (ROOT / rel).write_text(backup.read_text())
            backup.unlink()
            print(f"RECOVERED  restored {rel} from an interrupted mutation run")


def targets():
    return sorted({rel for _, rel, *_ in MUTATIONS})


def dirty_targets():
    return [f for f in targets() if run(["git", "diff", "--quiet", "--", f]).returncode != 0]


def _terminate(signum, _frame):
    raise SystemExit(128 + signum)  # unwinds through `finally`, which restores the file


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="debug")
    ap.add_argument("--only", default="")
    ap.add_argument("--check", action="store_true",
                    help="only validate that every anchor matches exactly once; build nothing")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="run even if mutation target files have uncommitted changes")
    args = ap.parse_args()

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, _terminate)

    recover_leftovers()
    dirty = dirty_targets()
    if dirty and not args.allow_dirty:
        print("REFUSING to run: these mutation targets have uncommitted changes, so a restored "
              "file could not be told apart from a mutated one:\n  " + "\n  ".join(dirty) +
              "\nCommit or stash them (or pass --allow-dirty).")
        return 2
    snapshot = {f: (ROOT / f).read_text() for f in targets()}

    wanted = [w for w in args.only.split("|") if w]  # `--only "parser:|lexer:"` = either substring
    selected = [m for m in MUTATIONS if not wanted or any(w in m[0] for w in wanted)]
    stale = [f"{rel}: {name}" for name, rel, old, _new, _needs in selected
             if len(pattern_for(old).findall((ROOT / rel).read_text())) != 1]
    if stale:
        print("ERROR  these mutation anchors no longer match exactly once (code changed?):\n  " +
              "\n  ".join(stale))
        return 2

    if args.check:
        print(f"all {len(selected)} mutation anchors match exactly once")
        return 0

    survivors = []
    ran = 0
    for name, rel, old, new, needs in selected:
        preset = needs or args.preset
        path = ROOT / rel
        original = path.read_text()
        pat = pattern_for(old)
        if len(pat.findall(original)) != 1:
            print(f"ERROR  mutation target not found exactly once in {rel}: {name}")
            return 2
        ran += 1
        backup = BACKUP_DIR / rel
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_text(original)
        try:
            path.write_text(pat.sub(lambda _m: new, original, count=1))
            build = run(["cmake", "--build", "--preset", preset])
            if build.returncode != 0:
                # A mutant that does not compile proves nothing about the tests: fix the mutant.
                tail = (build.stderr or build.stdout)[-300:]
                print(f"INVALID   [{preset}] mutant does not build: {name}\n    {tail!r}")
                survivors.append(name + " (invalid mutant)")
                continue
            tests = run(["ctest", "--preset", preset, "-j8", "--stop-on-failure"])
            if tests.returncode != 0:
                failed = [l.strip() for l in tests.stdout.splitlines() if "***Failed" in l
                          or "***Exception" in l or "Subprocess aborted" in l][:2]
                print(f"killed    [{preset}] {name}\n          by: {failed}")
            else:
                print(f"SURVIVED  [{preset}] {name}")
                survivors.append(name)
        finally:
            path.write_text(original)
            backup.unlink(missing_ok=True)

    # Integrity check: every mutated file must be exactly as we found it.
    damaged = [f for f in targets() if (ROOT / f).read_text() != snapshot[f]]
    if damaged:
        print("FATAL: files differ from their original content after the run: " + ", ".join(damaged))
        return 3

    # leave the build directories consistent with the restored sources
    for preset in {args.preset, "asan", "tsan"}:
        run(["cmake", "--build", "--preset", preset])

    print(f"\n{ran - len(survivors)}/{ran} mutations killed")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
