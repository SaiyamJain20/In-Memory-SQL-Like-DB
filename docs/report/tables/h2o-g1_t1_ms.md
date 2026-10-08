| Query | cdb | DuckDB | DataFusion | ClickHouse (chDB) | Polars |
| :--- | ---: | ---: | ---: | ---: | ---: |
| g1_sum_v1_by_id1 | 122 | 144 | 83.8 | 51.1 | 121 |
| g2_sum_v1_by_id1_id2 | 310 | 288 | 237 | 264 | 153 |
| g3_sum_v1_mean_v3_by_id3 | 442 | 348 | 284 | 255 | 569 |
| g4_mean_v1v2v3_by_id4 | 137 | 56.4 | 63.4 | 59.9 | 99.1 |
| g5_sum_v1v2v3_by_id6 | 429 | 184 | 138 | 230 | 268 |
| g6_median_sd_by_id4_id5 | n/a | 498 | 278 | 369 | 651 |
| g7_range_v1v2_by_id3 | 364 | 300 | 244 | 248 | 450 |
| g8_top2_v3_by_id6 | n/a | 282 | 3,602 | 2,313 | 1,047 |
| g9_corr_by_id2_id4 | n/a | 272 | 290 | 240 | 196 |
| g10_sum_v3_count_by_id1_6 | 1,659 | 1,972 | 1,127 | 3,698 | 2,144 |
| **geometric mean** (ms) | **346** | **264** | **204** | **239** | **308** |
