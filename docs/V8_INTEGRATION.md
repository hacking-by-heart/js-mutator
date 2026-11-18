# V8 parser integration

How to build a libFuzzer parser fuzzer for an existing V8 checkout with js-mutator. These steps were run end to end on 2026-11-19 against V8 12.5 (`68cfd6923d2`) on Linux x64, using the GN arguments shown below.

## 1. Fetch the mutator sources

Place the checkouts side by side:

```text
workspace/
  v8/          existing checkout (depot_tools on PATH)
  js-mutator/
```

From `workspace/`, configure js-mutator once. This only downloads the pinned Tree-sitter sources into `js-mutator/build/_deps`; GN compiles them, so nothing from this build is linked. Then link js-mutator into V8's source tree:

```sh
cmake -S js-mutator -B js-mutator/build
cd v8
ln -s ../../../js-mutator test/fuzzer/js_mutator_external
```

The symlink target is relative to `v8/test/fuzzer/`.

## 2. Create the harness

V8's `test/fuzzer/parser.cc` drops any input whose brackets are unbalanced as raw characters, including brackets inside strings, regular expressions and comments. That discards about 60% of valid generated programs. Copy it with only the size check:

```sh
sed 's/^  if (!IsValidInput(data, size)) {$/  if (size > 2048) {/' \
  test/fuzzer/parser.cc > test/fuzzer/js_mutator_parser.cc
grep -n 'size > 2048) {' test/fuzzer/js_mutator_parser.cc
```

The `grep` must print a line; if it does not, your revision's `parser.cc` differs and you should make the same change by hand.

## 3. Add the GN targets

Append to V8's root `BUILD.gn`:

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
  sources = [ "test/fuzzer/js_mutator_parser.cc" ]
  deps = [
    ":fuzzer_support",
    ":js_mutator_for_fuzzing",
  ]
  configs = [
    ":external_config",
    ":internal_config_base",
    ":js_mutator_fuzzer_link",
  ]
  libs = [ "stdc++" ]
}
```

Why each part is needed:

- The mutator catches `std::bad_alloc`, so its source set enables C++ exceptions.
- `libs = [ "stdc++" ]` is required. Clang's libFuzzer runtime uses libstdc++ while V8 builds with libc++, so without it linking fails with undefined `std::__cxx11` symbols.
- The adapter defines `LLVMFuzzerCustomMutator` and `LLVMFuzzerCustomCrossOver`. Link it into exactly one fuzzer.

## 4. Build

```sh
gn gen out/js-mutator --args='use_libfuzzer=true is_asan=true is_debug=false dcheck_always_on=true is_component_build=true symbol_level=1 v8_enable_fuzztest=false use_sysroot=false'
ninja -C out/js-mutator v8_js_mutator_parser_fuzzer
```

These are the verified arguments. `dcheck_always_on=true` keeps V8's internal consistency checks active, so they turn into crashes. `use_sysroot=false` builds against the host system libraries instead of Chromium's Debian sysroot; drop it if your checkout has the sysroot installed.

## 5. Fuzz

```sh
mkdir -p ../corpus
printf '%s\n' 'function f(x) { return x + 1; }' > ../corpus/seed.js
out/js-mutator/v8_js_mutator_parser_fuzzer ../corpus -max_len=2048
```

An empty corpus also works; the mutator generates programs from scratch. The corpus directory must be writable, because libFuzzer saves coverage-increasing inputs there. Add `-runs=N` for a bounded run, `-jobs=N -workers=N` for parallel processes, or pass a single file path to replay it. Crashes are written to `crash-*` files in the current directory.

Settings are environment variables read at startup:

| Variable | Default | Effect |
|---|---|---|
| `JS_MUTATOR_VALID_PERCENT` | 90 | Share of valid-leaning edits; the rest are deliberately malformed |
| `JS_MUTATOR_MAX_LEN` | 0 (off) | Extra output cap; `-max_len=2048` already matches the harness |
| `JS_MUTATOR_DEFAULT_PERCENT` | 0 | Share of calls handed to libFuzzer's built-in mutator |

The JavaScript vocabulary is compiled in, so `-dict` is unnecessary.

## Comparing against libFuzzer's built-in mutator

Run the same binary for equal time on two copies of the corpus:

```sh
cp -r ../corpus ../corpus-mut && cp -r ../corpus ../corpus-def
JS_MUTATOR_DEFAULT_PERCENT=0   out/js-mutator/v8_js_mutator_parser_fuzzer ../corpus-mut -max_total_time=600 -max_len=2048
JS_MUTATOR_DEFAULT_PERCENT=100 out/js-mutator/v8_js_mutator_parser_fuzzer ../corpus-def -max_total_time=600 -max_len=2048 -cross_over=0
```

`-cross_over=0` keeps js-mutator's crossover out of the baseline. Compare the final `cov:` values. Delegating to the built-in mutator works on Linux (ELF) builds only.
