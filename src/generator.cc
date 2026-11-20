#include "generator.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace js_mutator {
namespace internal {
namespace {

// Expression precedence, lowest first, following the ECMAScript grammar.
enum Prec {
  kSeq = 0,
  kAssign,  // assignment, arrow functions, yield
  kCond,
  kOr,      // ||, ??  (never mixed without parentheses)
  kAnd,
  kBitOr,
  kBitXor,
  kBitAnd,
  kEq,
  kRel,
  kShift,
  kAdd,
  kMul,
  kExp,
  kUnary,   // unary operators, await
  kUpdate,  // ++/--, and `new X` without arguments
  kLHS,     // calls, member access, tagged templates
  kPrimary,
};

enum Flag : unsigned {
  kCoalesce = 1u << 0,   // a ?? b
  kLogical = 1u << 1,    // a || b, a && b
  kOptional = 1u << 2,   // part of an optional chain
  kCall = 1u << 3,       // call or import()
  kNumber = 1u << 4,     // numeric literal (1.x is not member access)
  kPrivate = 1u << 5,    // ends in a private member access
};

using Expr = Generator::Expr;

constexpr std::string_view kPlainProperties[] = {
    "length", "value", "name",  "prototype", "x",     "y",    "then",
    "toString", "valueOf", "next", "done",  "call",   "apply", "bind",
    "raw",    "size",  "has",   "push",      "map",   "at",   "key",
    "description", "message", "source", "flags", "lastIndex", "buffer"};

constexpr std::string_view kGlobals[] = {
    "Object", "Array",   "String",  "Number",   "Boolean", "Symbol",
    "Promise", "Map",    "Set",     "WeakMap",  "Math",    "JSON",
    "Reflect", "Proxy",  "Error",   "RegExp",   "Date",    "BigInt",
    "globalThis", "Function", "Uint8Array", "ArrayBuffer", "parseInt",
    "isNaN",  "undefined", "NaN",   "Infinity"};

// Identifiers that are never reserved in Script code and stay usable as
// binding names in strict code, but have special meaning in some positions.
constexpr std::string_view kContextualNames[] = {
    "async", "of", "get", "set", "from", "as", "target", "meta"};

constexpr std::string_view kNumbers[] = {
    "0",     "1",        "2",          "7",        "42",       "255",
    "1000",  "3.14",     ".5",         "5.",       "0.0",      "1e3",
    "1E-3",  "2e+2",     "6.02e23",    "0x1F",     "0XaB",     "0xFFFFFFFF",
    "0o17",  "0O7",      "0b101",      "0B1",      "1_000",    "1_0.0_1",
    "0xF_F", "0b1_0",    "1e1_0",      "9007199254740993",
    "1.7976931348623157e308",          "5e-324",   "4294967295",
    "2147483648", "0n",  "1n",         "123n",     "0x1fn",    "0o7n",
    "0b11n", "1_000n",   "18446744073709551616n"};

constexpr std::string_view kSloppyNumbers[] = {"017", "08", "00", "0777"};

constexpr std::string_view kStringPieces[] = {
    "a",       "item",     "x y",     "use",     "0",       "\\n",
    "\\t",     "\\r",      "\\b",     "\\f",     "\\v",     "\\0",
    "\\'",     "\\\"",     "\\\\",    "\\x41",   "\\u0041", "\\u{1F600}",
    "\\u{10FFFF}", "\\u2028", "\\uD83D\\uDE00", "\\\n",   "\\a",  "$",
    "{",       "}",        "/*",      "//",      "<!--",    "`"};

constexpr std::string_view kSloppyStringPieces[] = {"\\1", "\\08", "\\377",
                                                    "\\8", "\\9"};

constexpr std::string_view kTemplatePieces[] = {
    "",       "text",   "a b",    "\\n",   "\\`",       "\\${",
    "$x",     "{}",     "}",      "\\u0041", "\\x41",    "\\u{1F600}",
    "\\\\",   "\n",     "'\"",    "\\0"};

// Not valid escapes; only allowed in tagged templates.
constexpr std::string_view kTaggedOnlyPieces[] = {"\\u", "\\xg", "\\u{",
                                                  "\\01", "\\1", "\\u{110000}"};

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '\\';
}

bool StartsWithWord(std::string_view text, std::string_view word) {
  return text.substr(0, word.size()) == word &&
         (text.size() == word.size() || !IsIdentChar(text[word.size()]));
}

bool IsContextual(std::string_view name) {
  for (std::string_view c : kContextualNames) {
    if (name == c) return true;
  }
  return false;
}

std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += sep;
    out += parts[i];
  }
  return out;
}

bool ContainsInOperator(std::string_view text) {
  return text.find(" in ") != std::string_view::npos;
}

}  // namespace

bool NeedsStatementParens(std::string_view text) {
  if (text.empty()) return false;
  if (text[0] == '{') return true;
  if (StartsWithWord(text, "function") || StartsWithWord(text, "class")) {
    return true;
  }
  if (StartsWithWord(text, "let")) {
    size_t i = 3;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n')) ++i;
    if (i < text.size() && text[i] == '[') return true;
  }
  if (StartsWithWord(text, "async")) {
    size_t i = 5;
    while (i < text.size() && text[i] == ' ') ++i;
    if (StartsWithWord(text.substr(i), "function")) return true;
  }
  return false;
}

// Out of line, as Chromium style requires for non-trivial types.
SeedInfo::SeedInfo() = default;
SeedInfo::SeedInfo(const SeedInfo&) = default;
SeedInfo::SeedInfo(SeedInfo&&) = default;
SeedInfo& SeedInfo::operator=(const SeedInfo&) = default;
SeedInfo& SeedInfo::operator=(SeedInfo&&) = default;
SeedInfo::~SeedInfo() = default;
GenContext::GenContext() = default;
GenContext::GenContext(const GenContext&) = default;
GenContext::GenContext(GenContext&&) = default;
GenContext& GenContext::operator=(const GenContext&) = default;
GenContext& GenContext::operator=(GenContext&&) = default;
GenContext::~GenContext() = default;
Generator::~Generator() = default;

Generator::Generator(Random* random, size_t limit, const SeedInfo& seed)
    : random_(random), limit_(limit), seed_(seed) {
  Reset();
}

void Generator::Reset() {
  scopes_.assign(1, {});
  used_.clear();
  name_counter_ = 0;
  for (std::string_view keyword : kKeywords) used_.insert(std::string(keyword));
  for (const std::string& name : seed_.reserved) used_.insert(name);
  for (const std::string& name : seed_.names) {
    used_.insert(name);
    scopes_[0].push_back(name);
  }
  const unsigned budget =
      static_cast<unsigned>(std::max<size_t>(4, std::min<size_t>(limit_ / 7, 360)));
  nodes_left_ = budget / 4 + random_->Below(budget - budget / 4 + 1);
}

bool Generator::Spend(unsigned cost) {
  if (nodes_left_ < cost) {
    nodes_left_ = 0;
    return false;
  }
  nodes_left_ -= cost;
  return true;
}

std::string Generator::Fit(const Expr& e, int min, unsigned forbid) {
  if (e.prec < min || (e.flags & forbid) != 0 || random_->Chance(2)) {
    return "(" + e.text + ")";
  }
  return e.text;
}

std::string Generator::FreshName(std::string_view prefix) {
  if (!avoid_contextual_ && random_->Chance(3)) {
    const std::string name(random_->Pick(kContextualNames));
    if (used_.insert(name).second) return name;
  }
  std::string lead;
  if (random_->Chance(8)) lead = random_->Chance(50) ? "$" : "_";
  for (;;) {
    std::string name = lead + std::string(prefix) + std::to_string(++name_counter_);
    if (used_.insert(name).second) return name;
  }
}

void Generator::Declare(const std::string& name) {
  if (name.empty()) return;
  used_.insert(name);
  if (scopes_.empty()) scopes_.emplace_back();
  scopes_.back().push_back(name);
}

void Generator::PushScope() { scopes_.emplace_back(); }

void Generator::PopScope() {
  if (scopes_.size() > 1) scopes_.pop_back();
}

bool Generator::VisibleName(std::string* out, bool reference) {
  size_t count = 0;
  for (const auto& scope : scopes_) count += scope.size();
  if (count == 0) return false;
  size_t index = random_->Below(static_cast<uint32_t>(count));
  for (const auto& scope : scopes_) {
    if (index < scope.size()) {
      *out = scope[index];
      break;
    }
    index -= scope.size();
  }
  // An escaped identifier names the same binding: abc is abc.
  if (reference && !out->empty() && !IsContextual(*out) && random_->Chance(4)) {
    const char first = (*out)[0];
    if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')) {
      static const char kHex[] = "0123456789abcdef";
      std::string escaped = "\\u00";
      escaped += kHex[(first >> 4) & 0xf];
      escaped += kHex[first & 0xf];
      *out = escaped + out->substr(1);
    }
  }
  return true;
}

bool Generator::AssignableName(std::string* out) {
  return VisibleName(out, true);
}

// ---------------------------------------------------------------------------
// Lexical rules.

std::string Generator::NumberLiteral(bool strict) {
  if (!strict && random_->Chance(6)) {
    return std::string(random_->Pick(kSloppyNumbers));
  }
  return std::string(random_->Pick(kNumbers));
}

std::string Generator::StringLiteral(bool strict) {
  const char quote = random_->Chance(50) ? '"' : '\'';
  std::string out(1, quote);
  const unsigned pieces = random_->Below(4);
  for (unsigned i = 0; i < pieces; ++i) {
    std::string_view piece = random_->Pick(kStringPieces);
    if (!strict && random_->Chance(8)) piece = random_->Pick(kSloppyStringPieces);
    // \0 must not be followed by a digit, or it becomes a legacy octal escape.
    if (!out.empty() && out.size() >= 2 && out.substr(out.size() - 2) == "\\0" &&
        !piece.empty() && piece[0] >= '0' && piece[0] <= '9') {
      continue;
    }
    if (piece == "\\0" && i + 1 < pieces) {
      out += "\\0 ";
      continue;
    }
    out += piece;
  }
  out += quote;
  return out;
}

std::string Generator::TemplateText(bool tagged) {
  std::string out;
  const unsigned pieces = random_->Below(3);
  for (unsigned i = 0; i < pieces; ++i) {
    std::string_view piece = random_->Pick(kTemplatePieces);
    if (tagged && random_->Chance(20)) piece = random_->Pick(kTaggedOnlyPieces);
    if (piece == "\\0") {
      out += "\\0 ";
      continue;
    }
    // `$` immediately followed by `{` would start a substitution.
    if (!out.empty() && out.back() == '$' && !piece.empty() && piece[0] == '{') {
      out += ' ';
    }
    out += piece;
  }
  if (!out.empty() && out.back() == '$') out += ' ';
  return out;
}

std::string Generator::RegexLiteral() {
  std::vector<char> flags;
  for (char flag : std::string_view("dgimsy")) {
    if (random_->Chance(25)) flags.push_back(flag);
  }
  bool unicode = false;
  bool sets = false;
  if (random_->Chance(30)) {
    unicode = true;
    flags.push_back('u');
  } else if (random_->Chance(15)) {
    sets = true;
    flags.push_back('v');
  }
  for (size_t n = flags.size(); n > 1; --n) {
    std::swap(flags[n - 1], flags[random_->Below(static_cast<uint32_t>(n))]);
  }

  static constexpr std::string_view kAtoms[] = {
      "a", "b", "x", ".", "\\d", "\\D", "\\w", "\\W", "\\s", "\\S", "\\.",
      "\\/", "\\n", "\\t", "\\u0041", "\\x41", "[abc]", "[^a-z]", "[\\d_]",
      "[a-z0-9]", "\\*", "\\(", "\\)"};
  static constexpr std::string_view kUnicodeAtoms[] = {
      "\\u{1F600}", "\\p{L}", "\\P{Lu}", "\\p{Script=Greek}", "\\p{N}"};
  static constexpr std::string_view kSetAtoms[] = {
      "[\\p{L}--[a-z]]", "[[a-z]&&[aeiou]]", "[\\q{abc|d}]", "[[0-9][a-f]]"};
  static constexpr std::string_view kQuantifiers[] = {"*", "+", "?", "{2}",
                                                      "{1,}", "{1,3}"};
  static constexpr std::string_view kAssertions[] = {"^", "$", "\\b", "\\B"};

  unsigned groups = 0;
  std::vector<std::string> named;
  unsigned name_counter = 0;

  // Recursive disjunction builder.
  std::string body;
  auto atom = [&](auto&& self, unsigned depth) -> std::string {
    const unsigned pick = random_->Below(100);
    if (depth > 0 && pick < 18) {
      const unsigned kind = random_->Below(7);
      std::string inner = self(self, depth - 1);
      switch (kind) {
        case 0:
          ++groups;
          return "(" + inner + ")";
        case 1:
          return "(?:" + inner + ")";
        case 2: {
          ++groups;
          std::string name = "g" + std::to_string(++name_counter);
          named.push_back(name);
          return "(?<" + name + ">" + inner + ")";
        }
        case 3:
          return "(?=" + inner + ")";
        case 4:
          return "(?!" + inner + ")";
        case 5:
          return "(?<=" + inner + ")";
        default:
          return "(?<!" + inner + ")";
      }
    }
    if (pick < 26) return std::string(random_->Pick(kAssertions));
    if (pick < 30 && groups > 0) {
      return "\\" + std::to_string(1 + random_->Below(groups));
    }
    if (pick < 33 && !named.empty()) {
      return "\\k<" + named[random_->Below(static_cast<uint32_t>(named.size()))] +
             ">";
    }
    if (unicode && pick < 42) return std::string(random_->Pick(kUnicodeAtoms));
    if (sets && pick < 42) {
      return random_->Chance(50) ? std::string(random_->Pick(kSetAtoms))
                                 : std::string(random_->Pick(kUnicodeAtoms));
    }
    return std::string(random_->Pick(kAtoms));
  };
  auto term = [&](unsigned depth) {
    std::string text = atom(atom, depth);
    const bool quantifiable =
        !(text == "^" || text == "$" || text == "\\b" || text == "\\B" ||
          text.rfind("(?=", 0) == 0 || text.rfind("(?!", 0) == 0 ||
          text.rfind("(?<=", 0) == 0 || text.rfind("(?<!", 0) == 0);
    if (quantifiable && random_->Chance(30)) {
      text += random_->Pick(kQuantifiers);
      if (random_->Chance(30)) text += "?";
    }
    return text;
  };
  const unsigned alternatives = 1 + (random_->Chance(20) ? random_->Below(2) + 1 : 0);
  for (unsigned a = 0; a < alternatives; ++a) {
    if (a != 0) body += "|";
    const unsigned terms = 1 + random_->Below(3);
    for (unsigned t = 0; t < terms; ++t) body += term(2);
  }
  if (body.empty() || body[0] == '*') body = "a" + body;
  return "/" + body + "/" + std::string(flags.begin(), flags.end());
}

std::string Generator::PropertyName() {
  const unsigned pick = random_->Below(100);
  if (pick < 60) return std::string(random_->Pick(kPlainProperties));
  if (pick < 92) return std::string(random_->Pick(kKeywords));
  return std::string(random_->Pick(kContextualNames));
}

// ---------------------------------------------------------------------------
// Statements.

std::string Generator::StatementList(const GenContext& context, unsigned depth,
                                     unsigned max_statements, bool prologue,
                                     bool* became_strict) {
  GenContext ctx = context;
  std::vector<std::string> statements;
  if (prologue && !ctx.strict && random_->Chance(12)) {
    statements.push_back(random_->Chance(50) ? "\"use strict\";" : "'use strict';");
    ctx.strict = true;
    if (became_strict) *became_strict = true;
  }
  const unsigned count = random_->Below(max_statements + 1);
  for (unsigned i = 0; i < count; ++i) {
    if (!Spend()) break;
    statements.push_back(StatementImpl(ctx, depth, Pos::kList));
  }
  std::string out;
  for (size_t i = 0; i < statements.size(); ++i) {
    std::string piece = statements[i];
    const bool directive = prologue && i == 0 && ctx.strict && !context.strict;
    // Automatic semicolon insertion: drop a semicolon when the next line
    // cannot continue the statement.
    // An empty-statement body (`for (...) ;`, `L: ;`) keeps its semicolon.
    if (!directive && piece.size() > 1 && piece.back() == ';' &&
        piece[piece.size() - 2] != ' ' && random_->Chance(10)) {
      const bool last = i + 1 == statements.size();
      const char next = last ? '\0' : statements[i + 1][0];
      if (last || std::strchr("([`+-/", next) == nullptr) piece.pop_back();
    }
    if (i != 0) out += "\n";
    if (!directive && random_->Chance(5)) {
      switch (random_->Below(5)) {
        case 0:
          out += "/* c */ ";
          break;
        case 1:
          out += "/* line\ncomment */ ";
          break;
        case 2:
          out += "// c\n";
          break;
        case 3:
          out += "<!-- html open\n";
          break;
        default:
          out += "\n--> html close\n";
          break;
      }
    }
    out += piece;
  }
  return out;
}

std::string Generator::Block(const GenContext& ctx, unsigned depth, unsigned max) {
  PushScope();
  std::string list = StatementList(ctx, depth, max, false, nullptr);
  PopScope();
  if (list.empty()) return "{}";
  return "{\n" + list + "\n}";
}

std::string Generator::SubStatement(const GenContext& ctx, unsigned depth,
                                    Pos pos) {
  if (random_->Chance(55)) return Block(ctx, depth, 3);
  PushScope();
  std::string text = StatementImpl(ctx, depth, pos);
  PopScope();
  return text;
}

std::string Generator::BindingPattern(const GenContext& ctx, unsigned depth,
                                      bool allow_pattern,
                                      std::vector<std::string>* names) {
  auto default_value = [&]() {
    return " = " + Fit(ExpressionImpl(ctx, depth > 0 ? depth - 1 : 0), kAssign);
  };
  if (!allow_pattern || depth == 0 || !Spend()) {
    std::string name = FreshName("v");
    names->push_back(name);
    return name;
  }
  std::vector<std::string> parts;
  const unsigned count = random_->Below(4);
  if (random_->Chance(55)) {
    for (unsigned i = 0; i < count; ++i) {
      const unsigned pick = random_->Below(100);
      if (pick < 40) {
        std::string name = FreshName("v");
        names->push_back(name);
        parts.push_back(random_->Chance(30) ? name + default_value() : name);
      } else {
        std::string key;
        if (random_->Chance(15)) {
          key = "[" + Fit(ExpressionImpl(ctx, depth - 1), kAssign) + "]";
        } else if (random_->Chance(15)) {
          key = StringLiteral(ctx.strict);
        } else {
          key = PropertyName();
        }
        std::string value = BindingPattern(ctx, depth - 1, true, names);
        if (random_->Chance(25)) value += default_value();
        parts.push_back(key + ": " + value);
      }
    }
    if (random_->Chance(15)) {
      std::string name = FreshName("r");
      names->push_back(name);
      parts.push_back("..." + name);
    }
    return "{" + Join(parts, ", ") + "}";
  }
  for (unsigned i = 0; i < count; ++i) {
    if (random_->Chance(15)) {
      parts.push_back("");
      continue;
    }
    std::string element = BindingPattern(ctx, depth - 1, true, names);
    if (random_->Chance(25)) element += default_value();
    parts.push_back(element);
  }
  if (random_->Chance(15)) {
    parts.push_back("..." + BindingPattern(ctx, depth - 1, true, names));
  } else if (!parts.empty() && parts.back().empty()) {
    parts.push_back(BindingPattern(ctx, 0, false, names));
  }
  return "[" + Join(parts, ", ") + "]";
}

std::string Generator::Parameters(const GenContext& ctx, unsigned depth,
                                  bool simple, unsigned min_count,
                                  unsigned max_count, bool allow_rest) {
  const unsigned count =
      min_count + random_->Below(max_count - min_count + 1);
  std::vector<std::string> parts;
  for (unsigned i = 0; i < count; ++i) {
    std::vector<std::string> names;
    std::string param;
    if (simple) {
      param = FreshName("p");
      names.push_back(param);
    } else {
      param = BindingPattern(ctx, depth, random_->Chance(30), &names);
      if (random_->Chance(30)) {
        // Defaults that are functions, classes, or arrows get their own scopes
        // inside the parameter scope, a parser-heavy combination.
        const unsigned d = depth > 0 ? depth - 1 : 0;
        std::string value;
        const unsigned pick = random_->Below(100);
        std::string ignored;
        if (pick < 12) {
          value = ClassText(ctx, d, false, &ignored);
        } else if (pick < 22) {
          value = ArrowFunction(ctx, d);
        } else if (pick < 28) {
          value = FunctionExpressionOrDeclaration(ctx, d, false, &ignored);
        } else {
          value = Fit(ExpressionImpl(ctx, d), kAssign);
        }
        param += " = " + value;
      }
    }
    // Earlier parameters are visible in later defaults.
    for (const std::string& name : names) Declare(name);
    parts.push_back(param);
  }
  if (!simple && allow_rest && random_->Chance(15)) {
    std::vector<std::string> names;
    parts.push_back("..." + BindingPattern(ctx, depth, random_->Chance(25), &names));
    for (const std::string& name : names) Declare(name);
  } else if (allow_rest && !parts.empty() && random_->Chance(5)) {
    // Setters, the only callers without rest, do not allow a trailing comma.
    return Join(parts, ", ") + ",";
  }
  return Join(parts, ", ");
}

std::string Generator::VariableDeclaration(const GenContext& ctx, unsigned depth,
                                           bool var_only, bool for_head) {
  std::string kind = "var";
  if (!var_only) {
    const unsigned pick = random_->Below(100);
    kind = pick < 25 ? "var" : pick < 60 ? "let" : "const";
  }
  const unsigned count = for_head ? 1 + random_->Below(2) : 1 + (random_->Chance(20) ? random_->Below(2) + 1 : 0);
  std::vector<std::string> parts;
  std::vector<std::string> declared;
  for (unsigned i = 0; i < count; ++i) {
    std::vector<std::string> names;
    const bool pattern = random_->Chance(22);
    std::string binding = BindingPattern(ctx, depth, pattern, &names);
    const bool needs_init = kind == "const" || binding[0] == '{' || binding[0] == '[';
    if (needs_init || random_->Chance(70)) {
      std::string init = Fit(ExpressionImpl(ctx, depth > 0 ? depth - 1 : 0), kAssign);
      if (for_head && ContainsInOperator(init)) init = "(" + init + ")";
      binding += " = " + init;
    }
    parts.push_back(binding);
    declared.insert(declared.end(), names.begin(), names.end());
  }
  for (const std::string& name : declared) Declare(name);
  return kind + " " + Join(parts, ", ");
}

std::string Generator::ForStatement(const GenContext& ctx, unsigned depth) {
  GenContext loop = ctx;
  loop.loop = true;
  PushScope();
  std::string init;
  const unsigned pick = random_->Below(100);
  if (pick < 55) {
    avoid_contextual_ = true;
    init = VariableDeclaration(ctx, depth, false, true);
    avoid_contextual_ = false;
  } else if (pick < 80) {
    init = Fit(ExpressionImpl(ctx, depth), kSeq);
    if (ContainsInOperator(init) || NeedsStatementParens(init)) init = "(" + init + ")";
  }
  std::string test = random_->Chance(80) ? Fit(ExpressionImpl(ctx, depth), kSeq) : "";
  std::string update = random_->Chance(75) ? Fit(ExpressionImpl(ctx, depth), kSeq) : "";
  std::string body = SubStatement(loop, depth, Pos::kSub);
  PopScope();
  return "for (" + init + "; " + test + "; " + update + ") " + body;
}

std::string Generator::ForInOfStatement(const GenContext& ctx, unsigned depth) {
  GenContext loop = ctx;
  loop.loop = true;
  PushScope();
  const bool of = random_->Chance(55);
  const bool await_of = of && ctx.async && random_->Chance(30);
  std::string head;
  std::vector<std::string> names;
  std::string target;
  const unsigned pick = random_->Below(100);
  if (!of && !ctx.strict && pick < 5) {
    // Annex B: for (var x = init in obj)
    std::string name = FreshName("v");
    names.push_back(name);
    head = "var " + name + " = " + Fit(ExpressionImpl(ctx, depth > 0 ? depth - 1 : 0), kBitOr);
  } else if (pick < 80 || !VisibleName(&target, false) || IsContextual(target)) {
    const unsigned kind = random_->Below(3);
    // `for (let of ...)` and similar spellings are ambiguous; avoid
    // contextual keywords as loop heads.
    avoid_contextual_ = true;
    std::string binding = BindingPattern(ctx, depth, random_->Chance(25), &names);
    avoid_contextual_ = false;
    head = std::string(kind == 0 ? "var " : kind == 1 ? "let " : "const ") + binding;
  } else {
    head = target;
  }
  std::string right = of ? Fit(ExpressionImpl(ctx, depth), kAssign)
                         : Fit(ExpressionImpl(ctx, depth), kSeq);
  for (const std::string& name : names) Declare(name);
  std::string body = SubStatement(loop, depth, Pos::kSub);
  PopScope();
  return std::string("for ") + (await_of ? "await " : "") + "(" + head +
         (of ? " of " : " in ") + right + ") " + body;
}

std::string Generator::SwitchStatement(const GenContext& ctx, unsigned depth) {
  GenContext inner = ctx;
  inner.switch_stmt = true;
  std::string discriminant = Fit(ExpressionImpl(ctx, depth), kSeq);
  PushScope();
  std::string out = "switch (" + discriminant + ") {";
  const unsigned cases = random_->Below(4);
  const int default_at = random_->Chance(55) ? static_cast<int>(random_->Below(cases + 1)) : -1;
  for (unsigned i = 0; i <= cases; ++i) {
    if (static_cast<int>(i) == default_at) {
      out += "\ndefault:";
    } else if (i < cases) {
      out += "\ncase " + Fit(ExpressionImpl(ctx, depth), kSeq) + ":";
    } else {
      continue;
    }
    std::string list = StatementList(inner, depth, 2, false, nullptr);
    if (!list.empty()) out += "\n" + list;
  }
  PopScope();
  return out + "\n}";
}

std::string Generator::TryStatement(const GenContext& ctx, unsigned depth) {
  std::string out = "try " + Block(ctx, depth, 3);
  const bool has_catch = random_->Chance(80);
  if (has_catch) {
    PushScope();
    const unsigned pick = random_->Below(100);
    if (pick < 20) {
      out += " catch ";
    } else {
      std::vector<std::string> names;
      std::string param = BindingPattern(ctx, depth, pick > 80, &names);
      for (const std::string& name : names) Declare(name);
      out += " catch (" + param + ") ";
    }
    out += Block(ctx, depth, 3);
    PopScope();
  }
  if (!has_catch || random_->Chance(40)) out += " finally " + Block(ctx, depth, 2);
  return out;
}

std::string Generator::LabeledStatement(const GenContext& ctx, unsigned depth) {
  std::string label = FreshName("L");
  GenContext inner = ctx;
  inner.labels.push_back(label);
  std::string body;
  if (random_->Chance(55)) {
    inner.loop_labels.push_back(label);
    switch (random_->Below(4)) {
      case 0:
        body = ForStatement(inner, depth);
        break;
      case 1:
        body = ForInOfStatement(inner, depth);
        break;
      case 2: {
        GenContext loop = inner;
        loop.loop = true;
        body = "while (" + Fit(ExpressionImpl(ctx, depth), kSeq) + ") " +
               SubStatement(loop, depth, Pos::kSub);
        break;
      }
      default: {
        GenContext loop = inner;
        loop.loop = true;
        body = "do " + SubStatement(loop, depth, Pos::kSub) + " while (" +
               Fit(ExpressionImpl(ctx, depth), kSeq) + ");";
        break;
      }
    }
  } else {
    body = SubStatement(inner, depth, Pos::kSub);
  }
  return label + ": " + body;
}

std::string Generator::StatementImpl(const GenContext& ctx, unsigned depth,
                                     Pos pos) {
  auto expression_statement = [&](unsigned d) {
    std::string text = Fit(d == 0 ? Atom(ctx) : ExpressionImpl(ctx, d), kSeq);
    if (NeedsStatementParens(text)) text = "(" + text + ")";
    return text + ";";
  };
  if (depth == 0 || !Spend()) return expression_statement(0);
  const unsigned d = depth - 1;
  const bool list = pos == Pos::kList;

  enum Kind {
    kExpr, kVar, kIf, kFor, kForInOf, kWhile, kDo, kBlock, kLabel, kFunc,
    kClass, kTry, kSwitch, kReturn, kThrow, kJump, kWith, kDebugger,
    kEmpty, kAnnexBFunction, kCount
  };
  unsigned weights[kCount] = {};
  weights[kExpr] = 14;
  weights[kVar] = 12;
  weights[kIf] = 9;
  weights[kFor] = 5;
  weights[kForInOf] = 5;
  weights[kWhile] = 3;
  weights[kDo] = 3;
  weights[kBlock] = 3;
  weights[kLabel] = 3;
  weights[kFunc] = list ? 9 : 0;
  weights[kClass] = list ? 8 : 0;
  weights[kTry] = 5;
  weights[kSwitch] = 4;
  weights[kReturn] = ctx.function ? 5 : 0;
  weights[kThrow] = 2;
  weights[kJump] = (ctx.loop || ctx.switch_stmt || !ctx.labels.empty()) ? 5 : 0;
  weights[kWith] = ctx.strict ? 0 : 2;
  weights[kDebugger] = 1;
  weights[kEmpty] = 1;
  weights[kAnnexBFunction] = (pos == Pos::kIfBody && !ctx.strict) ? 6 : 0;
  unsigned total = 0;
  for (unsigned w : weights) total += w;
  unsigned roll = random_->Below(total);
  unsigned kind = 0;
  while (roll >= weights[kind]) roll -= weights[kind++];

  switch (kind) {
    case kExpr:
      return expression_statement(d);
    case kVar:
      return VariableDeclaration(ctx, d, !list, false) + ";";
    case kIf: {
      std::string out = "if (" + Fit(ExpressionImpl(ctx, d), kSeq) + ") " +
                        SubStatement(ctx, d, Pos::kIfBody);
      if (random_->Chance(40)) {
        // A single-statement consequent must end before `else`.
        out += (out.back() == '}' || out.back() == ';') ? " else " : "; else ";
        out += SubStatement(ctx, d, Pos::kIfBody);
      }
      return out;
    }
    case kFor:
      return ForStatement(ctx, d);
    case kForInOf:
      return ForInOfStatement(ctx, d);
    case kWhile: {
      GenContext loop = ctx;
      loop.loop = true;
      return "while (" + Fit(ExpressionImpl(ctx, d), kSeq) + ") " +
             SubStatement(loop, d, Pos::kSub);
    }
    case kDo: {
      GenContext loop = ctx;
      loop.loop = true;
      std::string out = "do " + SubStatement(loop, d, Pos::kSub) + " while (" +
                        Fit(ExpressionImpl(ctx, d), kSeq) + ")";
      return (list && random_->Chance(20)) ? out : out + ";";
    }
    case kBlock:
      return Block(ctx, d, 4);
    case kLabel:
      return LabeledStatement(ctx, d);
    case kFunc:
    case kAnnexBFunction: {
      std::string name;
      GenContext fctx = ctx;
      std::string text = FunctionExpressionOrDeclaration(fctx, d, true, &name);
      if (kind == kAnnexBFunction) {
        // Only plain function declarations are allowed as an if body.
        if (text.rfind("function ", 0) != 0) return expression_statement(d);
      }
      return text;
    }
    case kClass: {
      std::string name;
      return ClassText(ctx, d, true, &name);
    }
    case kTry:
      return TryStatement(ctx, d);
    case kSwitch:
      return SwitchStatement(ctx, d);
    case kReturn:
      if (random_->Chance(25)) return "return;";
      return "return " + Fit(ExpressionImpl(ctx, d), kSeq) + ";";
    case kThrow:
      return "throw " + Fit(ExpressionImpl(ctx, d), kSeq) + ";";
    case kJump: {
      std::vector<std::string> options;
      if (ctx.loop || ctx.switch_stmt) options.push_back("break;");
      if (ctx.loop) options.push_back("continue;");
      for (const std::string& label : ctx.labels) options.push_back("break " + label + ";");
      for (const std::string& label : ctx.loop_labels) {
        options.push_back("continue " + label + ";");
      }
      return options[random_->Below(static_cast<uint32_t>(options.size()))];
    }
    case kWith:
      return "with (" + Fit(ExpressionImpl(ctx, d), kSeq) + ") " +
             SubStatement(ctx, d, Pos::kSub);
    case kDebugger:
      return "debugger;";
    default:
      return ";";
  }
}

// ---------------------------------------------------------------------------
// Functions and classes.

std::string Generator::FunctionTail(const GenContext& outer, unsigned depth,
                                    GenContext body_ctx, unsigned param_min,
                                    unsigned param_max, bool allow_rest,
                                    const std::vector<std::string>& extra_names,
                                    bool prepend_super_call) {
  (void)outer;
  body_ctx.function = true;
  body_ctx.loop = false;
  body_ctx.switch_stmt = false;
  body_ctx.labels.clear();
  body_ctx.loop_labels.clear();
  // await and yield expressions are early errors in parameter lists.
  GenContext param_ctx = body_ctx;
  param_ctx.async = false;
  param_ctx.generator = false;

  PushScope();
  for (const std::string& name : extra_names) Declare(name);
  const bool simple = random_->Chance(40);
  std::string params =
      Parameters(param_ctx, depth, simple, param_min, param_max, allow_rest);
  // "use strict" is only allowed in a function with simple parameters.
  std::string list = StatementList(body_ctx, depth, 3, simple, nullptr);
  PopScope();
  std::string body;
  if (prepend_super_call) {
    body = "super(" + ArgumentList(param_ctx, depth) + ");";
    if (!list.empty()) body += "\n" + list;
  } else {
    body = list;
  }
  return "(" + params + ") " + (body.empty() ? "{}" : "{\n" + body + "\n}");
}

std::string Generator::FunctionExpressionOrDeclaration(
    const GenContext& ctx, unsigned depth, bool declaration,
    std::string* declared_name) {
  const bool is_async = random_->Chance(25);
  const bool is_generator = random_->Chance(20);
  std::string name;
  if (declaration || random_->Chance(50)) name = FreshName(is_async ? "af" : "f");
  if (declaration) {
    Declare(name);
    *declared_name = name;
  }
  GenContext body = ctx;
  body.async = is_async;
  body.generator = is_generator;
  body.super_property = false;
  body.super_call = false;
  body.new_target = true;
  body.class_field = false;
  std::vector<std::string> extra;
  if (!declaration && !name.empty()) extra.push_back(name);
  std::string text = std::string(is_async ? "async " : "") + "function" +
                     (is_generator ? "*" : "") + (name.empty() ? "" : " " + name);
  return text + " " + FunctionTail(ctx, depth, body, 0, 3, true, extra, false);
}

std::string Generator::ArrowFunction(const GenContext& ctx, unsigned depth) {
  const bool is_async = random_->Chance(22);
  GenContext body = ctx;
  body.function = true;
  body.async = is_async;
  body.generator = false;
  body.loop = false;
  body.switch_stmt = false;
  body.labels.clear();
  body.loop_labels.clear();
  GenContext param_ctx = body;
  param_ctx.async = false;

  PushScope();
  std::string params;
  bool simple = true;
  if (random_->Chance(25)) {
    avoid_contextual_ = is_async;  // keep `async async => x` out
    std::string name = FreshName("a");
    avoid_contextual_ = false;
    Declare(name);
    params = name;
  } else {
    simple = random_->Chance(45);
    params = "(" + Parameters(param_ctx, depth, simple, 0, 3, true) + ")";
  }
  std::string text;
  if (random_->Chance(50)) {
    std::string list = StatementList(body, depth, 3, simple, nullptr);
    text = list.empty() ? "{}" : "{\n" + list + "\n}";
  } else {
    text = Fit(ExpressionImpl(body, depth), kAssign);
    if (text[0] == '{') text = "(" + text + ")";
  }
  PopScope();
  return std::string(is_async ? "async " : "") + params + " => " + text;
}

std::string Generator::ClassText(const GenContext& ctx, unsigned depth,
                                 bool declaration, std::string* declared_name) {
  GenContext outer = ctx;
  outer.strict = true;  // The whole class, including heritage, is strict.
  std::string name;
  if (declaration || random_->Chance(50)) name = FreshName("C");
  if (declaration) {
    Declare(name);
    *declared_name = name;
  }
  std::string heritage;
  // `super` is not allowed in a class heritage, even inside a method.
  GenContext heritage_ctx = outer;
  heritage_ctx.super_property = false;
  heritage_ctx.super_call = false;
  // V8 also rejects yield, await, and new.target in heritages and computed
  // class keys; keep those positions plain.
  heritage_ctx.generator = false;
  heritage_ctx.async = false;
  heritage_ctx.new_target = false;
  if (random_->Chance(35)) {
    std::string visible;
    const unsigned pick = random_->Below(100);
    if (pick < 30) {
      heritage = std::string(random_->Pick(kGlobals));
    } else if (pick < 40) {
      heritage = "null";
    } else if (pick < 55 && VisibleName(&visible, true)) {
      heritage = visible;
    } else {
      heritage = Fit(ExpressionImpl(heritage_ctx, depth), kLHS);
    }
  }
  const bool derived = !heritage.empty();

  PushScope();
  if (!declaration && !name.empty()) Declare(name);

  GenContext body = outer;
  body.labels.clear();
  body.loop_labels.clear();
  body.loop = false;
  body.switch_stmt = false;
  struct PrivateMember {
    std::string name;
    unsigned kind;  // 0 field, 1 method, 2 getter, 3 setter, 4 accessor pair
    bool is_static;
  };
  std::vector<PrivateMember> privates;
  if (random_->Chance(40)) {
    const unsigned count = 1 + random_->Below(3);
    for (unsigned i = 0; i < count; ++i) {
      PrivateMember member{"#" + FreshName("q"), random_->Below(5),
                           random_->Chance(25)};
      body.private_names.push_back(member.name);
      privates.push_back(member);
    }
  }

  GenContext field = body;
  field.function = false;
  field.async = false;
  field.generator = false;
  field.super_property = true;
  field.super_call = false;
  field.new_target = true;
  field.class_field = true;

  auto method_ctx = [&](bool is_async, bool is_generator, bool super_call) {
    GenContext m = body;
    m.function = true;
    m.async = is_async;
    m.generator = is_generator;
    m.super_property = true;
    m.super_call = super_call;
    m.new_target = true;
    m.class_field = false;
    return m;
  };
  auto key = [&](bool is_static, bool is_field) -> std::string {
    const unsigned pick = random_->Below(100);
    if (pick < 62) {
      for (;;) {
        std::string k = PropertyName();
        if (k == "constructor") continue;
        if (is_static && k == "prototype") continue;
        if (is_field && (k == "get" || k == "set" || k == "static" ||
                         k == "async" || k == "accessor")) {
          continue;
        }
        return k;
      }
    }
    if (pick < 76) return "'k" + std::to_string(random_->Below(4)) + "'";
    if (pick < 86) {
      static constexpr std::string_view kKeys[] = {"0", "1", "1.5", "0x1", "1e3", "2n"};
      return std::string(random_->Pick(kKeys));
    }
    return "[" + Fit(ExpressionImpl(heritage_ctx, depth), kAssign) + "]";
  };

  std::vector<std::string> members;
  auto emit_method = [&](const std::string& member_name, bool is_static,
                         unsigned accessor) {
    // accessor: 0 normal method, 1 getter, 2 setter
    bool is_async = false;
    bool is_generator = false;
    if (accessor == 0) {
      is_async = random_->Chance(22);
      is_generator = random_->Chance(20);
    }
    std::string head = is_static ? "static " : "";
    if (is_async) head += "async ";
    if (is_generator) head += "*";
    if (accessor == 1) head += "get ";
    if (accessor == 2) head += "set ";
    const unsigned pmin = accessor == 2 ? 1 : 0;
    const unsigned pmax = accessor == 1 ? 0 : accessor == 2 ? 1 : 3;
    members.push_back(head + member_name +
                      FunctionTail(outer, depth,
                                   method_ctx(is_async, is_generator, false),
                                   pmin, pmax, accessor == 0, {}, false));
  };
  auto emit_field = [&](const std::string& member_name, bool is_static) {
    std::string text = (is_static ? "static " : "") + member_name;
    if (random_->Chance(70)) text += " = " + Fit(ExpressionImpl(field, depth), kAssign);
    members.push_back(text + ";");
  };

  for (const PrivateMember& member : privates) {
    switch (member.kind) {
      case 0:
        emit_field(member.name, member.is_static);
        break;
      case 1:
        emit_method(member.name, member.is_static, 0);
        break;
      case 2:
        emit_method(member.name, member.is_static, 1);
        break;
      case 3:
        emit_method(member.name, member.is_static, 2);
        break;
      default:
        emit_method(member.name, member.is_static, 1);
        emit_method(member.name, member.is_static, 2);
        break;
    }
  }

  bool constructor_emitted = false;
  const unsigned count = random_->Below(4);
  for (unsigned i = 0; i < count; ++i) {
    if (!Spend()) break;
    const unsigned pick = random_->Below(100);
    const bool is_static = random_->Chance(25);
    if (pick < 38) {
      emit_method(key(is_static, false), is_static, 0);
    } else if (pick < 58) {
      emit_field(key(is_static, true), is_static);
    } else if (pick < 66) {
      emit_method(key(is_static, false), is_static, 1);
    } else if (pick < 74) {
      emit_method(key(is_static, false), is_static, 2);
    } else if (pick < 88) {
      members.push_back("static " + Block(field, depth, 3));
    } else if (!constructor_emitted) {
      constructor_emitted = true;
      const std::string ctor_name = random_->Chance(20) ? "'constructor'" : "constructor";
      members.push_back(ctor_name +
                        FunctionTail(outer, depth, method_ctx(false, false, derived),
                                     0, 3, true, {}, derived && random_->Chance(60)));
    } else {
      members.push_back(";");
    }
  }
  // Shuffle members so private declarations are not always first.
  for (size_t n = members.size(); n > 1; --n) {
    std::swap(members[n - 1], members[random_->Below(static_cast<uint32_t>(n))]);
  }
  PopScope();

  std::string text = "class";
  if (!name.empty()) text += " " + name;
  if (derived) text += " extends " + heritage;
  if (members.empty()) return text + " {}";
  return text + " {\n" + Join(members, "\n") + "\n}";
}

std::string Generator::ObjectLiteral(const GenContext& ctx, unsigned depth) {
  std::vector<std::string> parts;
  bool proto_used = false;
  const unsigned count = random_->Below(5);
  auto key = [&]() -> std::string {
    const unsigned pick = random_->Below(100);
    if (pick < 60) return PropertyName();
    if (pick < 75) return StringLiteral(ctx.strict);
    if (pick < 85) return NumberLiteral(true);
    return "[" + Fit(ExpressionImpl(ctx, depth), kAssign) + "]";
  };
  auto method_ctx = [&](bool is_async, bool is_generator) {
    GenContext m = ctx;
    m.async = is_async;
    m.generator = is_generator;
    m.super_property = true;
    m.super_call = false;
    m.new_target = true;
    m.class_field = false;
    return m;
  };
  for (unsigned i = 0; i < count; ++i) {
    if (!Spend()) break;
    const unsigned pick = random_->Below(100);
    std::string name;
    if (pick < 35) {
      parts.push_back(key() + ": " + Fit(ExpressionImpl(ctx, depth), kAssign));
    } else if (pick < 48 && VisibleName(&name, false)) {
      parts.push_back(name);
    } else if (pick < 63) {
      const bool is_async = random_->Chance(22);
      const bool is_generator = random_->Chance(20);
      std::string head = std::string(is_async ? "async " : "") +
                         (is_generator ? "*" : "");
      parts.push_back(head + key() +
                      FunctionTail(ctx, depth, method_ctx(is_async, is_generator),
                                   0, 3, true, {}, false));
    } else if (pick < 70) {
      parts.push_back("get " + key() +
                      FunctionTail(ctx, depth, method_ctx(false, false), 0, 0,
                                   false, {}, false));
    } else if (pick < 77) {
      parts.push_back("set " + key() +
                      FunctionTail(ctx, depth, method_ctx(false, false), 1, 1,
                                   false, {}, false));
    } else if (pick < 87) {
      parts.push_back("..." + Fit(ExpressionImpl(ctx, depth), kAssign));
    } else if (!proto_used) {
      proto_used = true;
      parts.push_back("__proto__: " + Fit(ExpressionImpl(ctx, depth), kAssign));
    }
  }
  if (parts.empty()) return "{}";
  return "{" + Join(parts, ", ") + (random_->Chance(10) ? ",}" : "}");
}

std::string Generator::TemplateLiteral(const GenContext& ctx, unsigned depth,
                                       bool tagged) {
  std::string out = "`" + TemplateText(tagged);
  const unsigned substitutions = random_->Below(4);
  for (unsigned i = 0; i < substitutions; ++i) {
    out += "${" + Fit(ExpressionImpl(ctx, depth), kSeq) + "}";
    out += TemplateText(tagged);
  }
  return out + "`";
}

// ---------------------------------------------------------------------------
// Expressions.

Expr Generator::Atom(const GenContext& ctx) {
  // In field initializers and static blocks `this` is the class or instance
  // being defined, the binding those contexts exist to exercise.
  if (ctx.class_field && random_->Chance(25)) return {"this", kPrimary, 0};
  const unsigned pick = random_->Below(100);
  std::string name;
  if (pick < 28 && VisibleName(&name, true)) return {name, kPrimary, 0};
  if (pick < 36 && !seed_.expressions.empty()) {
    return {"(" + seed_.expressions[random_->Below(
                      static_cast<uint32_t>(seed_.expressions.size()))] + ")",
            kPrimary, 0};
  }
  if (pick < 39 && !seed_.blocks.empty()) {
    return {"(" + seed_.blocks[random_->Below(
                      static_cast<uint32_t>(seed_.blocks.size()))] + ")",
            kPrimary, 0};
  }
  if (pick < 54) return {NumberLiteral(ctx.strict), kPrimary, kNumber};
  if (pick < 64) return {StringLiteral(ctx.strict), kPrimary, 0};
  if (pick < 68) return {"`" + TemplateText(false) + "`", kPrimary, 0};
  if (pick < 74) return {RegexLiteral(), kPrimary, 0};
  if (pick < 80) {
    static constexpr std::string_view kConstants[] = {"true", "false", "null"};
    return {std::string(random_->Pick(kConstants)), kPrimary, 0};
  }
  if (pick < 85) return {"this", kPrimary, 0};
  if (pick < 87 && !ctx.class_field) return {"arguments", kPrimary, 0};
  if (pick < 89 && ctx.new_target) return {"new.target", kPrimary, 0};
  if (pick < 92 && ctx.super_property) {
    if (random_->Chance(50)) return {"super." + PropertyName(), kLHS, 0};
    return {"super[" + StringLiteral(true) + "]", kLHS, 0};
  }
  return {std::string(random_->Pick(kGlobals)), kPrimary, 0};
}

std::string Generator::ArgumentList(const GenContext& ctx, unsigned depth) {
  const unsigned count = random_->Below(4);
  std::vector<std::string> args;
  for (unsigned i = 0; i < count; ++i) {
    std::string arg = Fit(ExpressionImpl(ctx, depth), kAssign);
    if (random_->Chance(10)) arg = "..." + arg;
    args.push_back(arg);
  }
  std::string out = Join(args, ", ");
  if (!args.empty() && random_->Chance(5)) out += ",";
  return out;
}

Expr Generator::Binary(const GenContext& ctx, unsigned depth) {
  struct Op {
    std::string_view text;
    int prec;
  };
  static constexpr Op kOps[] = {
      {"**", kExp},  {"*", kMul},   {"/", kMul},    {"%", kMul},
      {"+", kAdd},   {"-", kAdd},   {"<<", kShift}, {">>", kShift},
      {">>>", kShift}, {"<", kRel}, {">", kRel},    {"<=", kRel},
      {">=", kRel},  {"instanceof", kRel}, {"in", kRel}, {"==", kEq},
      {"!=", kEq},   {"===", kEq},  {"!==", kEq},   {"&", kBitAnd},
      {"^", kBitXor}, {"|", kBitOr}, {"&&", kAnd},  {"||", kOr},
      {"??", kOr}};
  const Op& op = random_->Pick(kOps);
  Expr left = ExpressionImpl(ctx, depth);
  Expr right = ExpressionImpl(ctx, depth);
  std::string l;
  std::string r;
  unsigned flags = 0;
  if (op.text == "**") {
    l = Fit(left, kUpdate);  // -a ** b is a SyntaxError
    r = Fit(right, kExp);
  } else if (op.text == "??") {
    l = (left.prec == kOr && (left.flags & kCoalesce)) ? Fit(left, kOr)
                                                       : Fit(left, kBitOr);
    r = Fit(right, kBitOr);
    flags = kCoalesce;
  } else if (op.text == "||") {
    l = Fit(left, kOr, kCoalesce);
    r = Fit(right, kAnd, kCoalesce);
    flags = kLogical;
  } else if (op.text == "&&") {
    l = Fit(left, kAnd, kCoalesce);
    r = Fit(right, kBitOr);
    flags = kLogical;
  } else {
    l = Fit(left, op.prec);
    r = Fit(right, op.prec + 1);
  }
  return {l + " " + std::string(op.text) + " " + r, op.prec, flags};
}

Expr Generator::Unary(const GenContext& ctx, unsigned depth) {
  static constexpr std::string_view kOps[] = {"!", "~", "-", "+", "typeof ",
                                              "void ", "delete "};
  const unsigned pick = random_->Below(100);
  if (pick < 15 && ctx.async) {
    return {"await " + Fit(ExpressionImpl(ctx, depth), kUnary), kUnary, 0};
  }
  if (pick < 30) {
    std::string target;
    if (random_->Chance(60) && AssignableName(&target)) {
    } else {
      Expr object = ExpressionImpl(ctx, depth);
      target = Fit(object, kLHS, kNumber | kOptional) + "." + PropertyName();
    }
    const std::string op = random_->Chance(50) ? "++" : "--";
    if (random_->Chance(50)) return {op + target, kUpdate, 0};
    return {target + op, kUpdate, 0};
  }
  const std::string_view op = random_->Pick(kOps);
  std::string operand;
  if (op == "delete ") {
    Expr inner = ExpressionImpl(ctx, depth);
    if (ctx.strict || (inner.flags & kPrivate)) {
      // `delete x` and `delete this.#x` are SyntaxErrors in strict code.
      operand = Fit(inner, kLHS, kNumber | kPrivate) + "." + PropertyName();
    } else {
      operand = Fit(inner, kUnary, kPrivate);
    }
  } else {
    operand = Fit(ExpressionImpl(ctx, depth), kUnary);
    if ((op == "-" || op == "+") && !operand.empty() && operand[0] == op[0]) {
      operand = " " + operand;
    }
  }
  return {std::string(op) + operand, kUnary, 0};
}

Expr Generator::Assignment(const GenContext& ctx, unsigned depth) {
  static constexpr std::string_view kOps[] = {
      "=",  "=",   "=",  "+=",  "-=",  "*=",  "/=",  "%=",   "**=",
      "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?="};
  const std::string_view op = random_->Pick(kOps);
  std::string target;
  const unsigned pick = random_->Below(100);
  auto member_target = [&]() {
    Expr object = ExpressionImpl(ctx, depth);
    if (random_->Chance(50)) {
      return Fit(object, kLHS, kNumber | kOptional) + "." + PropertyName();
    }
    return Fit(object, kLHS, kNumber | kOptional) + "[" +
           Fit(ExpressionImpl(ctx, depth), kSeq) + "]";
  };
  auto simple_target = [&]() {
    std::string name;
    if (random_->Chance(70) && AssignableName(&name)) return name;
    return member_target();
  };
  if (op == "=" && pick < 15 && depth > 0) {
    // Destructuring assignment.
    std::vector<std::string> parts;
    const unsigned count = random_->Below(4);
    const bool object = random_->Chance(50);
    for (unsigned i = 0; i < count; ++i) {
      std::string element;
      std::string name;
      if (object && random_->Chance(40) && VisibleName(&name, false)) {
        element = name;
      } else if (object) {
        element = PropertyName() + ": " + simple_target();
      } else if (random_->Chance(15)) {
        parts.push_back("");
        continue;
      } else {
        element = simple_target();
      }
      if (random_->Chance(25)) {
        element += " = " + Fit(ExpressionImpl(ctx, depth), kAssign);
      }
      parts.push_back(element);
    }
    if (random_->Chance(15)) {
      parts.push_back("..." + simple_target());
    } else if (!object && !parts.empty() && parts.back().empty()) {
      parts.push_back(simple_target());
    }
    target = object ? "{" + Join(parts, ", ") + "}" : "[" + Join(parts, ", ") + "]";
  } else {
    target = simple_target();
  }
  return {target + " " + std::string(op) + " " +
              Fit(ExpressionImpl(ctx, depth), kAssign),
          kAssign, 0};
}

Expr Generator::Call(const GenContext& ctx, unsigned depth) {
  const unsigned pick = random_->Below(100);
  if (pick < 8 && ctx.super_call) {
    return {"super(" + ArgumentList(ctx, depth) + ")", kLHS, kCall};
  }
  if (pick < 14) {
    return {"import(" + Fit(ExpressionImpl(ctx, depth), kAssign) + ")", kLHS, kCall};
  }
  Expr callee = ExpressionImpl(ctx, depth);
  if (pick < 26) {
    // Tagged templates may not be part of an optional chain.
    return {Fit(callee, kLHS, kOptional) + TemplateLiteral(ctx, depth, true), kLHS, 0};
  }
  if (pick < 40) {
    std::string text = "new " + Fit(callee, kLHS, kOptional | kCall);
    if (random_->Chance(20)) return {text, kUpdate, 0};
    return {text + "(" + ArgumentList(ctx, depth) + ")", kLHS, 0};
  }
  std::string text = Fit(callee, kLHS);
  const bool kept_chain = text == callee.text;
  unsigned flags = kCall | (kept_chain ? (callee.flags & kOptional) : 0);
  if (random_->Chance(15)) {
    flags |= kOptional;
    return {text + "?.(" + ArgumentList(ctx, depth) + ")", kLHS, flags};
  }
  return {text + "(" + ArgumentList(ctx, depth) + ")", kLHS, flags};
}

Expr Generator::Member(const GenContext& ctx, unsigned depth) {
  Expr object = ExpressionImpl(ctx, depth);
  std::string base = Fit(object, kLHS, kNumber);
  const bool kept_chain = base == object.text;
  // kCall stays so that `new` parenthesizes `import(x).y` and `super(x).y`.
  unsigned flags = kept_chain ? (object.flags & (kOptional | kCall)) : 0;
  const bool optional = random_->Chance(18);
  if (optional) flags |= kOptional;
  const std::string dot = optional ? "?." : ".";
  const unsigned pick = random_->Below(100);
  if (pick < 15 && !ctx.private_names.empty()) {
    return {base + dot + ctx.private_names[random_->Below(
                             static_cast<uint32_t>(ctx.private_names.size()))],
            kLHS, flags | kPrivate};
  }
  if (pick < 65) return {base + dot + PropertyName(), kLHS, flags};
  return {base + (optional ? "?.[" : "[") + Fit(ExpressionImpl(ctx, depth), kSeq) + "]",
          kLHS, flags};
}

Expr Generator::ExpressionImpl(const GenContext& ctx, unsigned depth) {
  if (depth == 0 || !Spend()) return Atom(ctx);
  const unsigned d = depth - 1;
  enum Kind {
    kAtom, kBinary, kCall, kMember, kArray, kObject, kTemplate, kUnary,
    kConditional, kAssignment, kSequence, kArrow, kFunction, kClass, kYield,
    kPrivateIn, kCount
  };
  unsigned weights[kCount] = {};
  weights[kAtom] = 16;
  weights[kBinary] = 14;
  weights[kCall] = 10;
  weights[kMember] = 9;
  weights[kArray] = 4;
  weights[kObject] = 6;
  weights[kTemplate] = 3;
  weights[kUnary] = 7;
  weights[kConditional] = 3;
  weights[kAssignment] = 6;
  weights[kSequence] = 1;
  weights[kArrow] = 6;
  weights[kFunction] = 3;
  weights[kClass] = 4;
  weights[kYield] = ctx.generator ? 4 : 0;
  weights[kPrivateIn] = ctx.private_names.empty() ? 0 : 2;
  unsigned total = 0;
  for (unsigned w : weights) total += w;
  unsigned roll = random_->Below(total);
  unsigned kind = 0;
  while (roll >= weights[kind]) roll -= weights[kind++];

  switch (kind) {
    case kAtom:
      return Atom(ctx);
    case kBinary:
      return Binary(ctx, d);
    case kCall:
      return Call(ctx, d);
    case kMember:
      return Member(ctx, d);
    case kArray: {
      std::vector<std::string> elements;
      const unsigned count = random_->Below(4);
      for (unsigned i = 0; i < count; ++i) {
        if (random_->Chance(12)) {
          elements.push_back("");
          continue;
        }
        std::string element = Fit(ExpressionImpl(ctx, d), kAssign);
        if (random_->Chance(10)) element = "..." + element;
        elements.push_back(element);
      }
      if (!elements.empty() && elements.back().empty()) elements.push_back("0");
      return {"[" + Join(elements, ", ") + "]", kPrimary, 0};
    }
    case kObject:
      return {ObjectLiteral(ctx, d), kPrimary, 0};
    case kTemplate:
      return {TemplateLiteral(ctx, d, false), kPrimary, 0};
    case kUnary:
      return Unary(ctx, d);
    case kConditional: {
      std::string test = Fit(ExpressionImpl(ctx, d), kOr);
      std::string then = Fit(ExpressionImpl(ctx, d), kAssign);
      std::string otherwise = Fit(ExpressionImpl(ctx, d), kAssign);
      return {test + " ? " + then + " : " + otherwise, kCond, 0};
    }
    case kAssignment:
      return Assignment(ctx, d);
    case kSequence:
      return {Fit(ExpressionImpl(ctx, d), kSeq) + ", " +
                  Fit(ExpressionImpl(ctx, d), kAssign),
              kSeq, 0};
    case kArrow:
      return {ArrowFunction(ctx, d), kAssign, 0};
    case kFunction: {
      std::string ignored;
      return {FunctionExpressionOrDeclaration(ctx, d, false, &ignored), kPrimary, 0};
    }
    case kClass: {
      std::string ignored;
      return {ClassText(ctx, d, false, &ignored), kPrimary, 0};
    }
    case kYield: {
      const unsigned pick = random_->Below(100);
      if (pick < 20) return {"yield", kAssign, 0};
      const std::string head = pick < 40 ? "yield* " : "yield ";
      return {head + Fit(ExpressionImpl(ctx, d), kAssign), kAssign, 0};
    }
    default: {
      const std::string& name = ctx.private_names[random_->Below(
          static_cast<uint32_t>(ctx.private_names.size()))];
      return {name + " in " + Fit(ExpressionImpl(ctx, d), kShift), kRel, 0};
    }
  }
}

// ---------------------------------------------------------------------------
// Entry points. Each retries with a smaller budget and depth until the text
// fits; the sequence of attempts depends only on the random stream.

bool Generator::Program(std::string* out, unsigned depth,
                        unsigned max_statements) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    Reset();
    if (attempt != 0) nodes_left_ >>= attempt;
    GenContext ctx;
    std::string text;
    if (random_->Chance(3)) text = "#!/usr/bin/env node\n";
    std::string list;
    for (unsigned tries = 0; tries < 4 && list.empty(); ++tries) {
      list = StatementList(ctx, depth > attempt ? depth - attempt : 1,
                           std::max(1u, max_statements), true, nullptr);
    }
    if (list.empty()) list = ";";
    text += list;
    if (text.size() <= limit_) {
      *out = std::move(text);
      return true;
    }
  }
  out->clear();
  return false;
}

bool Generator::Statement(std::string* out, const GenContext& context,
                          unsigned depth) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    Reset();
    if (attempt != 0) nodes_left_ >>= attempt;
    std::string text = StatementImpl(context, depth > attempt ? depth - attempt : 1,
                                     Pos::kList);
    if (!text.empty() && text.size() <= limit_) {
      *out = std::move(text);
      return true;
    }
  }
  out->clear();
  return false;
}

bool Generator::Expression(std::string* out, const GenContext& context,
                           unsigned depth) {
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    Reset();
    if (attempt != 0) nodes_left_ >>= attempt;
    Expr e = ExpressionImpl(context, depth > attempt ? depth - attempt : 0);
    std::string text = "(" + e.text + ")";
    if (text.size() <= limit_) {
      *out = std::move(text);
      return true;
    }
  }
  out->clear();
  return false;
}

std::string Generator::Literal(std::string_view type, bool strict, bool tagged) {
  if (type == "number") return NumberLiteral(strict);
  if (type == "string") return StringLiteral(strict);
  if (type == "regex") return RegexLiteral();
  if (type == "template_string") return "`" + TemplateText(tagged) + "`";
  if (type == "true") return "false";
  if (type == "false") return "true";
  if (type == "null") return random_->Chance(50) ? "0" : "undefined";
  return {};
}

}  // namespace internal
}  // namespace js_mutator
