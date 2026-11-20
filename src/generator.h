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
  bool VisibleName(std::string* out, bool reference);
  bool AssignableName(std::string* out);

  // Lexical rules.
  std::string NumberLiteral(bool strict);
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
  Expr Member(const GenContext& ctx, unsigned depth);
  std::string ArgumentList(const GenContext& ctx, unsigned depth);

  Random* random_;
  size_t limit_;
  SeedInfo seed_;
  std::vector<std::vector<std::string>> scopes_;
  std::set<std::string> used_;
  unsigned name_counter_ = 0;
  unsigned nodes_left_ = 0;
  bool avoid_contextual_ = false;
  std::set<std::string> exported_;
};

// The tag that marks a source as Module code.
inline constexpr std::string_view kModuleTag = "/*M*/";

// True when an expression statement starting with `text` would be parsed as
// something else (a block, declaration, or `let [` declaration).
bool NeedsStatementParens(std::string_view text);

}  // namespace internal
}  // namespace js_mutator
