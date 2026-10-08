#!/usr/bin/env python3
"""Adapts the Polars TPC-H queries of pola-rs/polars-benchmark to the harness.

    gh api repos/pola-rs/polars-benchmark/contents/queries/polars/q1.py --jq .content | base64 -d > DIR/q1.py   # x22
    python3 bench/report/make_polars_queries.py DIR      # writes bench/report/queries/polars/qNN.py

Source: https://github.com/pola-rs/polars-benchmark, Apache License 2.0, commit below. The queries are
used as they are, with four mechanical changes that keep the work done identical:
  * `utils` (where the tables come from) is the harness's in-memory tables (_polars_utils.py);
  * `.round(2)` is removed - it only formats the result, and the answers are compared with the
    exact answer files (a rounded intermediate, Q11's threshold, would also change the result);
  * Q11's threshold fraction is the constant of the SQL text shared by all engines (0.0001, the SF1
    value) instead of 0.0001 / scale_factor, so that every scale factor asks the same question;
  * a `query(tables)` entry point is appended.
"""
import os
import re
import sys

SHA = "401908a307f133a2dffbe3f5e0e0fb2b50ea310b"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "queries", "polars")
HEADER = (f"# TPC-H query of pola-rs/polars-benchmark (Apache License 2.0, https://github.com/pola-rs/polars-benchmark,\n"
          f"# queries/polars/q{{n}}.py at commit {SHA}), adapted mechanically for the harness by\n"
          "# bench/report/make_polars_queries.py: in-memory tables, no .round(2), Q11's constant fraction,\n"
          "# and the query(tables) entry point at the end.\n")


def adapt(n, s):
    assert s.count("from queries.polars import utils") == 1
    s = s.replace("from queries.polars import utils", "import _polars_utils as utils")
    s = s.replace("from settings import Settings\n", "").replace("settings = Settings()\n", "")
    s = s.replace("0.0001 / settings.scale_factor", "0.0001")
    s = re.sub(r"\.round\(2\)", "", s)
    assert "settings" not in s, n
    return HEADER.format(n=n) + s.rstrip("\n") + "\n\n\ndef query(tables):\n    utils.TABLES = tables\n    return q()\n"


def main():
    src = sys.argv[1]
    for i in range(1, 23):
        s = open(os.path.join(src, f"q{i}.py")).read()
        open(os.path.join(OUT, f"q{i:02d}.py"), "w").write(adapt(i, s))
    print("wrote 22 queries to", OUT)


if __name__ == "__main__":
    main()
