# TPC-H query of pola-rs/polars-benchmark (Apache License 2.0, https://github.com/pola-rs/polars-benchmark,
# queries/polars/q1.py at commit 401908a307f133a2dffbe3f5e0e0fb2b50ea310b), adapted mechanically for the harness by
# bench/report/make_polars_queries.py: in-memory tables, no .round(2), Q11's constant fraction,
# and the query(tables) entry point at the end.
from datetime import date
from typing import Any

import polars as pl

import _polars_utils as utils

Q_NUM = 1


def q(lineitem: None | pl.LazyFrame = None, **kwargs: Any) -> pl.LazyFrame:
    if lineitem is None:
        lineitem = utils.get_line_item_ds()

    var1 = date(1998, 9, 2)

    return (
        lineitem.filter(pl.col("l_shipdate") <= var1)
        .group_by("l_returnflag", "l_linestatus")
        .agg(
            pl.sum("l_quantity").alias("sum_qty"),
            pl.sum("l_extendedprice").alias("sum_base_price"),
            (pl.col("l_extendedprice") * (1.0 - pl.col("l_discount")))
            .sum()
            .alias("sum_disc_price"),
            (
                pl.col("l_extendedprice")
                * (1.0 - pl.col("l_discount"))
                * (1.0 + pl.col("l_tax"))
            )
            .sum()
            .alias("sum_charge"),
            pl.mean("l_quantity").alias("avg_qty"),
            pl.mean("l_extendedprice").alias("avg_price"),
            pl.mean("l_discount").alias("avg_disc"),
            pl.len().alias("count_order"),
        )
        .sort("l_returnflag", "l_linestatus")
    )


if __name__ == "__main__":
    utils.run_query(Q_NUM, q())


def query(tables):
    utils.TABLES = tables
    return q()
