SELECT count(*) AS n, sum(v3) AS v3, sum(cnt) AS cnt FROM (SELECT id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS cnt FROM x GROUP BY id1, id2, id3, id4, id5, id6) q
