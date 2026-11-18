# Verification record

## Standalone tests (2026-11-19, `debzero`, Linux x64)

`ctest` runs three suites. All pass in Release mode:

- **`js_mutator_mutator_test`** checks the interface contract: size limits (including `MaxSize` 0–16), determinism, exact-size replay of generated programs, the Tree-sitter check on every valid-leaning output, diversity per parent, repair of invalid parents (never a `;` dead end), malformed mode, and crossover.
- **`js_mutator_generation_test`** checks the end goal. It generates 20,000 programs from empty input and 400 chains growing `let x = 1 + 2;` for 30 generations. Both must produce all 69 tracked constructs. Both must produce the nested shape `function f() { (a = class { static { this; } }) => {} }` (20 times from empty, 5 times in growth). Code generated inside `function total(price, qty)` must reuse its parameters (239 of 372 edits, 64%).
- **`js_mutator_adapter_test`** checks both libFuzzer callbacks, their buffer limits, and null handling.

With Node.js installed, `js_mutator_v8_parse_test` also parses samples with V8.

## V8 parser acceptance

Measured with V8 via Node 26.9 `vm.Script` (compile only), using `tools/sample.cc` output built on `debzero`:

| Samples | Result |
|---|---|
| 10,000 valid-leaning (fresh programs and 8-step chains from 3 seeds) | 9,996 accepted |
| 5,000 malformed | 4,873 rejected (97.5%) |
| 14,978 chain steps, 500 chains of 30 (earlier run) | 1 reject |

The remaining rejects are all one shape, such as `f([7], [a] = 1)`. The specification allows it and JavaScriptCore accepts it, but V8 rejects it. It looks like a V8 parser bug, not a mutator error.

## Lab fuzzer (V8 12.5, `v8_lab_parser_libfuzzer`)

The harness was rebuilt with `src/generator.cc` in the GN source set. Started from a copy of the 376-file corpus at about 9,660 coverage edges, it reached 15,238 edges in about 8 minutes, then stopped on a V8 debug check in scope analysis:

```text
src/ast/scopes.cc:2450 Debug check failed:
!var->has_forced_context_allocation() || var->is_used().
```

The crashing inputs are kept outside the repository.
