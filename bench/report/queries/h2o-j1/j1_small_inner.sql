SELECT count(*) AS n, sum(x.v1) AS v1, sum(small.v2) AS v2 FROM x JOIN small ON x.id1 = small.id1
