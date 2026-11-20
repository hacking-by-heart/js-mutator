// Lazy-vs-eager differential target for js-mutator.
//
// Each input is parsed twice with fresh state: once with the default flags
// (lazy functions go through the preparser) and once with lazy compilation
// off (the full parser runs on every function). Both passes must agree on
// success, and on max_info_id when both succeed; a count mismatch is what
// later trips CHECK_EQ(function_literal_id, GetNextInfoId()) in
// Parser::DoParseFunction. Inputs where either pass overflowed the stack
// are skipped, since stack depth differs between the passes.

#include <stddef.h>
#include <stdint.h>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "src/base/logging.h"
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
    jsm::ParseResult lazy = jsm::ParseOnce(i_isolate, source, options);
    options.lazy = false;
    jsm::ParseResult eager = jsm::ParseOnce(i_isolate, source, options);
    if (!lazy.stack_overflow && !eager.stack_overflow) {
      CHECK_EQ(lazy.ok, eager.ok);
      if (lazy.ok) CHECK_EQ(lazy.max_info_id, eager.max_info_id);
    }
  }
  jsm::MaybeCollectGarbage(isolate);
  return 0;
}
