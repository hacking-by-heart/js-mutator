# js-mutator

A libFuzzer custom mutator for JavaScript parsers. Its JavaScript tokens and grammar rules are compiled in. It generates new programs from empty input and transforms corpus seeds, using the seed's own in-scope variables and expressions, into more varied and deeply nested JavaScript. Output is valid by default, with an optional mode that breaks it deliberately.

- [Requirements](docs/REQUIREMENTS.md) · [Design](docs/DESIGN.md) · [Verification](docs/TESTING.md) · [V8 integration](docs/V8_INTEGRATION.md) · [V8 harnesses](docs/HARNESSES.md)

## Build and test

Requires CMake 3.18+, a C/C++17 compiler, and Git (configuration downloads pinned Tree-sitter sources).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
(cd build && ctest --output-on-failure)
```

CTest runs the interface contract, the end-goal generation checks, and the adapter test. If Node.js is installed, it also checks acceptance with V8's own parser (`js_mutator_v8_parse_test`).

## libFuzzer adapter

`adapters/libfuzzer.cc` exports `LLVMFuzzerCustomMutator` and `LLVMFuzzerCustomCrossOver`. By default about 90% of outputs are valid JavaScript and the rest are deliberately broken; set `JS_MUTATOR_VALID_PERCENT` (0–100) to change that mix.
