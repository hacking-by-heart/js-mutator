// Interface contract: limits, determinism, fallback, validity, crossover.
#include "test_util.h"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "generator.h"

using js_mutator::EditKind;
using js_mutator::Outcome;
using test::Require;

namespace {

namespace internal = js_mutator::internal;

std::string Utf8(uint32_t cp) {
  std::string out;
  internal::AppendUtf8(&out, cp);
  return out;
}

bool HasNonAscii(const std::string& text) {
  for (char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80) return true;
  }
  return false;
}

// Every Unicode table entry goes through Tree-sitter and the early-error
// checker, raw and escaped; a raw and an escaped spelling are one name.
void TestUnicodeTables(test::Syntax& syntax) {
  Require(internal::CanonicalName("\\u0061b\\u{63}") == "abc", "escapes are not decoded");
  Require(internal::CanonicalName("\\u{1d400}") == Utf8(0x1D400), "astral escape");
  auto check_name = [&](const std::string& raw, const std::string& escaped) {
    const std::string decl = "let " + raw + " = 1; " + raw + ";";
    Require(syntax.Parses(decl) && !internal::HasEarlyErrorForTesting(decl),
            "identifier rejected: " + decl);
    unsigned matches = 0;
    syntax.Visit(decl, [&](TSNode node, std::string_view source) {
      if (std::strcmp(ts_node_type(node), "identifier") != 0) return;
      const uint32_t start = ts_node_start_byte(node);
      if (source.substr(start, ts_node_end_byte(node) - start) == raw) ++matches;
    });
    Require(matches == 2, "Tree-sitter split the identifier: " + decl);
    const std::string redeclared = "let " + raw + " = 1; let " + escaped + " = 2;";
    Require(syntax.Parses(redeclared) && internal::HasEarlyErrorForTesting(redeclared),
            "raw and escaped spellings must be one name: " + redeclared);
    const std::string field =
        "class K { #" + raw + " = 1; m() { return this.#" + escaped + "; } }";
    Require(syntax.Parses(field) && !internal::HasEarlyErrorForTesting(field),
            "private name rejected: " + field);
    const std::string twice = "class K { #" + raw + "; #" + escaped + "; }";
    Require(internal::HasEarlyErrorForTesting(twice), "duplicate private name: " + twice);
  };
  for (uint32_t cp : internal::kUnicodeIdStart) {
    check_name(Utf8(cp) + "a", internal::EscapeCodePoint(cp, false) + "a");
  }
  for (uint32_t cp : internal::kUnicodeIdStartAstral) {
    check_name(Utf8(cp) + "a", internal::EscapeCodePoint(cp, true) + "a");
  }
  for (uint32_t cp : internal::kUnicodeIdContinue) {
    check_name("a" + Utf8(cp), "a" + internal::EscapeCodePoint(cp, true));
  }
  for (uint32_t cp : internal::kUnicodeWhiteSpace) {
    const std::string source = "a" + Utf8(cp) + "=" + Utf8(cp) + "1;";
    Require(syntax.Parses(source), "whitespace rejected: " + source);
  }
  for (uint32_t cp : internal::kUnicodeLineTerminator) {
    const std::string source = "a = 1" + Utf8(cp) + "b = 2";
    Require(syntax.Parses(source), "line terminator rejected: " + source);
  }
}

// Raw non-ASCII output stays a modest share; identifiers only ever hold
// ID_Start/ID_Continue characters, and regexps never a line terminator.
void TestRawUnicode(test::Syntax& syntax) {
  unsigned raw = 0;
  const unsigned total = 3000;
  for (uint32_t seed = 1; seed <= total; ++seed) {
    const std::string source = test::Generate(2048, seed).source;
    if (!HasNonAscii(source)) continue;
    ++raw;
    syntax.Visit(source, [&](TSNode node, std::string_view text) {
      const char* type = ts_node_type(node);
      const uint32_t start = ts_node_start_byte(node);
      const std::string value(text.substr(start, ts_node_end_byte(node) - start));
      const bool identifier = std::strcmp(type, "identifier") == 0 ||
                              std::strcmp(type, "property_identifier") == 0 ||
                              std::strcmp(type, "private_property_identifier") == 0;
      if (identifier) {
        for (uint32_t cp : internal::kUnicodeLookAlike) {
          Require(value.find(Utf8(cp)) == std::string::npos, "look-alike in " + value + "\n" + source);
        }
        for (uint32_t cp : internal::kUnicodeWhiteSpace) {
          Require(value.find(Utf8(cp)) == std::string::npos, "whitespace in " + value);
        }
      }
      if (std::strcmp(type, "regex") == 0) {
        for (uint32_t cp : internal::kUnicodeLineTerminator) {
          Require(value.find(Utf8(cp)) == std::string::npos, "terminator in " + value);
        }
      }
    });
  }
  Require(raw * 100 >= total * 5 && raw * 100 <= total * 18,
          "raw non-ASCII share is off: " + std::to_string(raw));
}

// The numeric grammar, numeric/BigInt property keys, and long literals.
void TestLiterals(test::Syntax& syntax) {
  std::set<std::string> seen;
  auto note = [&](const std::string& what) { seen.insert(what); };
  for (uint32_t seed = 1; seed <= 3000; ++seed) {
    const std::string source = test::Generate(2048, seed).source;
    syntax.Visit(source, [&](TSNode node, std::string_view text) {
      const char* type = ts_node_type(node);
      const uint32_t start = ts_node_start_byte(node);
      const std::string value(text.substr(start, ts_node_end_byte(node) - start));
      TSNode parent = ts_node_parent(node);
      if (std::strcmp(type, "number") == 0) {
        if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) note("hex");
        if (value.rfind("0o", 0) == 0 || value.rfind("0O", 0) == 0) note("octal");
        if (value.rfind("0b", 0) == 0 || value.rfind("0B", 0) == 0) note("binary");
        if (value.back() == 'n') note("bigint");
        if (value.find('_') != std::string::npos) note("separator");
        if (value.find("e-3") != std::string::npos) note("denormal");
        if (value[0] == '.') note("fraction");
        if (value.size() >= 100) note("long number");
        if (value.size() == 11 || value.size() == 10) note("smi edge");
        if (!ts_node_is_null(parent) && (std::strcmp(ts_node_type(parent), "pair") == 0 ||
                                         std::strcmp(ts_node_type(parent), "method_definition") == 0 ||
                                         std::strcmp(ts_node_type(parent), "field_definition") == 0)) {
          note("numeric key");
          if (value.back() == 'n') note("bigint key");
        }
      }
      if (std::strcmp(type, "string") == 0 || std::strcmp(type, "template_string") == 0) {
        if (value.size() >= 250 && value.size() <= 262) note("long 256");
        if (value.size() >= 1020 && value.size() <= 1100) note("long 1024");
        if (value.size() >= 120 && value.find("\\u") != std::string::npos) note("two-byte unit");
      }
      if (std::strcmp(type, "unary_expression") == 0 && (value.rfind("-~", 0) == 0 ||
                                                         value.rfind("~-", 0) == 0)) {
        note("fold chain");
      }
    });
  }
  for (const char* what : {"hex", "octal", "binary", "bigint", "separator", "denormal",
                           "fraction", "long number", "smi edge", "numeric key", "bigint key",
                           "long 256", "long 1024", "two-byte unit", "fold chain"}) {
    Require(seen.count(what) != 0, std::string("literals never produced: ") + what);
  }
}

// kMagicComment rebuilds its comments rather than accumulating them.
void TestMagicComments(test::Syntax& syntax) {
  std::set<std::string> names;
  std::string parent = "#!/usr/bin/env d8\nlet a = 1;\nfunction f() { return a; }";
  unsigned edits = 0;
  for (uint32_t seed = 1; seed <= 6000; ++seed) {
    const auto result = test::Mutate(parent, 4096, seed);
    if (result.edit != EditKind::kMagicComment) continue;
    ++edits;
    Require(syntax.Parses(result.source), "magic comment broke the program");
    Require(result.source.rfind("#!/usr/bin/env d8\n", 0) == 0, "hashbang moved");
    unsigned lines = 0;
    for (size_t at = 0; (at = result.source.find("//", at)) != std::string::npos; at += 2) {
      const char sign = at + 2 < result.source.size() ? result.source[at + 2] : 0;
      if (sign == '#' || sign == '@') ++lines;
    }
    Require(lines >= 1 && lines <= 5, "magic comments accumulate");
    for (const char* name : {"sourceURL=", "sourceMappingURL=", "debugId=",
                             "allFunctionsCalledOnLoad", "functionsCalledOnLoad="}) {
      if (result.source.find(name) != std::string::npos) names.insert(name);
    }
    parent = result.source;  // the next rebuild must replace this one
  }
  Require(edits >= 50, "kMagicComment is rarely chosen");
  Require(names.size() == 5, "some magic comment kinds never appear");
}

}  // namespace

int main() {
  test::Syntax syntax;
  const std::vector<std::string> parents = {
      "const price = 5;",
      "function total(price, qty) { return price * qty; }\ntotal(2, 3);",
      "class Item { #id = 1; get id() { return this.#id; } static { this.n = 0; } }",
      "async function* read(xs) { for await (const x of xs) yield x; }",
      "label: for (let i = 0; i < 3; i++) { if (i) continue label; else break; }",
      "const {a, b: [c = 1, ...d]} = obj, s = `x${a}y`, r = /a(?<n>b)\\k<n>/gu;",
      "\"use strict\";\nlet x = 1;\nx ||= y ?? 2;",
  };

  // MaxSize = 0 returns nothing; small limits are always honored.
  Require(test::Generate(0, 1).source.empty(), "MaxSize=0 must return zero bytes");
  for (size_t limit = 1; limit <= 16; ++limit) {
    for (uint32_t seed = 1; seed <= 32; ++seed) {
      Require(test::Generate(limit, seed).source.size() <= limit,
              "generation exceeded a small MaxSize");
      for (const auto& parent : parents) {
        Require(test::Mutate(parent, limit, seed).source.size() <= limit,
                "mutation exceeded a small MaxSize");
      }
    }
  }
  Require(test::Mutate(";", 1, 1).source == ";", "a one-byte limit returns the fallback");
  Require(test::Mutate("", 128, 1, 0).outcome == Outcome::kFallback,
          "malformed mode on an empty parent falls back");

  // Determinism and exact-size replay.
  for (uint32_t seed = 1; seed <= 64; ++seed) {
    const auto generated = test::Generate(2048, seed);
    Require(generated.outcome == Outcome::kGeneratedValidLeaning,
            "empty input must generate a program");
    Require(test::Generate(2048, seed).source == generated.source,
            "generation is not deterministic");
    Require(test::Generate(generated.source.size(), seed).source == generated.source,
            "generation must replay at its exact size");
    for (const auto& parent : parents) {
      Require(test::Mutate(parent, 4096, seed).source ==
                  test::Mutate(parent, 4096, seed).source,
              "mutation is not deterministic");
    }
  }

  // Valid-leaning output always passes the syntax check and keeps the parent's
  // vocabulary; no edit kind returns the old fixed snippets.
  for (const auto& parent : parents) {
    std::set<std::string> distinct;
    for (uint32_t seed = 1; seed <= 300; ++seed) {
      const auto result = test::Mutate(parent, 4096, seed);
      Require(result.outcome != Outcome::kFallback, "valid parent fell back: " + parent);
      Require(syntax.Parses(result.source), "valid-leaning output did not parse:\n" +
                                                result.source);
      Require(result.source.find("void class Example {};") == std::string::npos,
              "a fixed template snippet is still emitted");
      distinct.insert(result.source);
    }
    Require(distinct.size() >= 200, "mutations of one parent are not diverse: " + parent);
  }

  // An invalid parent is repaired or edited, never replaced by `;`.
  const std::string broken = "function f(a { return a + ; }\nlet x = [1, 2;";
  unsigned repaired = 0;
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    const auto result = test::Mutate(broken, 4096, seed);
    Require(result.source != ";", "an invalid parent became a dead-end fallback");
    if (syntax.Errors(result.source) < syntax.Errors(broken)) ++repaired;
  }
  Require(repaired >= 20, "invalid parents are rarely repaired");

  // Malformed mode breaks syntax (or injects an early error). A token-level
  // disruption may also leave only an early error, such as `await f()` in a
  // script; those stay rare.
  unsigned broken_outputs = 0;
  unsigned early_disruptions = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const auto result = test::Mutate(parents[1], 4096, seed, 0);
    Require(result.outcome == Outcome::kEditedMalformed, "malformed mode did not edit");
    if (!syntax.Parses(result.source) || result.edit == EditKind::kInjectEarlyError ||
        result.edit == EditKind::kRegexMalformed ||
        result.edit == EditKind::kFlipGoal) {
      ++broken_outputs;
    } else if (result.edit == EditKind::kDisruptSyntax) {
      ++early_disruptions;
    }
  }
  Require(broken_outputs + early_disruptions == 300, "malformed mode produced valid output");
  Require(early_disruptions <= 15, "malformed mode rarely breaks the syntax itself");

  // Regexp corruptions edit a seed literal, or add a broken one when the seed
  // has none.
  for (const std::string& parent : {parents[5], parents[1]}) {
    unsigned regex_edits = 0;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
      const auto result = test::Mutate(parent, 4096, seed, 0);
      if (result.edit != EditKind::kRegexMalformed) continue;
      ++regex_edits;
      Require(result.source != parent, "a regexp corruption changed nothing");
      Require(syntax.Types(result.source).count("regex") > 0,
              "a regexp corruption left no regexp literal:\n" + result.source);
    }
    Require(regex_edits >= 40, "regexp corruptions are rare: " + parent);
  }

  // Crossover transplants code from the donor into the parent.
  const std::string donor = "function helper(z) { return z * 3; }\nhelper(4);";
  bool transplanted = false;
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    const auto& parent = parents[1];
    const auto result = js_mutator::CrossOver(
        reinterpret_cast<const uint8_t*>(parent.data()), parent.size(),
        reinterpret_cast<const uint8_t*>(donor.data()), donor.size(), 4096, seed, {100});
    Require(syntax.Parses(result.source), "crossover output did not parse");
    if (result.edit == EditKind::kCrossOver &&
        result.source.find("helper") != std::string::npos &&
        result.source.find("total") != std::string::npos) {
      transplanted = true;
    }
  }
  Require(transplanted, "crossover never combined both inputs");

  // Scope stress: laziness, capture and direct eval edits stay valid.
  auto occurrences = [](const std::string& text, const std::string& what) {
    unsigned n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
  };
  const std::vector<std::string> scope_parents = {
      parents[1], "(function (a) { let c = a; return () => c; })(1);\nlet y = 2;\ny + 1;"};
  unsigned laziness = 0, pife = 0, nested = 0, captures = 0, evals = 0, lazy_errors = 0;
  for (uint32_t seed = 1; seed <= 600; ++seed) {
    for (const auto& parent : scope_parents) {
      const auto result = test::Mutate(parent, 4096, seed);
      if (result.edit == EditKind::kLaziness || result.edit == EditKind::kCapture) {
        Require(syntax.Parses(result.source), "scope edit did not parse:\n" + result.source);
        Require(result.outcome == Outcome::kEditedValidLeaning, "scope edit was not clean");
      }
      if (result.edit == EditKind::kLaziness) {
        ++laziness;
        if (result.source.find("var g") != std::string::npos) ++pife;
        if (occurrences(result.source, "{\n") >= occurrences(parent, "{\n") + 2) ++nested;
      }
      if (result.edit == EditKind::kCapture) ++captures;
      if (result.edit == EditKind::kGenerateStatement &&
          result.source.find("eval(") != std::string::npos) {
        ++evals;
      }
      const auto broken_result = test::Mutate(parent, 4096, seed, 0);
      if (broken_result.edit == EditKind::kInjectEarlyError &&
          (broken_result.source.find("\nvar z") != std::string::npos ||
           broken_result.source.find("let price;") != std::string::npos)) {
        ++lazy_errors;
      }
    }
  }
  Require(laziness >= 20 && pife >= 3 && nested >= 5, "laziness edits are rare");
  Require(captures >= 20, "capture edits are rare");
  Require(evals >= 10, "direct eval statements are rare");
  Require(lazy_errors >= 10, "early errors in lazy bodies are rare");
  unsigned generated_eval = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    if (test::Generate(2048, seed).source.find("eval(") != std::string::npos) ++generated_eval;
  }
  Require(generated_eval >= 20, "generation rarely calls eval");
  // The early-error checker agrees with V8 15.8 on names and targets.
  const char* const kEarlyErrors[] = {
      "let x; { var x; }", "{ var x; } let x;", "{ let x; { var x; } }",
      "{ function f(){} var f; }", "{ function f(){} { var f; } }",
      "try {} catch ([e]) { var e; }", "for (let i;;) { var i; }",
      "for (let x of []) { var x; }", "switch (1) { case 1: let x; case 2: { var x; } }",
      "class C { static { let y; { var y; } } }", "let\nx = 1; var x;",
      "(a, a) => 1;", "({ m(a, a) {} });", "class C { m() { (function (a, a) {}); } }",
      "function f(a, a) { \"use strict\"; }", "function f(a, [a]) {}",
      "function f(a, a = 1) {}", "function f(a = 1) { \"use strict\"; }",
      "function eval() { \"use strict\"; }", "function f(eval) { \"use strict\"; }",
      "let f; L: function f(){}", "try {} catch (f) { function f() {} }",
      "\"use strict\"; L: function f(){}", "\"use strict\"; if (1) function f(){}",
      "if (1) function* g(){}", "L: async function f(){}", "while (1) function f(){}",
      "{ async function f() {} function f() {} }", "\"use strict\"; var let = 1;",
      "\"use strict\"; ({static});", "let let = 1;", "try {} catch ([let]) {}",
      "async function f() { var await; }", "class C { static { var await; } }",
      "var v\\u0061r = 1;", "\"use strict\"; (f()) = 1;", "\"use strict\"; f()++;",
      "(f()) ?\?= 1;", "(f()) &&= 1;", "(f``) = 1;", "({a}) = 1;", "([a]) = 1;",
      "(new f()) = 1;", "(a?.b) = 1;", "(import(\"x\")) = 1;", "\"use strict\"; (eval) = 1;",
      "\"use strict\"; eval++;", "\"use strict\"; for ((f()) of o);",
      "{ using r = null; { var r; } }", "{ for (var r in o); using r = null; }",
      // Tree-sitter reads these blocks as object literals.
      "\"use strict\"; { L: function f() {} }", "/*M*/{ L: function f() {} }",
      "{ L: function* g() {} }", "{ L: async function g() {} }", "{ a: 1, b: 2 }",
      "() => { a: 1, b: 2 }", "{ \"a\": 1 }", "L: { L: 1 }",
      "/*M*/\nexport * as default from \"x\";\nexport default 1;", "/*M*/\n() => await 0;",
      // Strict-only errors in strict script code.
      "\"use strict\"; 010;", "function f() { \"use strict\"; 010; }",
      "class C { m() { return 010; } }", "class C { m() { with (a) {} } }",
      "\"use strict\"; with (a) {}",
      "function g() { class C { static { await (x)(); } } }",
      "class A { static { arguments } }", "class A { static { () => arguments } }",
      "class A { static { class B { [arguments] = 1 } } }", "class A { x = arguments }",
      "class A { x = class B { [arguments] = 1 } }"};
  for (const char* source : kEarlyErrors) {
    Require(js_mutator::internal::SourceHasEarlyError(source),
            std::string("checker missed an early error: ") + source);
  }
  const char* const kValid[] = {
      "try {} catch (e) { var e; }", "try {} catch (e) { for (var e in []); }",
      "try {} catch (e) { { var e; } }", "try {} catch (e) { { function e() {} } }",
      "function f(a, a) {}", "function* g(a, a) {}", "async function h(a, a) {}",
      "(function (a, b, a) { var b; });", "function f(a = 1) { var a; }",
      "let x; function g() { var x; }", "function f(x) { { let x; } var x; }",
      "if (1) function f() {}", "L: function f() {}", "let f; { function f() {} }",
      "let f; if (1) function f() {}", "function g(f) { { function f() {} } }",
      "{ function f() {} function f() {} }", "switch (1) { case 1: function f() {} }",
      "var await = 1; await + 1;", "var yield = 1; (yield) * 2;",
      "var let = 1; (let)[0] = 2;", "await: ;", "await: { break await; }",
      "if (1) let\nx = 1", "let\nx = 1; x;", "({await, yield, let, static});",
      "using\nx", "using[x];", "function f(eval, arguments, let, static) {}",
      "try {} catch (let) {}", "var \\u{0061}wait = 1;", "var a\\u{00000062}c = 1; abc;",
      "let x\\u0031 = 1; { let x1 = 2; }", "class A { get; set; static; async; accessor; }",
      "class A { static(){} static = 1; static static(){} get static() {} }",
      "class A { x = 1\ny = 2\n#z = 3\nm() {} }",
      "var x, o = {}; (x) = 1; ((o.p)) = 2; (x) += 1; (o.p) ?\?= 3; (x)++; ++(o.p);",
      "(f()) = 1;", "(f()) += 1;", "f()++;", "++f();", "for ((f()) of o);",
      "for ((x) in o);", "({ [(f())++]() { \"use strict\"; } });",
      "(function () { \"use strict\"; for (let [a] of b); });",
      "{ L: function f() {} }", "if (1) { L: function g() {} }", "{ a, b }", "{ a: 1, b }",
      "x => { L: 1 }", "/*M*/\nawait 0;", "/*M*/\nexport const k = await 1;",
      "/*M*/\n{ await 0; }", "\"use strict\"; var await = 1;",
      "class C { m() { return String.raw`\\01`; } }", "\"use strict\"; String.raw`\\1${0}`;",
      "async function g() { await (x)(); }", "async function g() { await (x)`t`; }",
      "class C { [arguments] = 1 }", "class A { static { class B { m() { arguments } } } }",
      "class A { x = function () { return arguments; } }"};
  for (const char* source : kValid) {
    Require(!js_mutator::internal::SourceHasEarlyError(source),
            std::string("checker rejected valid code: ") + source);
  }

  // Rebinding reuses visible names in new scopes, as vars, parameters, and
  // Annex B functions, and never produces an early error.
  const std::string scoped =
      "function f(a, b = () => a) { let x = a; try { g(); } catch (e) { x = e; } return x; }";
  unsigned rebinds = 0;
  bool shadowed = false;
  for (uint32_t seed = 1; seed <= 1200; ++seed) {
    const auto result = test::Mutate(scoped, 4096, seed);
    if (result.edit != EditKind::kRebind) continue;
    ++rebinds;
    Require(!js_mutator::internal::SourceHasEarlyError(result.source),
            "rebind produced an early error:\n" + result.source);
    for (const char* form : {"var a", "var b", "var x", "let x", "const a", "catch (e)",
                             "catch (x)", "function b", "function e", "(a, "}) {
      if (result.source.find(form) != std::string::npos &&
          scoped.find(form) == std::string::npos) {
        shadowed = true;
      }
    }
  }
  Require(rebinds >= 10, "rebind edits are rare");
  // An `await` label never moves into async code.
  for (uint32_t seed = 1; seed <= 600; ++seed) {
    const auto result = test::Mutate("await: {}", 4096, seed);
    if (result.edit != EditKind::kWrapStatement) continue;
    Require(result.source.find("async () => {\nawait:") == std::string::npos,
            "an await label moved into async code:\n" + result.source);
  }
  Require(shadowed, "rebind never reused a visible name");
  TestUnicodeTables(syntax);
  TestRawUnicode(syntax);
  TestLiterals(syntax);
  TestMagicComments(syntax);
  // Parameter scopes, unwrapping, range deletion, and deepening.
  const std::string program =
      "function s(a) { \"use strict\"; return a + 1; }\n"
      "function d(a, a) { return a * 2; }\n"
      "function t(b, ...r) { const k = [b, r]; return k.length ? b : -b; }\n"
      "const u = x => x + 1;\n"
      "let n = t(1, 2) + u(3) * s(4);\n"
      "n = (n, typeof n === 'number') ? n : 0;\n"
      "if (n) { n++; n--; n += 2; n -= 1; }\n";
  std::map<EditKind, unsigned> kinds;
  unsigned deep = 0;
  for (uint32_t seed = 1; seed <= 3000; ++seed) {
    const auto result = test::Mutate(program, 8192, seed);
    ++kinds[result.edit];
    if (result.edit == EditKind::kParamScope) {
      Require(result.source.find("function s(a) {") != std::string::npos &&
                  result.source.find("function d(a, a) {") != std::string::npos,
              "parameters added to a strict or duplicate-parameter function:\n" +
                  result.source);
      Require(result.source.find("...r)") != std::string::npos,
              "a parameter was added after a rest parameter:\n" + result.source);
    }
    if (result.edit == EditKind::kDeepen) {
      int level = 0;
      int max_level = 0;
      for (char c : result.source) {
        if (c == '(' || c == '[') max_level = std::max(max_level, ++level);
        if (c == ')' || c == ']') --level;
      }
      if (max_level >= 16) ++deep;
    }
  }
  for (EditKind kind : {EditKind::kParamScope, EditKind::kUnwrap, EditKind::kDeleteRange,
                        EditKind::kDeepen}) {
    Require(kinds[kind] >= 10, "a size, depth, or parameter edit is rarely applied");
  }
  Require(deep >= kinds[EditKind::kDeepen] / 2, "deepening rarely nests 16 levels");

  // Fill-aware weights: a nearly full input shrinks more often.
  auto shrink_share = [&](size_t limit) {
    unsigned shrink = 0;
    for (uint32_t seed = 1; seed <= 600; ++seed) {
      const auto edit = test::Mutate(program, limit, seed).edit;
      if (edit == EditKind::kDeleteStatement || edit == EditKind::kDeleteRange ||
          edit == EditKind::kUnwrap) {
        ++shrink;
      }
    }
    return shrink;
  };
  const unsigned full = shrink_share(program.size() + program.size() / 4);
  const unsigned roomy = shrink_share(program.size() * 8);
  Require(full > 3 * roomy, "fill does not steer edits toward shrinking");

  // Malformed mode includes cover-grammar and optional-chain errors.
  unsigned cover_errors = 0;
  for (uint32_t seed = 1; seed <= 2000; ++seed) {
    const auto result = test::Mutate(parents[1], 4096, seed, 0);
    for (const char* snippet : {"(...a, b) => 0;", "({a = 1});", "async (await) => 0;",
                                "a?.?.b;", "new a?.b();", "import(...a);"}) {
      if (result.source.find(snippet) != std::string::npos) ++cover_errors;
    }
  }
  Require(cover_errors > 0, "malformed mode never emits cover-grammar errors");
  // The early-error checker, seen through outcomes: a valid-leaning edit of a
  // clean parent is never reported malformed, while edits of a parent with an
  // early error mostly keep it. All of these were checked with V8 15.8.
  const std::vector<std::string> clean = {
      "{ using a = null; }", "function f() { using a = g(), b = null; return a; }",
      "async function f() { await using a = null; for (await using b of c) {} }",
      "for (using a of b) {}", "class K { static { using a = null; } }",
      "using[x];\nusing[x] = 1;", "using\nx;", "switch (0) { case 0: { using a = null; } }",
      "{ using a = 1; { using a = 2; } }", "async () => { for await (using a of b) ; }",
      "/*M*/\nusing a = null;\nexport { a };",
      "/*M*/\nawait using b = null;\nfor await (using c of d) ;",
      "/*M*/\nimport d, * as ns from \"/dev/null\";\nexport { d as \"x y\", ns };",
      "/*M*/\nexport default class {}\nexport * from \"/dev/null\";",
      "/*M*/\nexport { f };\nfunction f() {}", "/*M*/\n{ var v; }\nexport { v as default };",
      "/*M*/\nimport {} from \"/dev/null\" with { mode: \"x\" };\nimport.meta.url;",
      "/*M*/\nexport * as q from \"/dev/null\";\nexport { default as r, \"s t\" as u } from "
      "\"/dev/null\";",
      "/*M*/\nexport default function f() {}\nexport { f as g };",
      "class O { #a; m() { return class extends (o => o.#a, Object) { #a; [o => #a in o] = 1; "
      "}; } }"};
  const std::vector<std::string> early = {
      "using a = null;", "{ using a; }", "switch (0) { case 0: using a = null; }",
      "{ using let = 1; }", "class K { static { await using a = null; } }",
      "function f() { await using a = null; }", "{ using a = 1; var a; }", "export default 1;",
      "import a from \"/dev/null\";", "import.meta;", "/*M*/\nexport { nope };",
      "/*M*/\nexport { a as b, c as b };\nvar a, c;", "/*M*/\nvar f;\nfunction f() {}",
      "/*M*/\nwith (a) {}", "/*M*/\nvar await;", "/*M*/\n{ export {}; }",
      "/*M*/\nimport a from \"/dev/null\";\nlet a;",
      "/*M*/\nimport a from \"/dev/null\" with { k: \"1\", k: \"2\" };",
      "/*M*/\nexport default 1;\nexport default 2;", "/*M*/\nfunction f() { await 0; }",
      "/*M*/\n<!-- c\n0;", "await f();",
      "class O { #a; m() { return class extends (o => o.#b, Object) { #b; }; } }"};
  for (const auto& parent : clean) {
    Require(syntax.Parses(parent), "clean seed does not parse: " + parent);
    for (uint32_t seed = 1; seed <= 12; ++seed) {
      Require(test::Mutate(parent, 4096, seed).outcome != Outcome::kEditedMalformed,
              "the checker reports an early error in:\n" + parent);
    }
  }
  for (const auto& parent : early) {
    Require(syntax.Parses(parent), "early-error seed does not parse: " + parent);
    unsigned kept = 0;
    for (uint32_t seed = 1; seed <= 12; ++seed) {
      if (test::Mutate(parent, 4096, seed).outcome == Outcome::kEditedMalformed) ++kept;
    }
    Require(kept >= 6, "the checker misses the early error in:\n" + parent);
  }

  // kPrivateRename renames a declaration with its uses; an inner class that
  // redeclares the name keeps its own.
  const std::string classes =
      "class Outer { #a = 1; #b; static #m() {} f() { return class { #a; g(o) { return o.#a + "
      "this.#b; } }; } h() { return this.#a; } }";
  unsigned renamed = 0;
  unsigned shadow_kept = 0;
  for (uint32_t seed = 1; seed <= 600; ++seed) {
    const auto result = test::Mutate(classes, 4096, seed);
    if (result.edit != EditKind::kPrivateRename) continue;
    Require(result.outcome == Outcome::kEditedValidLeaning && syntax.Parses(result.source),
            "a private rename broke the class:\n" + result.source);
    ++renamed;
    if (result.source.find("#a = 1") == std::string::npos &&
        result.source.find("{ #a; g(o) { return o.#a") != std::string::npos) {
      ++shadow_kept;
    }
  }
  Require(renamed >= 10, "private names are rarely renamed");
  Require(shadow_kept > 0, "renaming an outer #name changed a shadowing inner class");

  // kFlipGoal turns clean scripts into modules and back; sloppy-only code
  // never becomes a valid-leaning module.
  unsigned to_module = 0;
  unsigned to_script = 0;
  unsigned to_repl = 0;
  unsigned to_eval = 0;
  for (uint32_t seed = 1; seed <= 600; ++seed) {
    const auto up = test::Mutate(parents[1], 4096, seed);
    if (up.edit == EditKind::kFlipGoal && up.source.rfind("/*M*/", 0) == 0) ++to_module;
    const auto down = test::Mutate("/*M*/\nlet x = 1;\n{ using r = null; }", 4096, seed);
    if (down.edit == EditKind::kFlipGoal && down.source.rfind("/*M*/", 0) != 0) ++to_script;
    const auto sloppy = test::Mutate("with (a) { b; }\nvar c = 010;", 4096, seed);
    Require(sloppy.edit != EditKind::kFlipGoal || sloppy.source.rfind("/*M*/", 0) != 0,
            "sloppy code was flipped to a module");
    if (sloppy.edit == EditKind::kFlipGoal) {
      if (sloppy.source.rfind("/*R*/", 0) == 0) ++to_repl;
      if (sloppy.source.rfind("/*E*/", 0) == 0) ++to_eval;
    }
    const auto named = test::Mutate("var await = 1;\nawait + 1;", 4096, seed);
    Require(named.edit != EditKind::kFlipGoal || named.source.rfind("/*R*/", 0) != 0,
            "an `await` name was flipped to REPL code");
  }
  Require(to_module > 0 && to_script > 0, "the goal flip never happens");
  Require(to_repl > 0 && to_eval > 0, "REPL and eval goals are never chosen");

  // A labelled function in a block stays out of modules.
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    for (const char* parent : {"{ L: function f() {} }\n", "if (1) { L: function g() {} }\n"}) {
      const auto result = test::Mutate(parent, 4096, seed);
      Require(result.outcome != Outcome::kEditedValidLeaning ||
                  result.source.rfind("/*M*/", 0) != 0 ||
                  result.source.find("L: function") == std::string::npos,
              "a labelled function declaration became module code:\n" + result.source);
    }
  }

  // `export * as default` is a default export.
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const auto result =
        test::Mutate("/*M*/\nexport * as default from \"x\";\nlet a = 1;\n", 4096, seed);
    Require(result.outcome != Outcome::kEditedValidLeaning ||
                result.source.find("\nexport default") == std::string::npos,
            "a second default export:\n" + result.source);
  }

  // Top-level await keeps a module clean, so its edits stay checked.
  unsigned tla_clean = 0;
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    const auto result = test::Mutate("/*M*/\nawait 0;\nlet q = 1;\nfunction f() { return q; }\n",
                                     4096, seed);
    if (result.outcome == Outcome::kEditedValidLeaning) ++tla_clean;
  }
  Require(tla_clean >= 150, "top-level await made the module look invalid");

  // On a parent that already fails the check, no edit relies on it.
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const auto result = test::Mutate(
        "let a; let a;\nclass C { #x; static #y; m() { return this.#x + C.#y; } }\n", 4096, seed);
    Require(result.edit != EditKind::kRebind && result.edit != EditKind::kPrivateRename &&
                result.edit != EditKind::kFlipGoal,
            "a checker-filtered edit ran on an invalid parent");
  }

  // Compile hints name real functions: each decoded position, counted from
  // the comment's line terminator, is the `(` of a function or arrow.
  {
    const std::string hinted = "let k = 1;\nfunction a(x) { return function b(y) { return y; }; }\n"
                               "const c = (z) => z;\nclass D { m(w) {} }\n";
    unsigned matched = 0;
    unsigned decoded = 0;
    for (uint32_t seed = 1; seed <= 3000 && matched < 20; ++seed) {
      const auto result = test::Mutate(hinted, 4096, seed);
      if (result.edit != EditKind::kMagicComment) continue;
      const std::string key = "//# functionsCalledOnLoad=";
      const size_t at = result.source.rfind(key);
      if (at == std::string::npos) continue;
      const size_t end = result.source.find('\n', at);
      if (end == std::string::npos) continue;
      const std::string value = result.source.substr(at + key.size(), end - at - key.size());
      if (value.empty() || value.find_first_of(" \t!=") != std::string::npos) continue;
      // Base64 VLQ deltas.
      static const std::string kBase64 =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::vector<long> positions;
      long last = static_cast<long>(end);
      bool real = true;
      uint32_t v = 0;
      unsigned shift = 0;
      for (char ch : value) {
        const size_t digit = kBase64.find(ch);
        if (digit == std::string::npos) real = false;
        v |= static_cast<uint32_t>(digit & 31) << shift;
        shift += 5;
        if (digit & 32) continue;
        const long delta = (v & 1) ? -static_cast<long>(v >> 1) : static_cast<long>(v >> 1);
        if (delta < 0) real = false;
        last += delta;
        positions.push_back(last);
        v = 0;
        shift = 0;
      }
      if (!real || positions.empty()) continue;
      // Random hint values are part of the mix too; count the real ones.
      ++decoded;
      bool all_near = true;
      for (long position : positions) {
        bool near = false;
        for (long i = std::max(0L, position - 3);
             i <= position + 3 && i < static_cast<long>(result.source.size()); ++i) {
          if (result.source[i] == '(') near = true;
        }
        all_near = all_near && near;
      }
      if (all_near) ++matched;
    }
    Require(matched >= 8 && matched * 2 >= decoded, "compile hints rarely name functions: " + std::to_string(matched) + " of " +
                std::to_string(decoded));
  }

  // Deeply nested input stays within the stack and the time limits.
  {
    const auto start = std::chrono::steady_clock::now();
    const std::string deep[] = {std::string(3000, '[') + std::string(3000, ']'),
                                std::string(1000, '{') + std::string(1000, '}'),
                                [] {
                                  std::string s;
                                  for (int i = 0; i < 5000; ++i) s += "a=";
                                  return s + "1";
                                }()};
    for (const std::string& input : deep) {
      for (uint32_t seed = 0; seed < 3; ++seed) {
        const auto result = test::Mutate(input, 65536, seed, 90);
        Require(result.source.size() <= 65536, "deep input exceeded max_size");
        const auto crossed = js_mutator::CrossOver(
            reinterpret_cast<const uint8_t*>(input.data()), input.size(),
            reinterpret_cast<const uint8_t*>(input.data()), input.size(), 4096, seed, {90});
        Require(crossed.source.size() <= 4096, "deep crossover exceeded max_size");
      }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Require(seconds < 20, "deeply nested input is slow: " + std::to_string(seconds) + " s");
  }
  std::cout << "mutator contract: ok\n";
  return 0;
}
