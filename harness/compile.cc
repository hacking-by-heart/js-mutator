// Lazy-compile target for js-mutator.
//
// Compiles the input as a script (or as a module with a leading /*M*/)
// without running it, then compiles every lazy SharedFunctionInfo of the
// script, repeating until a pass compiles nothing new or kMaxLazyCompiles
// compiles were tried. Compiling an outer function creates its inner SFIs,
// so the repetition reaches preparse-data restoration, scope-chain
// deserialization and class member initializers.
//
// The compilation cache is off (set before V8 starts): otherwise libFuzzer's
// re-runs of an input hit the cache and skip the parse.
//
// Oracle, log only: a lazy compile that fails with anything but a stack
// overflow, after the script itself compiled, means the preparser accepted a
// function the full parser rejects. It is printed to stderr, not a crash,
// until the cases that are expected (errors the preparser cannot see) are
// triaged.

#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>

#include <memory>
#include <vector>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"
#include "src/api/api-inl.h"
#include "src/codegen/compiler.h"
#include "src/execution/isolate-inl.h"
#include "src/flags/flags.h"
#include "src/objects/shared-function-info-inl.h"
#include "test/fuzzer/fuzzer-support.h"
#include "common.h"

namespace i = v8::internal;
namespace jsm = js_mutator_harness;

namespace {

constexpr int kMaxLazyCompiles = 64;

v8::MaybeLocal<v8::String> NewSourceString(v8::Isolate* isolate,
                                           const uint8_t* data, size_t size) {
  int length = static_cast<int>(size);
  if (jsm::IsAscii(data, size)) {
    return v8::String::NewFromOneByte(isolate, data,
                                      v8::NewStringType::kNormal, length);
  }
  return v8::String::NewFromUtf8(isolate, reinterpret_cast<const char*>(data),
                                 v8::NewStringType::kNormal, length);
}

bool Contains(const std::vector<i::Handle<i::SharedFunctionInfo>>& list,
              i::Tagged<i::SharedFunctionInfo> sfi) {
  for (const auto& h : list) {
    if (*h == sfi) return true;
  }
  return false;
}

// Clears the exception of a failed lazy compile and logs it unless it is a
// stack overflow.
void ReportLazyFailure(i::Isolate* isolate) {
  if (!isolate->has_exception()) return;
  i::DirectHandle<i::Object> exception(isolate->exception(), isolate);
  isolate->clear_exception();
  std::unique_ptr<char[]> text =
      i::Object::NoSideEffectsToString(isolate, exception)->ToCString();
  if (strstr(text.get(), "Maximum call stack size exceeded") == nullptr) {
    fprintf(stderr, "js-mutator: lazy compile failed after the script compiled: %s\n",
            text.get());
  }
}

void CompileLazyFunctions(i::Isolate* isolate, i::Handle<i::Script> script) {
  i::HandleScope scope(isolate);
  std::vector<i::Handle<i::SharedFunctionInfo>> tried;
  bool progress = true;
  while (progress && tried.size() < kMaxLazyCompiles) {
    progress = false;
    // Collect first: the iterator must not stay open across a compile.
    std::vector<i::Handle<i::SharedFunctionInfo>> todo;
    i::SharedFunctionInfo::ScriptIterator it(isolate, *script);
    for (i::Tagged<i::SharedFunctionInfo> sfi = it.Next(); !sfi.is_null();
         sfi = it.Next()) {
      if (!sfi->is_compiled() && !Contains(tried, sfi)) {
        todo.push_back(i::handle(sfi, isolate));
      }
    }
    for (const auto& sfi : todo) {
      if (tried.size() >= kMaxLazyCompiles) break;
      if (sfi->is_compiled()) continue;
      tried.push_back(sfi);
      i::IsCompiledScope is_compiled_scope;
      if (i::Compiler::Compile(isolate, sfi, i::Compiler::KEEP_EXCEPTION,
                               &is_compiled_scope)) {
        progress = true;
      } else {
        ReportLazyFailure(isolate);
      }
    }
  }
}

}  // namespace

V8_SYMBOL_USED extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  i::v8_flags.compilation_cache = false;
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
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);

  v8::Local<v8::String> source_string;
  if (NewSourceString(isolate, data, size).ToLocal(&source_string)) {
    bool is_module = jsm::GoalOf(data, size) == jsm::Goal::kModule;
    v8::ScriptOrigin origin(
        v8::String::NewFromUtf8Literal(isolate, "fuzz.js"), 0, 0, false, -1,
        v8::Local<v8::Value>(), false, false, is_module);
    v8::ScriptCompiler::Source source(source_string, origin);
    i::Handle<i::Script> script;
    if (is_module) {
      v8::Local<v8::Module> module;
      if (v8::ScriptCompiler::CompileModule(isolate, &source)
              .ToLocal(&module)) {
        auto sfi = v8::Utils::OpenHandle(*module->GetUnboundModuleScript());
        script = i::handle(i::Cast<i::Script>(sfi->script()), i_isolate);
      }
    } else {
      v8::Local<v8::UnboundScript> unbound;
      if (v8::ScriptCompiler::CompileUnboundScript(isolate, &source)
              .ToLocal(&unbound)) {
        auto sfi = v8::Utils::OpenHandle(*unbound);
        script = i::handle(i::Cast<i::Script>(sfi->script()), i_isolate);
      }
    }
    if (!script.is_null()) CompileLazyFunctions(i_isolate, script);
  }
  try_catch.Reset();
  jsm::MaybeCollectGarbage(isolate);
  return 0;
}
