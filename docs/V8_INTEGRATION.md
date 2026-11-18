# V8 parser integration

This recipe connects `js-mutator` to V8's parser fuzzer. Start with sibling checkouts:

```text
workspace/
  v8/
  js-mutator/
```

V8's internal parser API and GN configuration change between revisions. On 2026-11-19 the source set below, including `src/generator.cc`, was compiled with V8's Clang and Chromium style plugin and fuzzed on the lab checkout (V8 12.5, Linux x64). The plugin requires out-of-line special members for non-trivial structs, which `generator.cc` provides.

## Prepare the sources

From `workspace/`, configure the mutator once to fetch its pinned Tree-sitter sources, then make them visible inside V8's GN source root:

```sh
cmake -S js-mutator -B js-mutator/build
cd v8
ln -s ../../../js-mutator test/fuzzer/js_mutator_external
```

The symlink target is relative to `v8/test/fuzzer/`. Create it only once. GN compiles these sources with V8's Clang/libc++ toolchain; it does not link the CMake-built mutator archives.

## Add the GN targets

Add the following to V8's root `BUILD.gn`. The parser target uses the existing `test/fuzzer/parser.cc`, which supplies `LLVMFuzzerTestOneInput`; the mutator adapter supplies `LLVMFuzzerCustomMutator`.

```gn
source_set("js_mutator_for_fuzzing") {
  testonly = true
  sources = [
    "test/fuzzer/js_mutator_external/adapters/libfuzzer.cc",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_javascript-src/src/parser.c",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_javascript-src/src/scanner.c",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_runtime-src/lib/src/lib.c",
    "test/fuzzer/js_mutator_external/src/generator.cc",
    "test/fuzzer/js_mutator_external/src/mutator.cc",
  ]
  include_dirs = [
    "test/fuzzer/js_mutator_external/include",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_runtime-src/lib/include",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_runtime-src/lib/src",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_javascript-src/bindings/c",
    "test/fuzzer/js_mutator_external/build/_deps/tree_sitter_javascript-src/src",
  ]
  configs -= [
    "//build/config/compiler:chromium_code",
    "//build/config/compiler:no_exceptions",
  ]
  configs += [
    "//build/config/compiler:no_chromium_code",
    "//build/config/compiler:exceptions",
  ]
}

config("js_mutator_fuzzer_link") {
  ldflags = [ "-fsanitize=fuzzer" ]
}

v8_executable("v8_js_mutator_parser_fuzzer") {
  testonly = true
  sources = [ "test/fuzzer/parser.cc" ]
  deps = [
    ":fuzzer_support",
    ":js_mutator_for_fuzzing",
  ]
  configs = [
    ":external_config",
    ":internal_config_base",
    ":js_mutator_fuzzer_link",
  ]
}
```

The mutator core catches `std::bad_alloc`, so its source set enables C++ exceptions. Link exactly one `LLVMFuzzerCustomMutator`. V8's `v8_simple_parser_fuzzer` uses a one-input replay driver and does not run a libFuzzer loop.

## Build and fuzz

Configure GN for the V8 revision and its matching Clang/libFuzzer runtime. For a Linux checkout that supports these arguments, a starting configuration is:

```sh
gn gen out/js-mutator --args='use_libfuzzer=true is_asan=true is_debug=false dcheck_always_on=true v8_enable_fuzztest=false'
ninja -C out/js-mutator v8_js_mutator_parser_fuzzer
mkdir -p ../corpus
printf '%s\n' 'function f(x) { return x + 1; }' > ../corpus/seed.js
JS_MUTATOR_VALID_PERCENT=100 out/js-mutator/v8_js_mutator_parser_fuzzer ../corpus -max_len=2048
```

The corpus directory must be writable: libFuzzer saves inputs that add coverage there. Omit `-runs` for continuous fuzzing, or add `-runs=100` for a bounded check. To replay a saved input, pass its file path instead of the corpus directory. If linking reports a missing `libclang_rt.fuzzer.a`, provide compiler-rt matching the Clang used by V8; copying an unrelated runtime can cause ABI or libc link errors.

The existing `parser.cc` rejects inputs longer than 2,048 bytes and filters unbalanced brackets and nonprintable bytes. The command above uses only valid-leaning edits to avoid wasting malformed-mode attempts. To fuzz malformed inputs, copy `parser.cc` to a new harness file, replace its filter with this function, and point the executable's `sources` at the copy:

```cpp
bool IsValidInput(const uint8_t*, size_t size) { return size <= 2048; }
```

The adapter defaults to 90% valid-leaning edits. Set `JS_MUTATOR_VALID_PERCENT` to an integer from 0 to 100 before launch to change the mix.

The adapter also exports `LLVMFuzzerCustomCrossOver`, which libFuzzer uses in place of its byte-level crossover. The JavaScript vocabulary is compiled in, so `-dict` is unnecessary. Use `-max_len=2048` (or `JS_MUTATOR_MAX_LEN=2048`) with a harness that skips longer inputs.

To compare against libFuzzer's built-in mutations with the same binary and equal time, run two copies of the corpus:

```sh
JS_MUTATOR_DEFAULT_PERCENT=0   out/x64.lab-libfuzzer/v8_lab_parser_libfuzzer mut -max_total_time=600 -max_len=2048
JS_MUTATOR_DEFAULT_PERCENT=100 out/x64.lab-libfuzzer/v8_lab_parser_libfuzzer def -max_total_time=600 -max_len=2048 -cross_over=0
```

`-cross_over=0` keeps the baseline free of js-mutator's crossover.
