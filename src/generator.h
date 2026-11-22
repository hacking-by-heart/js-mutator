#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace js_mutator {
namespace internal {

// Small deterministic PRNG. It is deliberately not std::rand or a library
// distribution: the same seed must always produce the same program.
class Random {
 public:
  explicit Random(uint32_t seed) : state_(seed == 0 ? 0x9e3779b9u : seed) {}

  uint32_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }

  uint32_t Below(uint32_t limit) { return limit == 0 ? 0 : Next() % limit; }

  bool Chance(unsigned percent) {
    return Below(100) < static_cast<uint32_t>(percent);
  }

  template <typename T, size_t N>
  const T& Pick(const T (&items)[N]) {
    return items[Below(static_cast<uint32_t>(N))];
  }

 private:
  uint32_t state_;
};

// The JavaScript lexical vocabulary is compiled in. Keywords and punctuators
// have fixed spellings; identifiers, numbers, strings, templates, and regular
// expressions are produced by the generator's lexical rules.
inline constexpr std::string_view kKeywords[] = {
    "await",    "break",      "case",      "catch",     "class",
    "const",    "continue",   "debugger",  "default",   "delete",
    "do",       "else",       "enum",      "export",    "extends",
    "false",    "finally",    "for",       "function",  "if",
    "import",   "in",         "instanceof", "new",      "null",
    "return",   "super",      "switch",    "this",      "throw",
    "true",     "try",        "typeof",    "var",       "void",
    "while",    "with",       "yield",     "let",       "static",
    "implements", "interface", "package",  "private",   "protected",
    "public",   "async",      "get",       "set",       "of",
    "arguments", "eval",      "target",    "meta",      "from",
    "as",       "undefined"};

inline constexpr std::string_view kPunctuators[] = {
    "{",   "}",   "(",   ")",    "[",   "]",   ".",   "...", ";",   ",",
    "<",   ">",   "<=",  ">=",   "==",  "!=",  "===", "!==", "+",   "-",
    "*",   "/",   "%",   "**",   "++",  "--",  "<<",  ">>",  ">>>", "&",
    "|",   "^",   "!",   "~",    "&&",  "||",  "??",  "?",   "?.",  ":",
    "=",   "+=",  "-=",  "*=",   "/=",  "%=",  "**=", "<<=", ">>=", ">>>=",
    "&=",  "|=",  "^=",  "&&=",  "||=", "?\?=", "=>",  "#",   "@",   "`",
    "${",  "\"",  "'",   "/*",   "*/",  "//",  "<!--", "-->", "\\"};

// Curated Unicode tables, all checked against V8 15.8 and Tree-sitter.
enum class UnicodeClass {
  kIdStart,        // BMP ID_Start (including Other_ID_Start and Nl)
  kIdStartAstral,  // ID_Start outside the BMP
  kIdContinue,     // ID_Continue but not ID_Start (Mn, Mc, Nd, Pc, ZWNJ...)
  kWhiteSpace,     // non-ASCII WhiteSpace
  kLineTerminator,
  kLookAlike,      // not in the class it resembles; V8 and Tree-sitter may
                   // disagree, so only for the malformed share
};

inline constexpr uint32_t kUnicodeIdStart[] = {
    0xAA,   0xB5,   0xBA,   0xC0,   0xDF,   0xE9,   0xFF,   0x100,  0x17F,
    0x391,  0x3C0,  0x410,  0x44F,  0x5D0,  0x627,  0x905,  0xE01,  0x10D0,
    0x1E00, 0x2071, 0x2118, 0x212E, 0x2160, 0x309B, 0x3005, 0x3042, 0x30A2,
    0x4E2D, 0xAC00, 0xFB01, 0xFF21, 0xFFDC, 0x1885};
inline constexpr uint32_t kUnicodeIdStartAstral[] = {
    0x10000, 0x10400, 0x10480, 0x1D400, 0x1D6A8, 0x1E900, 0x20000, 0x2F800};
inline constexpr uint32_t kUnicodeIdContinue[] = {
    0x300,  0x36F,  0x483,  0x660,  0x669,  0x6F0,   0x903,   0x93C,  0x966,
    0xB7,   0x387,  0x1369, 0x19DA, 0x200C, 0x200D,  0x203F,  0x2040, 0x2054,
    0xFE33, 0xFE4F, 0xFF10, 0xFF3F, 0x20D0, 0x1D7CE, 0x101FD, 0x104A0, 0xE0100};
inline constexpr uint32_t kUnicodeWhiteSpace[] = {
    0xA0, 0x1680, 0x2000, 0x2005, 0x200A, 0x202F, 0x205F, 0x3000, 0xFEFF};
inline constexpr uint32_t kUnicodeLineTerminator[] = {0x2028, 0x2029};
inline constexpr uint32_t kUnicodeLookAlike[] = {
    0x200B, 0x2060, 0x180E, 0x85, 0x1F600, 0x2E2F, 0x37E, 0x2212, 0xFF1B,
    0xD7,   0xAD};

void AppendUtf8(std::string* out, uint32_t code_point);
// `\uXXXX` for the BMP, `\u{X}` (or a surrogate pair) otherwise.
std::string EscapeCodePoint(uint32_t code_point, bool braces);
// The identifier with \u escapes decoded, so `ab` and `ab` compare equal.
std::string CanonicalName(std::string_view name);

// Facts gathered from a real seed program, relative to the place where the
// generated code will be inserted.
struct SeedInfo {
  SeedInfo();
  SeedInfo(const SeedInfo&);
  SeedInfo(SeedInfo&&);
  SeedInfo& operator=(const SeedInfo&);
  SeedInfo& operator=(SeedInfo&&);
  ~SeedInfo();
  // Names visible at the destination and safe to reuse in expressions.
  std::vector<std::string> names;
  // Every identifier-like spelling in the seed. Generated declarations use
  // fresh names that avoid these, so insertion cannot redeclare a seed name.
  std::vector<std::string> reserved;
  // Seed expressions whose free names are all visible at the destination.
  std::vector<std::string> expressions;
  // Self-contained seed functions and classes, usable as building blocks.
  std::vector<std::string> blocks;
  // Names the module already exports; generated exports avoid them.
  std::vector<std::string> exports;
};

// The syntactic context of a position. Constructs such as return, await,
// yield, break, continue, super, new.target, and private names are only
// emitted when the context makes them legal.
struct GenContext {
  GenContext();
  GenContext(const GenContext&);
  GenContext(GenContext&&);
  GenContext& operator=(const GenContext&);
  GenContext& operator=(GenContext&&);
  ~GenContext();
  bool function = false;        // `return` is legal.
  bool async = false;           // `await` expressions are legal.
  bool generator = false;       // `yield` expressions are legal.
  bool loop = false;            // unlabeled `continue` (and `break`) legal.
  bool switch_stmt = false;     // unlabeled `break` legal.
  bool super_property = false;  // `super.x` / `super[x]` legal.
  bool super_call = false;      // `super(...)` legal.
  bool new_target = false;      // `new.target` legal.
  bool class_field = false;     // `arguments` is a SyntaxError here.
  bool strict = false;          // strict-mode code.
  // `await` and `yield` are plain identifiers: sloppy Script code outside
  // async functions, generators, and their parameters. Only set by the
  // generator itself, so seed contexts never use it.
  bool ident_await_yield = false;
  std::vector<std::string> labels;
  std::vector<std::string> loop_labels;
  std::vector<std::string> private_names;
  bool module = false;       // Module code: strict, `await` reserved.
  bool block_level = false;  // `using` declarations legal in this list.
};

// A recursive generator for ECMAScript Script code. Every production is
// composable, printing is precedence-aware, and output is bounded by `limit`.
class Generator {
 public:
  Generator(Random* random, size_t limit, const SeedInfo& seed);
  ~Generator();

  // A complete Script program.
  bool Program(std::string* out, unsigned depth, unsigned max_statements);
  // One statement legal in a statement list with `context`.
  bool Statement(std::string* out, const GenContext& context, unsigned depth);
  // One expression, parenthesized so it is safe in any expression position.
  bool Expression(std::string* out, const GenContext& context, unsigned depth);
  // A complete Module program, tagged with a leading /*M*/ comment.
  bool ModuleProgram(std::string* out, unsigned depth, unsigned max_statements);
  // One import or export declaration for the top level of a module.
  bool ModuleItem(std::string* out, const GenContext& context, unsigned depth);
  // A fresh literal of the same lexical kind as a Tree-sitter literal node
  // type (number, string, regex, template_string, true, false, null).
  std::string Literal(std::string_view type, bool strict, bool tagged);

  struct Expr {
    std::string text;
    int prec = 17;
    unsigned flags = 0;
  };

 private:
  enum class Pos { kList, kSub, kIfBody };

  void Reset();
  bool Spend(unsigned cost = 1);
  std::string Fit(const Expr& e, int min, unsigned forbid = 0);
  std::string FreshName(std::string_view prefix);
  void Declare(const std::string& name);
  void PushScope();
  void PopScope();
  // With `ctx`, names that are only legal in sloppy non-async non-generator
  // code (await, yield, let, eval, ...) are returned only where legal.
  bool VisibleName(std::string* out, bool reference,
                   const GenContext* ctx = nullptr);
  bool AssignableName(std::string* out, const GenContext* ctx = nullptr);

  // Binding names: fresh, reused from an outer scope (shadowing), or one of
  // the contextually reserved words where the context allows it.
  enum class BindKind { kNone, kVar, kLexical, kParam, kCatch };
  struct Frame {
    bool function = false;     // a function's parameter and body scope
    bool catch_param = false;  // the scope of a catch parameter
    std::vector<std::string> var_blocked;  // hoisted var names, no lexical reuse
  };
  std::string BindingName(const GenContext& ctx, BindKind kind,
                          std::string_view prefix);
  bool ReuseName(const GenContext& ctx, BindKind kind, std::string* out);
  bool SpecialNamesAllowed(const GenContext& ctx) const;
  bool NameUsableIn(const std::string& name, const GenContext& ctx) const;
  std::string EscapeIdent(const std::string& name);
  bool FrameHasSpecialOrDuplicate() const;
  std::string AnnexBStatement(const GenContext& ctx, unsigned depth);
  std::string SpecialFormStatement(const GenContext& ctx, unsigned depth,
                                   bool list);
  std::string ContextualClassMember(const GenContext& body_ctx,
                                    const GenContext& outer, unsigned depth);
  std::string CallTarget(const GenContext& ctx, unsigned depth);
  std::string ParenthesizedTarget(const GenContext& ctx, unsigned depth);

  // Lexical rules.
  std::string NumberLiteral(bool strict);
  std::string NumberGrammar();
  std::string PropertyKey();
  std::string LongLiteral(std::string_view type);
  std::string UnicodeChar(UnicodeClass c);
  uint32_t UnicodeCodePoint(UnicodeClass c);
  std::string RawUnicodePiece();
  std::string StringLiteral(bool strict);
  std::string TemplateText(bool tagged);
  std::string RegexLiteral();
  std::string PropertyName();

  // Statements.
  std::string StatementImpl(const GenContext& ctx, unsigned depth, Pos pos);
  std::string StatementList(const GenContext& ctx, unsigned depth,
                            unsigned max_statements, bool prologue,
                            bool* became_strict);
  std::string Block(const GenContext& ctx, unsigned depth, unsigned max);
  std::string SubStatement(const GenContext& ctx, unsigned depth, Pos pos);
  std::string VariableDeclaration(const GenContext& ctx, unsigned depth,
                                  bool var_only, bool for_head);
  std::string ForStatement(const GenContext& ctx, unsigned depth);
  std::string ForInOfStatement(const GenContext& ctx, unsigned depth);
  std::string SwitchStatement(const GenContext& ctx, unsigned depth);
  std::string TryStatement(const GenContext& ctx, unsigned depth);
  std::string LabeledStatement(const GenContext& ctx, unsigned depth);
  std::string BindingPattern(const GenContext& ctx, unsigned depth,
                             bool allow_pattern, std::vector<std::string>* names);
  std::string Parameters(const GenContext& ctx, unsigned depth, bool simple,
                         unsigned min_count, unsigned max_count,
                         bool allow_rest);

  // Functions and classes.
  std::string FunctionExpressionOrDeclaration(const GenContext& ctx,
                                              unsigned depth, bool declaration,
                                              std::string* declared_name);
  std::string FunctionTail(const GenContext& outer, unsigned depth,
                           GenContext body_ctx, unsigned param_min,
                           unsigned param_max, bool allow_rest,
                           const std::vector<std::string>& extra_names,
                           bool prepend_super_call);
  std::string ArrowFunction(const GenContext& ctx, unsigned depth);
  std::string ClassText(const GenContext& ctx, unsigned depth, bool declaration,
                        std::string* declared_name);
  std::string ObjectLiteral(const GenContext& ctx, unsigned depth);
  std::string FunctionLiteral(const GenContext& ctx, unsigned depth);
  // `using` / `await using` declarations, for-of heads, and look-alikes.
  std::string UsingStatement(const GenContext& ctx, unsigned depth, bool list);
  // Modules.
  std::string ModuleItemImpl(const GenContext& ctx, unsigned depth);
  std::string ModuleSpecifier();
  std::string ExportName();
  std::string TemplateLiteral(const GenContext& ctx, unsigned depth,
                              bool tagged);

  // Expressions.
  Expr ExpressionImpl(const GenContext& ctx, unsigned depth);
  Expr Atom(const GenContext& ctx);
  Expr Binary(const GenContext& ctx, unsigned depth);
  Expr Unary(const GenContext& ctx, unsigned depth);
  Expr Assignment(const GenContext& ctx, unsigned depth);
  Expr Call(const GenContext& ctx, unsigned depth);
  // `eval(...)`: mostly direct calls, sometimes the indirect forms.
  Expr EvalCall(const GenContext& ctx, unsigned depth);
  std::string EvalArgument(const GenContext& ctx, unsigned depth);
  Expr Member(const GenContext& ctx, unsigned depth);
  std::string ArgumentList(const GenContext& ctx, unsigned depth);

  // Cover grammar: one list finished as an arrow head, a parenthesized
  // expression, a destructuring assignment, or an async call or arrow.
  Expr Cover(const GenContext& ctx, unsigned depth);
  // A bare `yield` directly before a terminator: ] ) } : , and template end.
  Expr YieldTerminator(const GenContext& ctx, unsigned depth);
  // A left-hand-side chain of 3-6 mixed member, call, and template links.
  Expr Chain(const GenContext& ctx, unsigned depth);
  // A conditional chain of 3-6 arms.
  Expr ConditionalChain(const GenContext& ctx, unsigned depth);
  // A parameter default: direct eval, arguments, or a class with a static
  // block.
  std::string SpecialDefault(const GenContext& ctx, unsigned depth);

  Random* random_;
  size_t limit_;
  SeedInfo seed_;
  std::vector<std::vector<std::string>> scopes_;
  std::set<std::string> used_;
  unsigned name_counter_ = 0;
  unsigned nodes_left_ = 0;
  bool avoid_contextual_ = false;
  std::vector<Frame> frames_;            // parallel to scopes_
  BindKind bind_kind_ = BindKind::kNone;  // kind of names BindingPattern makes
  unsigned no_special_ = 0;     // inside async/generator parameters or arrows
  bool plain_function_ = false;  // next FunctionTail is a plain function
  std::set<std::string> special_used_;
  // Raw non-ASCII (UTF-8) output for this call; decided once per entry point.
  bool raw_unicode_ = false;
  std::set<std::string> exported_;
};

// Words that are identifiers only in sloppy, non-async, non-generator code
// (or that may not be bound in strict code).
bool IsSpecialName(std::string_view name);

// Early errors that Tree-sitter does not report (implemented in mutator.cc).
bool HasEarlyErrorForTesting(std::string_view source);

// The tag that marks a source as Module code.
inline constexpr std::string_view kModuleTag = "/*M*/";
// Tags for the REPL and eval goals of the V8 harness. Tagged sources stay
// plain scripts here: valid ones are valid scripts without `await` names.
inline constexpr std::string_view kReplTag = "/*R*/";
inline constexpr std::string_view kEvalTag = "/*E*/";

// True when an expression statement starting with `text` would be parsed as
// something else (a block, declaration, or `let [` declaration).
bool NeedsStatementParens(std::string_view text);

// An identifier spelling with its \uXXXX and \u{...} escapes decoded.
std::string DecodeIdentifier(std::string_view text);

// Reserved only in strict code: let, static, yield, implements, and so on.
bool IsStrictReservedWord(std::string_view name);

// True when Tree-sitter reports a syntax error or the early-error checker
// reports a parse-time error. Defined in mutator.cc; used by tests.
bool SourceHasEarlyError(std::string_view source);

}  // namespace internal
}  // namespace js_mutator
