# TPC-H query of pola-rs/polars-benchmark (Apache License 2.0, https://github.com/pola-rs/polars-benchmark,
# queries/polars/q14.py at commit 401908a307f133a2dffbe3f5e0e0fb2b50ea310b), adapted mechanically for the harness by
# bench/report/make_polars_queries.py: in-memory tables, no .round(2), Q11's constant fraction,
# and the query(tables) entry point at the end.
from datetime import date
from typing import Any

import polars as pl

import _polars_utils as utils

Q_NUM = 14


def q(
    lineitem: None | pl.LazyFrame = None,
    part: None | pl.LazyFrame = None,
    **kwargs: Any,
) -> pl.LazyFrame:
    if lineitem is None:
        lineitem = utils.get_line_item_ds()
        part = utils.get_part_ds()

    assert lineitem is not None
    assert part is not None

    var1 = date(1995, 9, 1)
    var2 = date(1995, 10, 1)

    return (
        lineitem.join(part, left_on="l_partkey", right_on="p_partkey")
        .filter(pl.col("l_shipdate").is_between(var1, var2, closed="left"))
        .select(
            (
                100.00
                * pl.when(pl.col("p_type").str.starts_with("PROMO"))
                .then(pl.col("l_extendedprice") * (1 - pl.col("l_discount")))
                .otherwise(0)
                .sum()
                / (pl.col("l_extendedprice") * (1 - pl.col("l_discount"))).sum()
            )
            
            .alias("promo_revenue")
        )
    )


if __name__ == "__main__":
    utils.run_query(Q_NUM, q())


def query(tables):
    utils.TABLES = tables
    return q()
