# js-mutator

A standalone JavaScript source mutator for different fuzzing harnesses. It provides a C++ core and a libFuzzer custom-mutator adapter.

**Status:** first implementation. The core makes bounded, seed-based source edits and uses Tree-sitter to check valid-leaning candidates. A V8 parser smoke test succeeded; target-parser acceptance rates and comparative coverage gains remain unmeasured.

- [Requirements](docs/REQUIREMENTS.md)
- [Design proposal](docs/DESIGN.md)
- [Verification record](docs/TESTING.md)
- [V8 parser integration](docs/V8_INTEGRATION.md)

## Build and test

Requires CMake 3.18 or newer, a C and C++17 compiler, and Git. Configuration downloads pinned Tree-sitter sources.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
(cd build && ctest --output-on-failure)
```

The `js_mutator` static library has no libFuzzer dependency. The optional `js_mutator_libfuzzer` object target exports `LLVMFuzzerCustomMutator` for a harness that supplies `LLVMFuzzerTestOneInput`. The adapter defaults to 90% valid-leaning edits; set `JS_MUTATOR_VALID_PERCENT` to an integer from 0 to 100 to change the mix.
