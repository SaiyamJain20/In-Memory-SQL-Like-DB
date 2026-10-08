# cdb against DuckDB, DataFusion, ClickHouse, Polars and SQLite

*An evaluation of a from-scratch columnar, vectorized SQL engine: how it works, how it was built and tested, and how it
compares with the engines it is modelled on, on the standard analytical workloads.*

Measured on {{n:results_date}} on one machine (AMD Ryzen 7 6800H, 8 cores / 16 threads, 14 GB) **that was in normal desktop use
while the measurements ran** (§5.1). Raw data, harness and a one-command reproduction are in the repository
(`bench/report/`); every table and number below is generated from the raw files (`bench/report/analyze.py`).

| | |
|---|---|
| [1. Summary](#1-summary) | what was measured, the headline result, the findings |
| [2. How cdb works](#2-how-cdb-works) | vectors, storage and encodings, the SQL front end, the optimizer, execution, persistence |
| [3. The systems compared](#3-the-systems-compared) | why these five, their design, where "the same query" is not the same |
| [4. Tools](#4-tools) | how the engine is built and checked; the measurement harness |
| [5. Methodology and environment](#5-methodology-and-environment) | workloads, fairness rules, **the machine and its noise** |
| [6. Results](#6-results) | correctness, TPC-H, scaling, resources, operators, H2O-style, optimizer quality, durability |
| [7. Analysis](#7-analysis-where-cdb-wins-where-it-loses-and-why) | why, from instruction-level profiles |
| [8. Threats to validity](#8-threats-to-validity-and-limitations) | what this report cannot claim |
| [9. Reproducing](#9-reproducing-this-report) | commands |
| [Appendix](#appendix) | per-query times, configuration, versions |
