# TPC-H query of pola-rs/polars-benchmark (Apache License 2.0, https://github.com/pola-rs/polars-benchmark,
# queries/polars/q4.py at commit 401908a307f133a2dffbe3f5e0e0fb2b50ea310b), adapted mechanically for the harness by
# bench/report/make_polars_queries.py: in-memory tables, no .round(2), Q11's constant fraction,
# and the query(tables) entry point at the end.
from datetime import date
from typing import Any

import polars as pl

import _polars_utils as utils

Q_NUM = 4


def q(
    lineitem: None | pl.LazyFrame = None,
    orders: None | pl.LazyFrame = None,
    **kwargs: Any,
) -> pl.LazyFrame:
    if lineitem is None:
        lineitem = utils.get_line_item_ds()
        orders = utils.get_orders_ds()

    assert lineitem is not None
    assert orders is not None

    var1 = date(1993, 7, 1)
    var2 = date(1993, 10, 1)

    return (
        # SQL exists translates to semi join in Polars API
        orders.join(
            (lineitem.filter(pl.col("l_commitdate") < pl.col("l_receiptdate"))),
            left_on="o_orderkey",
            right_on="l_orderkey",
            how="semi",
        )
        .filter(pl.col("o_orderdate").is_between(var1, var2, closed="left"))
        .group_by("o_orderpriority")
        .agg(pl.len().alias("order_count"))
        .sort("o_orderpriority")
    )


if __name__ == "__main__":
    utils.run_query(Q_NUM, q())


def query(tables):
    utils.TABLES = tables
    return q()
