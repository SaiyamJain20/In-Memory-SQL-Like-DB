SELECT k1k, sum(v), count(*), avg(v) FROM t GROUP BY k1k ORDER BY k1k LIMIT 10
