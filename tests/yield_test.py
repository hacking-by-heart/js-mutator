#!/usr/bin/env python3
"""Checks the yield-log adapter and tools/yield.py.

Usage: yield_test.py ADAPTER_YIELD_TEST_BINARY
"""
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "yield.py")


def main():
    with tempfile.TemporaryDirectory() as work:
        subprocess.run([sys.argv[1]], cwd=work, check=True, capture_output=True)
        logs = glob.glob(os.path.join(work, "js_mutator_yield.*.log"))
        assert len(logs) == 1, logs
        lines = [line.split() for line in open(logs[0])]
        assert len(lines) >= 400 and all(len(f) == 4 for f in lines), len(lines)
        logged = {f[0] for f in lines if f[3] == "mutate"}
        units = glob.glob(os.path.join(work, "unit.*"))
        assert len(units) == 256, len(units)
        hashes = [hashlib.sha1(open(u, "rb").read()).hexdigest() for u in units]
        assert all(h in logged for h in hashes), "a unit hash is missing from the log"
        # A corpus holding 10 of the units: yield.py reports 10 kept.
        corpus = os.path.join(work, "corpus")
        os.mkdir(corpus)
        for h in sorted(set(hashes))[:10]:
            open(os.path.join(corpus, h), "wb").close()
        out = subprocess.run([sys.executable, SCRIPT, corpus, logs[0]], check=True,
                             capture_output=True, text=True).stdout.splitlines()
        kept = sum(int(line.split()[4]) for line in out[1:] if line.startswith("mutate"))
        assert kept == 10, out
        assert any("crossover" in line for line in out), out
    print("yield: ok")


if __name__ == "__main__":
    main()
