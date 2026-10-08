| Query | cdb instructions (M) | DuckDB (M) | cdb / DuckDB | cdb simulated LL read misses (k) | DuckDB (k) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| micro: top-10 of 1M rows | 1,869 | 120 | 15.5 | 109 | 144 |
| micro: sort 1M rows | 2,771 | 583 | 4.8 | 1,707 | 216 |
| micro: join, 1,000-row build | 346 | 67 | 5.2 | 149 | 209 |
| micro: join, 1M-row build | 488 | 241 | 2.0 | 4,210 | 1,508 |
| micro: 100,000 groups | 190 | 112 | 1.7 | 293 | 1,169 |
| micro: 1M groups | 243 | 220 | 1.1 | 1,539 | 1,319 |
| micro: count(*) | 15 | 2 | 8.3 | 3 | 5 |
| micro: sum of a column | 11 | 8 | 1.3 | 35 | 134 |
| micro: filter 90%, sum | 49 | 20 | 2.5 | 70 | 199 |
| TPC-H Q1, SF0.1 | 328 | 211 | 1.6 | 190 | 465 |
| TPC-H Q6 | 47 | 34 | 1.4 | 63 | 126 |
| TPC-H Q9 | 498 | 124 | 4.0 | 884 | 329 |
| TPC-H Q17 | 181 | 46 | 3.9 | 128 | 99 |
| TPC-H Q20 | 112 | 48 | 2.3 | 166 | 141 |
