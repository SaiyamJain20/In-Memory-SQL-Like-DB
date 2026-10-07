SELECT count(*) AS n, sum(x.v1) AS v1, sum(medium.v2) AS v2 FROM x LEFT JOIN medium ON x.id2 = medium.id2
