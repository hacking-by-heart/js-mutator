#!/usr/bin/env python3
"""Per-file edge coverage of V8's parser-related sources.

Usage: cov_summary.py COV [BASE]

COV is the output of a libFuzzer replay with -print_coverage=1, e.g.
  v8_js_mutator_parser_fuzzer -runs=0 -print_coverage=1 CORPUS > cov.txt 2>&1
It prints covered/total edges per file under src/parsing, src/ast,
src/regexp, src/numbers and src/strings, largest files first, and a total.
BASE, if given, is another such output or a summary printed by this script;
a delta column is then added.
"""
import collections
import re
import sys

DIRS = ("src/parsing/", "src/ast/", "src/regexp/", "src/numbers/",
        "src/strings/")
FUNC = re.compile(r"^(?:UN)?COVERED_FUNC: hits: \d+ edges: (\d+)/(\d+) .* (\S+):\d+$")
SUMMARY = re.compile(r"^\s*(\d+)/\s*(\d+)\s+\d+%\s+(\S+)$")


def load(path):
    files = collections.defaultdict(lambda: [0, 0])
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = FUNC.match(line) or SUMMARY.match(line)
            if not m or not m.group(3).startswith(DIRS):
                continue
            entry = files[m.group(3)]
            entry[0] += int(m.group(1))
            entry[1] += int(m.group(2))
    return files


def pct(covered, total):
    return 100 * covered // total if total else 0


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    cov = load(sys.argv[1])
    base = load(sys.argv[2]) if len(sys.argv) == 3 else None
    print("== per-file edges covered/total" + ("  (delta vs base)" if base else ""))
    covered = total = base_covered = 0
    for name in sorted(cov, key=lambda n: (-cov[n][1], n)):
        c, t = cov[name]
        covered += c
        total += t
        line = f"{c:6}/{t:6} {pct(c, t):3}% {name}"
        if base is not None:
            b = base.get(name, [0, 0])[0]
            base_covered += b
            line += f"  {c - b:+d}"
        print(line)
    line = f"{covered:6}/{total:6} {pct(covered, total):3}% total"
    if base is not None:
        line += f"  {covered - base_covered:+d}"
    print(line)


main()
