# Verification record

## Standalone tests (2026-11-20, `debzero`, Linux x64)

`ctest` runs five suites (about 47 s). All pass in Release mode:

- **`js_mutator_mutator_test`** checks the interface contract: size limits (including `MaxSize` 0–16), determinism, exact-size replay of generated programs, the Tree-sitter check on every valid-leaning output, diversity per parent, repair of invalid parents (never a `;` dead end), malformed mode, and crossover.
- **`js_mutator_generation_test`** checks the end goal. It generates 20,000 programs from empty input and 400 chains growing `let x = 1 + 2;` for 30 generations. Both must produce all 69 tracked constructs. Both must produce the nested shape `function f() { (a = class { static { this; } }) => {} }` (20 times from empty, 5 times in growth). Code generated inside `function total(price, qty)` must reuse its parameters (239 of 372 edits, 64%).
- **`js_mutator_adapter_test`** checks both libFuzzer callbacks, their buffer limits, and null handling.
- **`js_mutator_yield_test`** runs the yield-log adapter and `tools/yield.py`.
- **`js_mutator_cov_summary_test`** checks `tools/cov_summary.py`.

With Node.js installed, `js_mutator_v8_parse_test` also parses samples with V8.

## V8 15.8 acceptance (d8)

`js_mutator_sample 1500`, each source compiled with `d8 --no-lazy --compile-only` (`--module` for `/*M*/`), on 2026-11-20:

| Samples | Result |
|---|---|
| 3,000 valid-leaning | 3,000 accepted |
| 1,500 malformed | 1,475 rejected (98.3%) |
| d8 crashes | 0 |

V8 15.8 accepts `f([7], [a] = 1)`. Only the older V8 in Node rejects it (see below).

## V8 harness targets (V8 15.8, `out/js-mutator`)

Smoke run of each target from the 1,968-file lab corpus with `-max_len=2048`: about 500-760 mutated inputs after loading the corpus, no crashes, leaks or timeouts.

| Target | Edges after load | After smoke run |
|---|---|---|
| `v8_js_mutator_parser_fuzzer` | 20,948 | 22,223 |
| `v8_js_mutator_compile_fuzzer` | 30,177 | 30,853 |
| `v8_js_mutator_diff_fuzzer` | 19,125 | 20,011 |
| `v8_js_mutator_streaming_fuzzer` | 30,615 | 31,208 |

### Coverage, parser target

Edges per file from `tools/cov_summary.py` on `-runs=0 -print_coverage=1` replays (parsing, ast, regexp, numbers and strings sources). "Old" is the old mutator's harness on the same 1,968-file corpus (19,453 libFuzzer edges). "Replay" is that corpus through `v8_js_mutator_parser_fuzzer` (20,712 edges). "15 min" is the corpus after `-max_total_time=900 -fork=8 -max_len=2048` from a fresh copy (760k runs, 8,390 files, 27,056 edges on replay, no crashes).

| File | Edges | Old | Replay | 15 min |
|---|---:|---:|---:|---:|
| src/parsing/parser-base.h | 7520 | 3622 | 3624 | 4277 |
| src/regexp/regexp-parser.cc | 5627 | 351 | 815 | 2803 |
| src/ast/scopes.cc | 3284 | 802 | 802 | 993 |
| src/ast/ast-traversal-visitor.h | 2197 | 263 | 529 | 639 |
| src/parsing/parser.cc | 2163 | 589 | 591 | 868 |
| src/numbers/conversions.cc | 1848 | 149 | 149 | 249 |
| src/parsing/scanner.cc | 1408 | 499 | 501 | 752 |
| src/regexp/regexp-compiler-tonode.cc | 1261 | 0 | 94 | 111 |
| src/parsing/preparse-data.cc | 718 | 118 | 118 | 119 |
| src/parsing/expression-scope.h | 602 | 290 | 290 | 363 |
| src/ast/modules.cc | 480 | 0 | 0 | 112 |
| src/strings/unicode-decoder.cc | 421 | 14 | 14 | 60 |
| src/regexp/regexp-ast.cc | 246 | 2 | 44 | 64 |
| **total** | 58265 | 7988 (13%) | 8905 (15%) | 13027 (22%) |

The parser target does not compile lazy functions, so the preparse-data consumer stays at the old level; `v8_js_mutator_compile_fuzzer` is the target for it.

## Older V8 parser acceptance (Node)

Measured earlier with V8 via Node 26.9 `vm.Script` (compile only), using `tools/sample.cc` output built on `debzero`:

| Samples | Result |
|---|---|
| 10,000 valid-leaning (fresh programs and 8-step chains from 3 seeds) | 9,996 accepted |
| 5,000 malformed | 4,873 rejected (97.5%) |
| 14,978 chain steps, 500 chains of 30 (earlier run) | 1 reject |

The remaining rejects are all one shape, such as `f([7], [a] = 1)`. The specification allows it, and JavaScriptCore and V8 15.8 accept it. Only the older V8 in Node rejects it.

## Lab fuzzer (V8 12.5, `v8_lab_parser_libfuzzer`)

The harness was rebuilt with `src/generator.cc` in the GN source set. Started from a copy of the 376-file corpus at about 9,660 coverage edges, it reached 15,238 edges in about 8 minutes, then stopped on a V8 debug check in scope analysis:

```text
src/ast/scopes.cc:2450 Debug check failed:
!var->has_forced_context_allocation() || var->is_used().
```

The crashing inputs are kept outside the repository.
