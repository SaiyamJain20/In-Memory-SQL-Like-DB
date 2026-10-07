SELECT count(*), sum(sv), sum(c) FROM (SELECT k1m, sum(v) AS sv, count(*) AS c FROM t GROUP BY k1m) q
