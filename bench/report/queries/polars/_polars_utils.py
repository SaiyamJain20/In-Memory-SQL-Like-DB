"""Stands in for queries/polars/utils.py of polars-benchmark: the tables are the harness's, already in
memory (a dict of DataFrames set by each query's `query(tables)`), and a query reads them lazily."""

TABLES = {}


def _lazy(name):
    return TABLES[name].lazy()


def get_line_item_ds():
    return _lazy("lineitem")


def get_orders_ds():
    return _lazy("orders")


def get_customer_ds():
    return _lazy("customer")


def get_region_ds():
    return _lazy("region")


def get_nation_ds():
    return _lazy("nation")


def get_supplier_ds():
    return _lazy("supplier")


def get_part_ds():
    return _lazy("part")


def get_part_supp_ds():
    return _lazy("partsupp")
