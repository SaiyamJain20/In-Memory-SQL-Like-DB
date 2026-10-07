## 2. How cdb works

cdb is a columnar, vectorized SQL analytics engine written in C++20: about {{n:loc_src}} lines of engine
code and {{n:loc_tests}} lines of tests. It is an embedded library with a small shell, in the same family as DuckDB,
ClickHouse and Velox: data is stored by column, queries run over batches of values (vectors) instead of one row at a
time, and the work is spread over cores. The design is written down decision by decision in
[`docs/adr/`](adr/) and [`docs/ARCHITECTURE.md`](ARCHITECTURE.md); this chapter is the tour a reader needs to
interpret the measurements that follow, and every claim in it can be traced to a module and a test.

```
 SQL text ─▶ lexer / parser ─▶ AST ─▶ binder ─▶ logical plan ─▶ optimizer ─▶ physical plan ─▶ pipelines
              (hand written)          names,      relational      statistics,     operators        source ▶ ops ▶ sink
                                      types,      operators       cardinality,    + local/global   run by a thread
                                      subquery    over bound      join order,     state            pool on morsels
                                      unnesting   expressions     pushdown
                                                                                                   │
        scans ◀── columnar storage: row groups of column segments (encodings, zone maps, HyperLogLog) ◀┘
                       │
                       └─ optional persistence: write-ahead log + checkpoints behind a FileSystem interface
```

### 2.1 Data model: vectors, selection vectors, strings

The unit of execution is the **vector**: up to 2,048 values of one type (16 KiB for 8-byte values, small enough that the
handful of vectors a pipeline touches stay in L1/L2). A vector is in one of three formats: *flat* (contiguous values and a
validity bitmask), *constant* (one value repeated, for literals) or *dictionary* (a child vector plus a selection vector,
which is also what a filter produces). Kernels never branch on the format inside a loop: each converts its input to a
unified view `(data, selection, validity)` once per call and indexes `data[sel[i]]`, with a specialised path for flat
inputs. NULLs are one bit per value, and a vector without NULLs carries no mask at all, so the common case costs a pointer
check instead of a bit test per row.

A **filter does not copy data**: it produces a selection vector (an array of row indices) and downstream operators read
through it, so filtering a chunk is work proportional to the rows kept. Strings are 16-byte views (a length, then either
12 inline bytes or a 4-byte prefix and a pointer), so strings up to 12 bytes need no heap, and most comparisons and
`LIKE 'abc%'` checks are decided by the prefix without a dereference.

### 2.2 Storage: row groups, immutable segments, encodings, zone maps

A table is a list of **row groups** of 122,880 rows (60 vectors); a row group holds one **column segment** per column.
Segments are immutable once sealed. A raw segment is laid out exactly like a flat vector (values, validity, sealed string
heap), so scanning it is pointing the output vector at read-only buffers: no copy, no allocation. Appends fill a mutable
builder; a full builder is sealed without copying, and `Table::Snapshot()` returns the sealed groups plus a frozen copy of
the open tail, so **readers get snapshot isolation and never take a lock while scanning**.

When a row group seals, each segment is **encoded** if that makes it at most 70% of its raw size (otherwise it stays raw,
keeping its zero-copy scans): constant, run-length, bit-packed integers (frame of reference, or per-vector deltas for
non-decreasing data), scaled doubles (`n / 10^e`, checked bit for bit, so it is lossless, with a per-vector raw fallback)
and dictionary strings (at most 2,047 entries; a dictionary segment hands out dictionary-format vectors over one shared
dictionary, so group-by and join hash each distinct string once). Decoding is per vector. On TPC-H SF1 the stored data
shrinks from 1,408 MB raw to {{n:stored_mb_sf1}} MB ({{n:stored_ratio}}x).

Every segment carries **zone maps** (exact min, max and NULL count) used to skip whole row groups when a predicate cannot
match, and, since Phase 8, a **HyperLogLog sketch** of its distinct values (4,096 one-byte registers, 1.6% standard error),
built when the segment seals and merged across segments into per-table statistics for the optimizer.

### 2.3 SQL front end

A hand-written lexer and recursive-descent parser produce a syntax-only AST (every node keeps its source offset;
`parse → print → parse` is a fixpoint, tested on a corpus, all 22 TPC-H queries and fuzzed input; nesting is depth-bounded
so hostile input cannot overflow the stack). The **binder** resolves names with scopes (aliases, `USING`, derived tables),
infers types with explicit casts, folds constants, extracts aggregates under SQL's `GROUP BY / HAVING / ORDER BY` rules and
produces a logical plan of relational operators over bound expressions. A single scalar interpreter defines the semantics of
every expression and is the oracle for the vectorized kernels; it is itself checked against DuckDB on about 4,300
generated expressions.

**Subqueries are unnested while binding**, so no operator evaluates a subquery per row: `[NOT] EXISTS` and `[NOT] IN`
become semi / anti joins (`NOT IN` a null-aware anti join that implements SQL's three-valued logic), an uncorrelated scalar
subquery a cross join with its one row (guarded so that more than one row is an error), and a correlated scalar aggregate a
`LEFT` join with the aggregate grouped by the correlation keys, with the aggregate's empty-input value (`0` for `COUNT`) for
outer rows that have no group. Non-recursive `WITH` is inlined through a scope chain. Shapes outside this table (correlated
`NOT IN`, `IN` under `OR`, `WITH RECURSIVE`) fail with a positioned `NotImplemented` error rather than a wrong answer.

### 2.4 The optimizer

Phase 4 had a rule-based optimizer (filter pushdown that is aware of outer joins, OR factoring, column pruning, zone-map
hints, top-N). Phase 8 added the cost-based part:

* **Cardinality estimation** (`planner/cardinality`): rows and per-column statistics for every operator, from the table
  statistics with the textbook assumptions (independence between predicates, uniformity between the bounds, containment of
  join keys): equality is `1/distinct`, ranges are read off a grid of `distinct` values between the bounds, a lower and an
  upper bound on one column form an interval, `OR` uses inclusion–exclusion, a join of two keys has `1/max(distinct)` of the
  product of its inputs, a semi / anti join keeps the share of the left key *domain* that the right keys cover, and an
  aggregate has the product of its group columns' distinct counts.
* **Join ordering** (`planner/join_order`): a tree of inner joins is flattened into relations (with their own filters
  pushed down and estimated) and predicates; dynamic programming over subsets finds the cheapest tree up to 12 relations
  (bushy trees included; cost = rows out + 2 × rows built + rows probed; the smaller input is the build side; no cross
  product where a predicate can avoid one), a greedy left-deep order up to 60, and the order as written beyond.
* **Rules** that use the estimates: a semi / anti join sinks below an inner join only if it shrinks its input and the join
  below it is not just a comparison against a one-row subquery; filters commute past semi / anti joins.

`EXPLAIN` prints the plan with `(~N rows)`; `EXPLAIN ANALYZE` runs it and prints, per operator, estimated rows, actual rows,
the build side of each join and CPU time, which is how the estimator was debugged and how chapter 6.6 compares it with
DuckDB's.

### 2.5 Execution: push-based pipelines and morsel-driven parallelism

A physical plan is cut into **pipelines** `source → streaming operators → sink`, run in dependency order. A pipeline breaker
(hash aggregate, join build, sort, top-N) is the sink of one pipeline and the source of the next. Operators follow a
three-role protocol with **global** state shared by the threads of a pipeline and **local** state private to one: a hash
aggregate fills a thread-local table and merges it in `Combine`; a join build collects rows locally and publishes them in
`Finalize`; a satisfied `LIMIT` returns `Finished` and stops the source. The push model puts the control flow in the
pipeline rather than in the operators, so adding threads did not require changing any operator (ADR 0005).

The pool (`TaskScheduler`) is fixed-size; a pipeline runs on several threads only if its source, every operator and its sink
allow it. Each thread claims **morsels** of 8 vectors (never across a row group) from an atomic cursor. The parallel
operators:

| Operator | How it parallelises |
|---|---|
| hash aggregate | per-thread group tables merged by hash partition (from 32,768 groups; one task per partition, no locks) |
| hash join | per-thread row stores adopted whole, hashed in parallel, linked into bucket chains without atomics (radix scatter by bucket partition); probing is per thread |
| sort | parallel stable merge sort with co-rank slices, identical to the serial stable order; top-N prunes per thread first |
| CSV load | mmap, record boundaries found in parallel, whole row groups parsed, sealed and compressed per task, published in file order |

Floating-point `SUM` / `AVG` are accumulated in compensated (double-double) form and rounded once, so they are
bit-identical on any number of threads and with or without SIMD. This was found the hard way: TPC-H Q15 compares a sum with
the maximum of the same sum computed a second time, and with a plain parallel sum the two differ in the last bits and the
query returns nothing (chapter 6.1 shows two other engines still do).

Hot loops have AVX2 kernels next to scalar ones (compare a column against a constant into a selection vector, scaled-double
decode, ungrouped sum / min / max, integer hashing), dispatched at run time with `__builtin_cpu_supports`; the build never
uses `-march=native`, and a kernel is only kept if it measured faster than what the compiler already generates.

### 2.6 Persistence

A database can be a directory: `checkpoint-<epoch>.cdb` (a complete snapshot, each segment stored in its in-memory
encoding with checksums) plus `wal-<epoch>.log` (frames of `[length][CRC-32C][sequence][flags][payload]`, the last of a
statement carrying a commit flag). A commit is "validate, log + fsync, apply to memory" under one mutex; recovery loads the
newest checkpoint and replays the log, cutting a torn tail at the last complete commit. All I/O goes through a `FileSystem`
interface whose in-memory implementation models what a power cut leaves behind, which is how the crash campaign (a crash at
every write, fsync, rename and directory fsync, under six policies, then a second crash during recovery) is deterministic.
The engine is in-memory first: tables are loaded at open and scanned zero-copy, so persistence is a log plus snapshots, not a
buffer pool. Chapter 6.8 measures its costs.

### 2.7 A query end to end: TPC-H Q3

{{include:explain_q03}}

### 2.8 What cdb does not have

No spill to disk (hash tables, sorts and the whole database are in memory), no window functions, `UPDATE` / `DELETE`,
`FULL` joins, `WITH RECURSIVE`, histograms, or semi-join reduction of decorrelated aggregates; `INTERVAL` arithmetic works
on constants only; one writer at a time; no group commit (every commit pays its own fsync). The tables of chapter 6 mark
every place where one of these decides a result, as `n/a` or as a cost.
