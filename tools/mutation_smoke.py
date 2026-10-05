#!/usr/bin/env python3
"""Mutation smoke test: proves the test suite can actually fail.

For each mutation below, one targeted bug is injected into the source (exact-text replace), the
project is rebuilt with the given preset, and the test suite is run. The mutation is "killed" if
the suite fails (or the build breaks the tests), "SURVIVED" if everything still passes - which
means a gap in the tests.  The source file is always restored afterwards.

Usage:  tools/mutation_smoke.py [--preset debug] [--only SUBSTRING]
Exit status is non-zero if any mutation survives.

Add a mutation whenever a new subsystem lands: pick a plausible, subtle bug (off-by-one, wrong
mask, missing null check, aliasing) rather than something that fails to compile.
"""
import argparse
import pathlib
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
     "if (!data_ || data_.use_count() > 1 || data_->size() < needed) {",
     "if (!data_ || data_->size() < needed) {", None),
    ("vector: Copy keeps pointers into the source's string heap",
     "src/vector/vector.cpp",
     "out[i] = in[s].IsInlined() ? in[s] : dst.AddString(in[s].view());", "out[i] = in[s];",
     None),
    ("validity: SetRangeValid last-word mask is off by one bit",
     "src/vector/validity_mask.cpp",
     "(uint64_t{1} << (end & 63)) - 1;", "(uint64_t{1} << (end & 63)) - 2;", None),
    ("vector: Copy memcpy fast path copies one element too few",
     "src/vector/vector.cpp",
     "std::memcpy(out, in, count * sizeof(T));", "std::memcpy(out, in, (count - 1) * sizeof(T));",
     None),
    ("vector: Copy forgets to mark overwritten NULL slots valid when the source has no NULLs",
     "src/vector/vector.cpp",
     "dst_validity.SetRangeValid(dst_offset, count);", "(void)0;", None),
    ("vector: Flatten of a dictionary drops the child's string heap (use-after-free)",
     "src/vector/vector.cpp",
     "        heap_ = child_->heap_;\n", "", "asan"),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="debug")
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    survivors = []
    ran = 0
    for name, rel, old, new, needs in MUTATIONS:
        if args.only and args.only not in name:
            continue
        preset = needs or args.preset
        path = ROOT / rel
        original = path.read_text()
        if original.count(old) != 1:
            print(f"ERROR  mutation target not found exactly once in {rel}: {name}")
            return 2
        ran += 1
        try:
            path.write_text(original.replace(old, new))
            build = run(["cmake", "--build", "--preset", preset])
            if build.returncode != 0:
                tail = (build.stderr or build.stdout)[-300:]
                print(f"BUILD-BROKEN (counts as killed)  [{preset}] {name}\n    {tail!r}")
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

    # leave the build directories consistent with the restored sources
    for preset in {args.preset, "asan"}:
        run(["cmake", "--build", "--preset", preset])

    print(f"\n{ran - len(survivors)}/{ran} mutations killed")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
