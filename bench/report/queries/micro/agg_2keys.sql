SELECT count(*), sum(sv) FROM (SELECT k1k, d, sum(v) AS sv FROM t GROUP BY k1k, d) q
