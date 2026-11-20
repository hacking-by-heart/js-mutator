// One ParseProgram pass over an input, shared by parser.cc and diff.cc.

#ifndef JS_MUTATOR_HARNESS_PARSE_H_
#define JS_MUTATOR_HARNESS_PARSE_H_

#include <vector>

#include "src/ast/ast-traversal-visitor.h"
#include "src/ast/ast.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/isolate.h"
#include "src/objects/string.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parsing.h"
#include "src/parsing/pending-compilation-error-handler.h"
#include "src/regexp/regexp-parser.h"
#include "src/regexp/regexp.h"
#include "src/zone/zone.h"
#include "common.h"

namespace js_mutator_harness {

namespace i = v8::internal;

// Latin-1 for ASCII input (exactly as V8's parser fuzzer), lossy UTF-8
// otherwise, so non-ASCII input yields two-byte sources.
inline i::MaybeDirectHandle<i::String> NewSource(i::Isolate* isolate,
                                                 const uint8_t* data,
                                                 size_t size) {
  i::Factory* factory = isolate->factory();
  if (IsAscii(data, size)) {
    return factory->NewStringFromOneByte(v8::base::VectorOf(data, size));
  }
  return factory->NewStringFromUtf8(v8::base::VectorOf(data, size));
}

// Caps for the regexp re-parse.
constexpr size_t kMaxRegExps = 32;
constexpr int kMaxRegExpLength = 512;

class RegExpCollector final
    : public i::AstTraversalVisitor<RegExpCollector> {
 public:
  RegExpCollector(uintptr_t stack_limit, i::FunctionLiteral* root,
                  std::vector<i::RegExpLiteral*>* out)
      : i::AstTraversalVisitor<RegExpCollector>(stack_limit, root),
        out_(out) {}

  void VisitRegExpLiteral(i::RegExpLiteral* literal) {
    if (out_->size() < kMaxRegExps) out_->push_back(literal);
  }

 private:
  std::vector<i::RegExpLiteral*>* out_;
};

// Re-parses every regexp literal of the parsed program in BuildAST mode
// (the parse itself only runs the verify-syntax mode).
inline void ReparseRegExps(i::Isolate* isolate, i::ParseInfo* info) {
  std::vector<i::RegExpLiteral*> literals;
  RegExpCollector collector(isolate->stack_guard()->real_climit(),
                            info->literal(), &literals);
  collector.Run();
  if (collector.HasStackOverflow()) collector.ClearStackOverflow();

  i::Factory* factory = isolate->factory();
  for (i::RegExpLiteral* literal : literals) {
    const i::AstRawString* raw = literal->raw_pattern();
    if (raw->length() > kMaxRegExpLength) continue;
    i::HandleScope scope(isolate);
    i::MaybeDirectHandle<i::String> maybe_pattern;
    if (raw->is_one_byte()) {
      maybe_pattern = factory->NewStringFromOneByte(
          v8::base::VectorOf(raw->raw_data(), raw->length()));
    } else {
      maybe_pattern = factory->NewStringFromTwoByte(v8::base::VectorOf(
          reinterpret_cast<const v8::base::uc16*>(raw->raw_data()),
          raw->length()));
    }
    i::DirectHandle<i::String> pattern;
    if (!maybe_pattern.ToHandle(&pattern)) continue;
    i::Zone zone(isolate->allocator(), ZONE_NAME);
    i::regexp::CompileData data;
    i::regexp::Parser::ParseRegExpFromHeapString(
        isolate, &zone, pattern,
        i::regexp::Flags(static_cast<i::regexp::Flags::mask_type>(
            literal->flags())),
        &data);
  }
}

struct ParseOptions {
  Goal goal = Goal::kScript;
  bool compile_hints = false;
  bool lazy = true;  // false: the full parser runs on every function
  bool reparse_regexps = false;
};

struct ParseResult {
  bool ok = false;
  bool stack_overflow = false;
  int max_info_id = 0;
};

// Parses |source| with a fresh Script and ParseInfo. The goal is set on the
// Script (module, REPL) or on the flags (eval) before parsing starts.
inline ParseResult ParseOnce(i::Isolate* isolate,
                             i::DirectHandle<i::String> source,
                             const ParseOptions& options) {
  i::HandleScope scope(isolate);
  i::Handle<i::Script> script = isolate->factory()->NewScript(source);
  if (options.goal == Goal::kModule) {
    script->set_origin_options(v8::ScriptOriginOptions(false, false, false,
                                                       /*is_module=*/true));
  } else if (options.goal == Goal::kRepl) {
    script->set_is_repl_mode(true);
  }

  i::UnoptimizedCompileState state;
  i::ReusableUnoptimizedCompileState reusable_state(isolate);
  i::UnoptimizedCompileFlags flags =
      i::UnoptimizedCompileFlags::ForScriptCompile(isolate, *script);
  if (options.goal == Goal::kEval) flags.set_is_eval(true);
  if (options.compile_hints) {
    flags.set_compile_hints_magic_enabled(true);
    flags.set_compile_hints_per_function_magic_enabled(true);
  }
  if (!options.lazy) flags.set_allow_lazy_compile(false);

  i::ParseInfo info(isolate, flags, &state, &reusable_state);
  ParseResult result;
  result.ok = i::parsing::ParseProgram(&info, script, isolate,
                                       i::parsing::ReportStatisticsMode{true});
  if (result.ok) {
    result.max_info_id = info.max_info_id();
    if (options.reparse_regexps) ReparseRegExps(isolate, &info);
  } else {
    result.stack_overflow = info.pending_error_handler()->stack_overflow();
    info.pending_error_handler()->PrepareErrors(isolate,
                                                info.ast_value_factory());
    info.pending_error_handler()->ReportErrors(isolate, script);
    isolate->clear_exception();
    isolate->clear_pending_message();
  }
  return result;
}

}  // namespace js_mutator_harness

#endif  // JS_MUTATOR_HARNESS_PARSE_H_
