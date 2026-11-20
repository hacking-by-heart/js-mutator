#!/usr/bin/env python3
"""Per-operator yield: what each EditKind emitted and what libFuzzer kept.

Usage: yield.py CORPUS_DIR LOG [LOG...]

LOG files come from a fuzzer linked with the yield-log adapter
(js_mutator_libfuzzer_yield): one "sha1 edit outcome callback" line per unit,
in js_mutator_yield.<pid>.log. libFuzzer names corpus files by SHA1, so a
unit was kept exactly when its hash names a file in CORPUS_DIR.
"""
import collections
import os
import sys

# Same order as js_mutator::EditKind and js_mutator::Outcome.
EDITS = ["None", "Literal", "BinaryOperator", "WrapExpression", "GenerateStatement",
         "GenerateExpression", "WrapStatement", "DeleteStatement", "DuplicateStatement",
         "SwapStatements", "ReuseSubtree", "RepairSyntax", "CrossOver", "GenerateProgram",
         "DisruptSyntax", "InjectEarlyError", "Laziness", "Capture", "Rebind",
         "MagicComment", "RegexMalformed", "ParamScope", "Unwrap", "DeleteRange", "Deepen",
         "PrivateRename", "FlipGoal"]
OUTCOMES = ["valid", "malformed", "fallback", "generated"]


def name(table, index):
    index = int(index)
    return table[index] if 0 <= index < len(table) else str(index)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    kept = set(os.listdir(argv[1]))
    emitted = collections.Counter()
    hashes = collections.defaultdict(set)
    outcomes = collections.Counter()
    for path in argv[2:]:
        with open(path) as log:
            for line in log:
                fields = line.split()
                if len(fields) != 4:
                    continue
                sha1, edit, outcome, callback = fields
                key = (callback, name(EDITS, edit))
                emitted[key] += 1
                hashes[key].add(sha1)
                outcomes[name(OUTCOMES, outcome)] += 1
    print(f"{'callback':10} {'edit':20} {'emitted':>9} {'distinct':>9} {'kept':>6} {'kept/1k':>8}")
    for key in sorted(emitted, key=lambda k: -emitted[k]):
        n_kept = len(hashes[key] & kept)
        print(f"{key[0]:10} {key[1]:20} {emitted[key]:9} {len(hashes[key]):9} {n_kept:6} "
              f"{1000.0 * n_kept / emitted[key]:8.2f}")
    total = sum(outcomes.values())
    print("outcomes: " + ", ".join(f"{k} {v} ({100.0 * v / total:.1f}%)"
                                   for k, v in outcomes.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
