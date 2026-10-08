SELECT count(*) AS n, sum(x.v1) AS v1, sum(big.v2) AS v2 FROM x JOIN big ON x.id3 = big.id3
