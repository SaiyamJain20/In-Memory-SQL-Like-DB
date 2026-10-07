SELECT count(*), sum(sv), sum(c) FROM (SELECT k100k, sum(v) AS sv, count(*) AS c FROM t GROUP BY k100k) q
