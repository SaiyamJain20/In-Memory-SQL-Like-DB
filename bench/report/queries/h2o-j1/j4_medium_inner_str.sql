SELECT count(*) AS n, sum(x.v1) AS v1, sum(medium.v2) AS v2 FROM x JOIN medium ON x.id5 = medium.id5
