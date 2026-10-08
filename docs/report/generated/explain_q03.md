The query (TPC-H Q03, shipping priority) joins three tables, filters two of them, groups, sorts and keeps the ten best groups:

```sql
SELECT
    l_orderkey,
    sum(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM
    customer,
    orders,
    lineitem
WHERE
    c_mktsegment = 'BUILDING'
    AND c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND o_orderdate < CAST('1995-03-15' AS date)
    AND l_shipdate > CAST('1995-03-15' AS date)
GROUP BY
    l_orderkey,
    o_orderdate,
    o_shippriority
ORDER BY
    revenue DESC,
    o_orderdate
LIMIT 10
```

After parsing, binding and optimizing, cdb prints the plan with the optimizer's row estimates (`EXPLAIN`, SF1):

```
LIMIT 10  (~10 rows)
  ORDER BY revenue DESC NULLS LAST, o_orderdate ASC NULLS LAST  (~462577 rows)
    PROJECT [l_orderkey, sum((l_extendedprice * (1.0 - l_discount))) AS revenue, o_orderdate, o_shippriority]  (~462577 rows)
      AGGREGATE groups=[l_orderkey, o_orderdate, o_shippriority] aggregates=[sum((l_extendedprice * (1.0 - l_discount)))]  (~462577 rows)
        PROJECT [o_orderdate, o_shippriority, l_orderkey, l_extendedprice, l_discount]  (~462577 rows)
          JOIN INNER ON (l_orderkey = o_orderkey)  (~462577 rows)
            FILTER (l_shipdate > DATE '1995-03-15')  (~3223931 rows)
              SCAN lineitem [l_orderkey, l_extendedprice, l_discount, l_shipdate] prune(l_shipdate > 1995-03-15)  (~6001215 rows)
            JOIN INNER ON (c_custkey = o_custkey)  (~219070 rows)
              FILTER (o_orderdate < DATE '1995-03-15')  (~728803 rows)
                SCAN orders [o_orderkey, o_custkey, o_orderdate, o_shippriority] prune(o_orderdate < 1995-03-15)  (~1500000 rows)
              FILTER (c_mktsegment = 'BUILDING')  (~29982 rows)
                SCAN customer [c_custkey, c_mktsegment] prune(c_mktsegment = BUILDING)  (~150000 rows)
```

`EXPLAIN ANALYZE` runs the query (here 1 thread, warm) and adds the rows that actually came out of every operator, the number of rows each join built its hash table from, the rows each sink consumed and the CPU time of each operator's own calls:

```
LIMIT 10  (est ~10, actual 10 rows, 1.74 ms; in 11620 rows)
  ORDER BY revenue DESC NULLS LAST, o_orderdate ASC NULLS LAST  (sorted as a top-N together with the LIMIT above)
    PROJECT [l_orderkey, sum((l_extendedprice * (1.0 - l_discount))) AS revenue, o_orderdate, o_shippriority]  (est ~462577, actual 11620 rows, 0.00 ms)
      AGGREGATE groups=[l_orderkey, o_orderdate, o_shippriority] aggregates=[sum((l_extendedprice * (1.0 - l_discount)))]  (est ~462577, actual 11620 rows, 3.33 ms; in 30519 rows)
        PROJECT [o_orderdate, o_shippriority, l_orderkey, l_extendedprice, l_discount]  (est ~462577, actual 30519 rows, 0.28 ms)
          JOIN INNER ON (l_orderkey = o_orderkey)  (est ~462577, actual 30519 rows, 37.1 ms; build 147126 rows, 3.95 ms)
            FILTER (l_shipdate > DATE '1995-03-15')  (est ~3223931, actual 3241776 rows, 3.92 ms)
              SCAN lineitem [l_orderkey, l_extendedprice, l_discount, l_shipdate] prune(l_shipdate > 1995-03-15)  (est ~6001215, actual 6001215 rows, 16.1 ms)
            JOIN INNER ON (c_custkey = o_custkey)  (est ~219070, actual 147126 rows, 17.4 ms; build 30142 rows, 0.34 ms)
              FILTER (o_orderdate < DATE '1995-03-15')  (est ~728803, actual 727305 rows, 0.88 ms)
                SCAN orders [o_orderkey, o_custkey, o_orderdate, o_shippriority] prune(o_orderdate < 1995-03-15)  (est ~1500000, actual 1500000 rows, 2.92 ms)
              FILTER (c_mktsegment = 'BUILDING')  (est ~29982, actual 30142 rows, 0.40 ms)
                SCAN customer [c_custkey, c_mktsegment] prune(c_mktsegment = BUILDING)  (est ~150000, actual 150000 rows, 0.16 ms)
Planning: 0.02 ms
Execution: 86.9 ms on 1 thread, 10 rows returned
(operator times are summed over threads)
```
