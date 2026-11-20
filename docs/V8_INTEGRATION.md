# V8 parser integration

How to build the js-mutator libFuzzer targets for an existing V8 checkout. These steps were run end to end on 2026-11-20 against V8 15.8 (`b601a2347f0`) on Linux x64, using the GN arguments shown below.

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

## 2. The harness targets

The harness sources live in this repository under `harness/`, so nothing is copied into V8:

- `parser.cc` (`v8_js_mutator_parser_fuzzer`): parse only, the fast default target.
- `compile.cc` (`v8_js_mutator_compile_fuzzer`): compiles without running, then compiles every lazy function until nothing new appears (at most 64).
- `diff.cc` (`v8_js_mutator_diff_fuzzer`): parses twice, lazy and eager, and CHECKs that both agree on success and on `max_info_id`.
- `streaming.cc` (`v8_js_mutator_streaming_fuzzer`): streaming compile with small chunks in the UTF8, TWO_BYTE, ONE_BYTE and WINDOWS_1252 encodings, as d8's `--streaming-compile`.

All four accept inputs up to 2048 bytes; unlike V8's `parser.cc`, they do not drop inputs with unbalanced brackets. Non-ASCII input is decoded as UTF-8. A leading `/*M*/` comment selects the module goal; in `parser.cc` and `diff.cc`, `/*R*/` selects REPL mode and `/*E*/` eval. `parser.cc` also enables compile-hints magic comments for half of the inputs (by input hash) and re-parses each regexp literal into a regexp AST. Every target runs a full GC every 64 inputs.

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

foreach(harness, [ "parser", "compile", "diff", "streaming" ]) {
  v8_executable("v8_js_mutator_${harness}_fuzzer") {
    testonly = true
    sources = [
      "test/fuzzer/js_mutator_external/harness/${harness}.cc",
      "test/fuzzer/js_mutator_external/harness/common.h",
      "test/fuzzer/js_mutator_external/harness/parse.h",
    ]
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
}
```

Why each part is needed:

- The mutator catches `std::bad_alloc`, so its source set enables C++ exceptions.
- `libs = [ "stdc++" ]` is required. Clang's libFuzzer runtime uses libstdc++ while V8 builds with libc++, so without it linking fails with undefined `std::__cxx11` symbols.
- The adapter defines `LLVMFuzzerCustomMutator` and `LLVMFuzzerCustomCrossOver`. Each target is its own executable, so each links it once.

## 4. Build

```sh
gn gen out/js-mutator --args='use_libfuzzer=true is_asan=true is_debug=false dcheck_always_on=true is_component_build=true symbol_level=1 v8_enable_fuzztest=false use_sysroot=false'
ninja -C out/js-mutator v8_js_mutator_parser_fuzzer
ninja -C out/js-mutator v8_js_mutator_compile_fuzzer
ninja -C out/js-mutator v8_js_mutator_diff_fuzzer
ninja -C out/js-mutator v8_js_mutator_streaming_fuzzer
```

These are the verified arguments. `dcheck_always_on=true` keeps V8's internal consistency checks active, so they turn into crashes. `use_sysroot=false` builds against the host system libraries instead of Chromium's Debian sysroot; drop it if your checkout has the sysroot installed.

### If linking fails with `libclang_rt.fuzzer.a: No such file or directory`

V8 ships its own Clang but not the libFuzzer runtime. Build that one library from the LLVM commit your Clang was made from, then copy it where the linker looks. Run these from `workspace/`, one line at a time.

Find your Clang's version and commit:

```sh
v8/third_party/llvm-build/Release+Asserts/bin/clang++ --version
```

The first line looks like this:

```text
clang version 24.0.0git (https://chromium.googlesource.com/a/external/github.com/llvm/llvm-project e063be79adf127cce301fe5948273102a33012b6)
```

Note two values: the major version (`24`) and the commit hash at the end (`e063be79…`). Substitute your own values where the commands below show `24` and `e063be79adf127cce301fe5948273102a33012b6`.

Download only libFuzzer's source at that commit:

```sh
git init llvm-fuzzer
cd llvm-fuzzer
git remote add origin https://github.com/llvm/llvm-project.git
git sparse-checkout set --no-cone compiler-rt/lib/fuzzer
git fetch --depth 1 --filter=blob:none origin e063be79adf127cce301fe5948273102a33012b6
git checkout FETCH_HEAD
cd compiler-rt/lib/fuzzer
```

Compile it with V8's Clang and package it:

```sh
../../../../v8/third_party/llvm-build/Release+Asserts/bin/clang++ -O2 -fPIC -std=c++17 -fno-omit-frame-pointer -c Fuzzer*.cpp
ar rcs libclang_rt.fuzzer.a Fuzzer*.o
```

Copy it into V8's Clang, using your major version in the path:

```sh
cp libclang_rt.fuzzer.a ../../../../v8/third_party/llvm-build/Release+Asserts/lib/clang/24/lib/x86_64-unknown-linux-gnu/
```

Go back to `v8/` and build again:

```sh
cd ../../../../v8
ninja -C out/js-mutator v8_js_mutator_parser_fuzzer
```

`gclient sync` can replace V8's Clang with a newer one. If the error returns afterwards, repeat these steps with the new version and commit.

## 5. Fuzz

```sh
mkdir -p ../corpus
printf '%s\n' 'function f(x) { return x + 1; }' > ../corpus/seed.js
out/js-mutator/v8_js_mutator_parser_fuzzer ../corpus -max_len=2048
```

An empty corpus also works; the mutator generates programs from scratch. The corpus directory must be writable, because libFuzzer saves coverage-increasing inputs there. Add `-runs=N` for a bounded run, `-jobs=N -workers=N` for parallel processes, or pass a single file path to replay it. Crashes are written to `crash-*` files in the current directory.

By default about 90% of outputs are valid JavaScript and the rest are deliberately broken, to exercise the parser's error handling. Set `JS_MUTATOR_VALID_PERCENT` (0–100) to change that mix. The JavaScript vocabulary is compiled in, so `-dict` is unnecessary.

### Flag groups

V8 flags go on the same command line as libFuzzer flags. Run one group per process, over one shared corpus, and always reproduce a crash with the flags of the group that found it:

- default: no V8 flags.
- `--no-lazy`: the full parser runs on every function.
- `--max-lazy`: also preparses `(function…)` and `!function` bodies.
- `--js-staging`: staged language features.
- `--stack-size=200`: parser and compiler stack overflows (`Isolate::StackOverflow`). With this ASan build, d8 reports a stack overflow for 200 to 400 nested parentheses or arrays, which `kDeepen` reaches within 2,048 bytes, while ordinary inputs still parse. Calibrate again for a different build.
- `--js-regexp-buffer-boundaries --regexp-possessive-quantifier`: extra regexp syntax for the regexp parser (`\A`, `\z`, `\Z`, and `a*+`).

The compile target always needs `--no-compilation-cache`; without it, re-runs of an input hit the cache and skip the parse:

```sh
out/js-mutator/v8_js_mutator_compile_fuzzer ../corpus -max_len=2048 --no-compilation-cache
```

For the compile target, also run a group with `--no-compilation-cache --stress-lazy`, and one with `--no-compilation-cache --stress-lazy-source-positions`. Do not use `--stress-lazy-compilation`: it simulates stack overflows.

If a flag is misspelled, the target prints `Unrecognized flag` at startup.

### Per-operator yield

The adapter compiled with `JS_MUTATOR_YIELD_LOG` defined writes a yield log; it is a separate binary, so the log needs no runtime switch. In `BUILD.gn`, copy a fuzzer target under a new name and add `defines = [ "JS_MUTATOR_YIELD_LOG" ]`; in CMake the same adapter is `js_mutator_libfuzzer_yield`. Each process appends `sha1 edit outcome callback` lines to `js_mutator_yield.<pid>.log` in its working directory. libFuzzer names corpus files by SHA1, so joining the logs with the corpus shows what each `EditKind` emitted and what libFuzzer kept:

```sh
python3 ../js-mutator/tools/yield.py ../corpus js_mutator_yield.*.log
```

### Coverage summary

Replay a corpus with coverage output, then summarize it per file for `src/parsing`, `src/ast`, `src/regexp`, `src/numbers` and `src/strings`:

```sh
out/js-mutator/v8_js_mutator_parser_fuzzer -runs=0 -print_coverage=1 ../corpus > ../cov.txt 2>&1
python3 ../js-mutator/tools/cov_summary.py ../cov.txt
```

Pass an older `cov.txt`, or an older summary, as a second argument to add a delta column.
