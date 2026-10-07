# TPC-H query of pola-rs/polars-benchmark (Apache License 2.0, https://github.com/pola-rs/polars-benchmark,
# queries/polars/q22.py at commit 401908a307f133a2dffbe3f5e0e0fb2b50ea310b), adapted mechanically for the harness by
# bench/report/make_polars_queries.py: in-memory tables, no .round(2), Q11's constant fraction,
# and the query(tables) entry point at the end.
from typing import Any

import polars as pl

import _polars_utils as utils

Q_NUM = 22


def q(
    customer: None | pl.LazyFrame = None,
    orders: None | pl.LazyFrame = None,
    **kwargs: Any,
) -> pl.LazyFrame:
    if customer is None:
        customer = utils.get_customer_ds()
        orders = utils.get_orders_ds()

    assert customer is not None
    assert orders is not None

    q1 = (
        customer.with_columns(pl.col("c_phone").str.slice(0, 2).alias("cntrycode"))
        .filter(pl.col("cntrycode").str.contains("13|31|23|29|30|18|17"))
        .select("c_acctbal", "c_custkey", "cntrycode")
    )

    q2 = q1.filter(pl.col("c_acctbal") > 0.0).select(
        pl.col("c_acctbal").mean().alias("avg_acctbal")
    )

    return (
        q1.join(orders, left_on="c_custkey", right_on="o_custkey", how="anti")
        .join(q2, how="cross")
        .filter(pl.col("c_acctbal") > pl.col("avg_acctbal"))
        .group_by("cntrycode")
        .agg(
            pl.col("c_acctbal").count().alias("numcust"),
            pl.col("c_acctbal").sum().alias("totacctbal"),
        )
        .sort("cntrycode")
    )


if __name__ == "__main__":
    utils.run_query(Q_NUM, q())


def query(tables):
    utils.TABLES = tables
    return q()
