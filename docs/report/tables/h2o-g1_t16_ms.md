| Query | cdb | DuckDB | DataFusion | ClickHouse (chDB) | Polars |
| :--- | ---: | ---: | ---: | ---: | ---: |
| g1_sum_v1_by_id1 | 19.4 | 39.4 | 17.3 | 10.8 | 25.0 |
| g2_sum_v1_by_id1_id2 | 52.2 | 64.1 | 47.3 | 138 | 101 |
| g3_sum_v1_mean_v3_by_id3 | 269 | 165 | 201 | 189 | 133 |
| g4_mean_v1v2v3_by_id4 | 25.2 | 9.17 | 15.6 | 14.3 | 26.9 |
| g5_sum_v1v2v3_by_id6 | 295 | 162 | 149 | 183 | 77.2 |
| g6_median_sd_by_id4_id5 | n/a | 192 | 195 | 169 | 176 |
| g7_range_v1v2_by_id3 | 222 | 123 | 167 | 167 | 113 |
| g8_top2_v3_by_id6 | n/a | 212 | 317 | 430 | 238 |
| g9_corr_by_id2_id4 | n/a | 98.6 | 86.9 | 133 | 130 |
| g10_sum_v3_count_by_id1_6 | 871 | 384 | 528 | 527 | 618 |
| **geometric mean** (ms) | **122** | **83.5** | **85.3** | **96.0** | **91.1** |
