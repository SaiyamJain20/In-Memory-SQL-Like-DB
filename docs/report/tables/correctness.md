| Engine | H2O groupby, 1 thr | H2O groupby, 16 thr | H2O join, 1 thr | H2O join, 16 thr | micro, 1 thr | micro, 16 thr | TPC-H SF0.1, 1 thr | TPC-H SF0.1, 16 thr | TPC-H SF1, 1 thr | TPC-H SF1, 16 thr | Not matching the reference answer |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| cdb | 7 / 7 | 7 / 7 | 5 / 5 | 5 / 5 | 24 / 24 | 24 / 24 | 22 / 22 | 22 / 22 | 22 / 22 | 22 / 22 | all answers match |
| DuckDB | 10 / 10 | 10 / 10 | 5 / 5 | 5 / 5 | 24 / 24 | 24 / 24 | 22 / 22 | 22 / 22 | 22 / 22 | 22 / 22 | all answers match |
| DataFusion | 10 / 10 | 10 / 10 | 5 / 5 | 5 / 5 | 24 / 24 | 24 / 24 | 22 / 22 | 22 / 22 | 22 / 22 | 21 / 22 | Q15 (mismatch: 0 rows, expected 1) at sf1_t16 |
| ClickHouse (chDB) | 10 / 10 | 10 / 10 | 5 / 5 | 5 / 5 | 24 / 24 | 24 / 24 | 22 / 22 | 22 / 22 | 22 / 22 | 21 / 22 | Q15 (mismatch: 0 rows, expected 1) at sf1_t16 |
| Polars | 10 / 10 | 10 / 10 | 5 / 5 | 5 / 5 | 24 / 24 | 24 / 24 | 22 / 22 | 22 / 22 | 22 / 22 | 22 / 22 | all answers match |
| SQLite | – | – | – | – | – | – | 22 / 22 | – | 22 / 22 | – | all answers match |
| DuckDB (DECIMAL) | – | – | – | – | – | – | 22 / 22 | 22 / 22 | 22 / 22 | 22 / 22 | all answers match |
| ClickHouse (MergeTree) | – | – | – | – | – | – | 22 / 22 | 22 / 22 | 22 / 22 | 21 / 22 | Q15 (mismatch: 0 rows, expected 1) at sf1_t16 |
