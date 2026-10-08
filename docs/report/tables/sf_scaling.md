| Engine | Threads | SF0.1 (ms) | SF1 (ms) | Growth |
| :--- | ---: | ---: | ---: | ---: |
| cdb | 1 | 10.3 | 136 | 13.2x over 10x the data |
| DuckDB | 1 | 7.59 | 55.0 | 7.2x over 10x the data |
| DataFusion | 1 | 8.56 | 67.1 | 7.8x over 10x the data |
| ClickHouse (chDB) | 1 | 13.6 | 101 | 7.4x over 10x the data |
| Polars | 1 | 7.00 | 57.7 | 8.2x over 10x the data |
| SQLite | 1 | 44.3 | 759 | 17.1x over 10x the data |
| cdb | 16 | 3.43 | 29.2 | 8.5x over 10x the data |
| DuckDB | 16 | 6.68 | 18.9 | 2.8x over 10x the data |
| DataFusion | 16 | 6.91 | 29.7 | 4.3x over 10x the data |
| ClickHouse (chDB) | 16 | 12.1 | 40.4 | 3.3x over 10x the data |
| Polars | 16 | 4.65 | 18.9 | 4.1x over 10x the data |
