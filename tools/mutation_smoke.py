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
    ("eval: LIKE '_' consumes one byte instead of one UTF-8 character",
     "src/planner/scalar_eval.cpp",
     "t += std::min(Utf8Len(static_cast<unsigned char>(text[t])), text.size() - t);\n            p++;", "t += 1;\n            p++;", None),
    ("eval: substring treats start 0 like start 1",
     "src/planner/scalar_eval.cpp",
     "int64_t begin = start > 0 ? start - 1 : (start == 0 ? -1 : n + start);", "int64_t begin = start > 0 ? start - 1 : (start == 0 ? 0 : n + start);", None),
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
