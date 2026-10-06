# ADR 0007 — Per-segment encodings chosen at seal time; AVX2 kernels with runtime dispatch

- **Status:** accepted
- **Date:** 2026-10-06

## Context
Raw columnar storage was 1.4 GB for TPC-H SF1 and scans of it are zero-copy, which is hard to beat for
speed but expensive in memory. Warehouse columns are usually highly compressible (narrow ranges, few
distinct strings, two-decimal money). We also want hand-vectorised kernels for the hot loops, on a
machine with AVX2 but no AVX-512, in a project whose CI and users run on unknown CPUs.

## Decision
**Encodings.** A segment is immutable once sealed, so its encoding is chosen then, from its data:
constant, RLE, bit-packed integers (frame of reference, or delta for non-decreasing vectors), scaled
doubles (`n / 10^e`, checked bit for bit so the encoding is lossless, per-vector raw fallback), and
dictionary strings. An encoding is used only if it is at most 70% of the raw size; otherwise the
segment stays raw and keeps its zero-copy scans (decoding costs CPU, so marginal savings are not
worth it). The open tail of a table is never encoded. Zone-map statistics are computed from the raw
data and are unchanged. Decoding is per 2048-row vector, into the output vector, except that
constant vectors decode to CONSTANT vectors and dictionary segments hand back DICTIONARY-format
vectors over one shared dictionary: operators that already read through `UnifiedFormat` work on the
compressed form without any change, and hashing (group-by, join) hashes each distinct dictionary entry
once.

**Kernels.** Hot loops get an AVX2 implementation next to a scalar one, in `src/kernels/`. AVX2
functions carry a per-function `target("avx2")` attribute (the build never uses `-march=native`) and are
called only after `__builtin_cpu_supports("avx2")`; `CDB_NO_SIMD` and `SetSimdEnabled` force the scalar
version, which is also the reference every AVX2 kernel is tested against (every length 0-70 and the
usual tails, special values, NULL-free and constant/dictionary inputs). A kernel is kept only if it
measures faster than the scalar loop the compiler generates: two were deleted or rewritten after
measuring slower.

**Semantics must not depend on the CPU.** The same query returns the same answer with and without SIMD,
except where floating-point addition is re-associated (ungrouped `SUM(DOUBLE)`, documented, still
deterministic for a given input). Integer `SUM` uses the vector path only when it is provable that a
left-to-right checked sum could not overflow either, so overflow errors are identical.

## Consequences
- Memory: TPC-H SF1 shrinks 2.3x (lineitem 2.9x). Cost: compressed scans decode, 0-30% on scan-bound
  queries; the raw layout remains available (`SetCompressionEnabled(false)`) for measurement.
- Encodings are an interface (`EncodedColumn::DecodeVector`), so adding one (FSST, ALP exceptions,
  sorted-dictionary) does not touch operators.
- A dictionary is limited to 2,047 entries (a vector holds at most 2,048 values); larger ones stay raw.
- Selection vectors, dictionary vectors and decoded outputs may now hold unspecified bytes in rows the
  producer did not write (`FlatDataForOverwrite`, `SelectionVector::Uninitialized`); the public
  constructors still zero-fill, and strings are always zero-filled.

## Alternatives considered
- **`-march=native` / building for AVX2 only:** simplest and fastest, but a binary that crashes on a
  CPU without AVX2 is not acceptable for a library others build.
- **`target_clones`/ifunc auto-vectorisation only:** useful for simple loops (the compiler already
  vectorises the integer decode loops), but the compaction, overflow detection and exact conversions
  here need explicit intrinsics.
- **A SIMD abstraction library (Highway, xsimd):** a good option for a larger kernel set; for five
  kernels the direct intrinsics are easier to read and to test against the scalar reference.
- **Heavier compression (FSST, LZ4, full ALP):** more savings on strings and floating point at a
  higher decode cost and much more code; the lightweight set already gets most of the win on TPC-H.
