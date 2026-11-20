// Parse-only target for js-mutator, based on V8's test/fuzzer/parser.cc.
//
// Differences from V8's parser.cc:
// - Only the size check; no bracket-balance filter.
// - Non-ASCII input is decoded as UTF-8 (two-byte sources).
// - A leading /*M*/, /*R*/ or /*E*/ selects the module, REPL or eval goal.
// - One input hash bit enables compile-hints magic comments.
// - After a successful parse, each regexp literal is re-parsed in BuildAST
//   mode.
// - A full GC runs every kGcEvery inputs instead of after every input.

#include <stddef.h>
#include <stdint.h>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "test/fuzzer/fuzzer-support.h"
#include "common.h"
#include "parse.h"

namespace jsm = js_mutator_harness;

V8_SYMBOL_USED extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  v8_fuzzer::FuzzerSupport::InitializeFuzzerSupport(argc, argv);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > jsm::kMaxInputSize) return 0;

  v8_fuzzer::FuzzerSupport* support = v8_fuzzer::FuzzerSupport::Get();
  v8::Isolate* isolate = support->GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(support->GetContext());
  v8::TryCatch try_catch(isolate);
  v8::internal::Isolate* i_isolate =
      reinterpret_cast<v8::internal::Isolate*>(isolate);

  v8::internal::DirectHandle<v8::internal::String> source;
  if (jsm::NewSource(i_isolate, data, size).ToHandle(&source)) {
    jsm::ParseOptions options;
    options.goal = jsm::GoalOf(data, size);
    options.compile_hints = (jsm::InputHash(data, size) & 1) != 0;
    options.reparse_regexps = true;
    jsm::ParseOnce(i_isolate, source, options);
  }
  jsm::MaybeCollectGarbage(isolate);
  return 0;
}
