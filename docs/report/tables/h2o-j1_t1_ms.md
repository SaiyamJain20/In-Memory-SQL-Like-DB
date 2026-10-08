| Query | cdb | DuckDB | DataFusion | ClickHouse (chDB) | Polars |
| :--- | ---: | ---: | ---: | ---: | ---: |
| j1_small_inner | 350 | 34.1 | 43.2 | 78.6 | 98.5 |
| j2_medium_inner | 329 | 35.0 | 47.4 | 85.1 | 120 |
| j3_medium_outer | 333 | 142 | 67.8 | 79.2 | 117 |
| j4_medium_inner_str | 458 | 311 | 193 | 194 | 293 |
| j5_big_inner | 2,154 | 1,261 | 825 | 991 | 1,309 |
| **geometric mean** (ms) | **519** | **148** | **119** | **160** | **227** |
