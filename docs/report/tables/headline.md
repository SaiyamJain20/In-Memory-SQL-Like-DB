| Engine | TPC-H SF1, 1 thread | SF1, 16 threads | SF0.1, 16 threads | operators, 1 thread | H2O groupby, 1 thread | H2O join, 1 thread | Peak RSS, SF1, 1 thread (MB) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cdb | 2.48 | 1.54 | 0.51 | 2.21 | 1.31 | 3.50 | 1,234 |
| DuckDB | 1 | 1 | 1 | 1 | 1 | 1 | 1,659 |
| DataFusion | 1.21 | 1.58 | 1.03 | 0.70 | 0.77 | 0.80 | 2,242 |
| ClickHouse (chDB) | 1.83 | 2.16 | 1.80 | 0.63 | 0.90 | 1.07 | 3,993 |
| Polars | 1.05 | 1.00 | 0.70 | 0.75 | 1.17 | 1.47 | 2,514 |
| SQLite | 13.62 | – | – | – | – | – | 1,957 |
