# js-mutator

A libFuzzer custom mutator for JavaScript parsers. Its JavaScript tokens and grammar rules are compiled in. It generates new programs from empty input and transforms corpus seeds, using the seed's own in-scope variables and expressions, into more varied and deeply nested JavaScript. Output is valid by default, with an optional mode that breaks it deliberately.

- [Requirements](docs/REQUIREMENTS.md) · [Design](docs/DESIGN.md) · [Verification](docs/TESTING.md) · [V8 integration](docs/V8_INTEGRATION.md)

## Build and test

Requires CMake 3.18+, a C/C++17 compiler, and Git (configuration downloads pinned Tree-sitter sources).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
(cd build && ctest --output-on-failure)
```

CTest runs the interface contract, the end-goal generation checks, and the adapter test. If Node.js is installed, it also checks acceptance with V8's own parser (`js_mutator_v8_parse_test`).

## libFuzzer adapter

`adapters/libfuzzer.cc` exports `LLVMFuzzerCustomMutator` and `LLVMFuzzerCustomCrossOver`. Environment variables, read once at startup:

| Variable | Default | Effect |
|---|---|---|
| `JS_MUTATOR_VALID_PERCENT` | 90 | Share of calls in valid-leaning mode; the rest are deliberately malformed |
| `JS_MUTATOR_MAX_LEN` | 0 (off) | Output cap below libFuzzer's `max_len`, e.g. 2048 for a harness that skips longer inputs |
| `JS_MUTATOR_DEFAULT_PERCENT` | 0 | Share of calls given to libFuzzer's built-in byte mutator (Linux/ELF only). 100 gives an A/B baseline from the same binary; also pass `-cross_over=0` |
