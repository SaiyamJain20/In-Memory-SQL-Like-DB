| Engine | Threads | Load, all 8 tables (s) | Peak RSS after load (MB) | Peak RSS after all queries (MB) | cdb storage (MB) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| cdb | 1 | 8.4 | 713 | 1,234 | 612 |
| DuckDB | 1 | 6.6 | 1,442 | 1,659 | – |
| DataFusion | 1 | 3.2 | 1,419 | 2,242 | – |
| ClickHouse (chDB) | 1 | 1.6 | 2,590 | 3,993 | – |
| Polars | 1 | 3.4 | 2,514 | 2,514 | – |
| SQLite | 1 | 45.7 | 1,952 | 1,957 | – |
| cdb | 16 | 1.8 | 1,340 | 1,357 | 612 |
| DuckDB | 16 | 2.6 | 2,439 | 2,464 | – |
| DataFusion | 16 | 0.8 | 1,488 | 2,455 | – |
| ClickHouse (chDB) | 16 | 1.7 | 2,604 | 5,008 | – |
| Polars | 16 | 0.9 | 2,729 | 2,729 | – |
