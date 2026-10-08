| Engine | Durable commit: mean / p99 | Commits per second | Not durable: mean | Bulk load SF1 (s) | On disk (MB) | Reopen + Q6 (s) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| cdb | 0.53 ms / 0.84 | 1,884 | 0.028 ms | 9.8 | 506 | 0.32 |
| SQLite | 0.43 ms / 0.70 | 2,329 | 0.011 ms | 38.4 | 1,096 | 0.58 |
| DuckDB | 1.11 ms / 5.73 | 901 | – | 8.3 | 260 | 0.02 |
| ClickHouse (MergeTree) | 5.17 ms / 15.72 | 194 | 1.321 ms | 8.9 | 770 | 0.08 |
