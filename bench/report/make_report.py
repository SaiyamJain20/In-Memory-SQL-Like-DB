#!/usr/bin/env python3
"""Assembles docs/REPORT.md from the chapter files in docs/report/src/ and the generated tables,
charts and numbers of bench/report/analyze.py. Placeholders:

    {{n:key}}              a number of docs/report/numbers.json (display text)
    {{table:name}}         docs/report/tables/name.md
    {{img:name|caption}}   the chart docs/report/name.svg
    {{include:name}}       docs/report/generated/name.md

An unresolved placeholder is an error: nothing in the report is a number nobody computed.
"""
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOC = os.path.join(ROOT, "docs")
REP = os.path.join(DOC, "report")


def main():
    text_numbers = json.load(open(os.path.join(REP, "numbers.json")))["text"]
    parts = [open(p).read().rstrip("\n") for p in sorted(glob.glob(os.path.join(REP, "src", "*.md")))]
    doc = "\n\n".join(parts) + "\n"
    missing = []

    def sub(m):
        kind, arg = m.group(1), m.group(2)
        if kind == "n":
            if arg not in text_numbers:
                missing.append(f"n:{arg}")
                return m.group(0)
            return str(text_numbers[arg])
        if kind == "table":
            path = os.path.join(REP, "tables", f"{arg}.md")
        elif kind == "include":
            path = os.path.join(REP, "generated", f"{arg}.md")
        elif kind == "img":
            name, _, caption = arg.partition("|")
            if not os.path.exists(os.path.join(REP, f"{name}.svg")):
                missing.append(f"img:{name}")
                return m.group(0)
            return f"![{caption or name}](report/{name}.svg)"
        else:
            missing.append(m.group(0))
            return m.group(0)
        if not os.path.exists(path):
            missing.append(f"{kind}:{arg}")
            return m.group(0)
        return open(path).read().rstrip("\n")

    doc = re.sub(r"\{\{(\w+):([^}]+)\}\}", sub, doc)
    open(os.path.join(DOC, "REPORT.md"), "w").write(doc)
    if missing:
        print("unresolved placeholders:", ", ".join(sorted(set(missing))))
        sys.exit(1)
    print(f"docs/REPORT.md: {len(doc.splitlines())} lines")


if __name__ == "__main__":
    main()
