#!/usr/bin/env python3
"""Checks tools/cov_summary.py on a small -print_coverage=1 fixture."""
import os
import subprocess
import sys
import tempfile

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools",
                      "cov_summary.py")

COV = """INFO: Seed: 1
COVERAGE:
COVERED_FUNC: hits: 5 edges: 3/10 v8::internal::Parser::ParseFoo(int, char) src/parsing/parser.cc:10
  UNCOVERED_PC: src/parsing/parser.cc:12
UNCOVERED_FUNC: hits: 0 edges: 0/4 v8::internal::Parser::ParseBar() src/parsing/parser.cc:40
COVERED_FUNC: hits: 1 edges: 2/2 bool v8::internal::regexp::Parser::Verify<unsigned short>(a b) src/regexp/regexp-parser.cc:7
COVERED_FUNC: hits: 1 edges: 9/9 v8::internal::Heap::Gc() src/heap/heap.cc:1
"""

BASE = """== per-file edges covered/total
     1/    14   7% src/parsing/parser.cc
"""


def run(*args):
    return subprocess.run([sys.executable, SCRIPT, *args], check=True,
                          capture_output=True, text=True).stdout.splitlines()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        cov = os.path.join(tmp, "cov.txt")
        base = os.path.join(tmp, "base.txt")
        open(cov, "w").write(COV)
        open(base, "w").write(BASE)
        out = run(cov)
        assert out[1:] == ["     3/    14  21% src/parsing/parser.cc",
                           "     2/     2 100% src/regexp/regexp-parser.cc",
                           "     5/    16  31% total"], out
        out = run(cov, base)
        assert out[1].endswith("src/parsing/parser.cc  +2"), out
        assert out[-1].endswith("total  +4"), out
        # A summary is itself accepted as input.
        summary = os.path.join(tmp, "summary.txt")
        open(summary, "w").write("\n".join(run(cov)) + "\n")
        assert run(summary) == run(cov), "summary round trip"
    print("cov_summary_test: ok")


main()
