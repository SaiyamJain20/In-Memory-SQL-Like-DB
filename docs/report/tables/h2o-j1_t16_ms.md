| Query | cdb | DuckDB | DataFusion | ClickHouse (chDB) | Polars |
| :--- | ---: | ---: | ---: | ---: | ---: |
| j1_small_inner | 48.8 | 5.60 | 10.6 | 23.0 | 45.6 |
| j2_medium_inner | 47.4 | 5.88 | 12.1 | 21.9 | 41.5 |
| j3_medium_outer | 48.6 | 23.4 | 17.1 | 23.1 | 42.4 |
| j4_medium_inner_str | 63.6 | 38.8 | 44.6 | 37.0 | 134 |
| j5_big_inner | 507 | 244 | 254 | 308 | 240 |
| **geometric mean** (ms) | **83.2** | **23.6** | **30.1** | **42.2** | **76.1** |
