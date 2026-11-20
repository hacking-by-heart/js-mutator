// Interface contract: limits, determinism, fallback, validity, crossover.
#include "test_util.h"

#include <cstring>
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
        if (value.size() >= 1020 && value.size() <= 1035) note("long 1024");
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
    Require(lines >= 1 && lines <= 4, "magic comments accumulate");
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

  // Malformed mode breaks syntax (or injects an early error).
  unsigned broken_outputs = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const auto result = test::Mutate(parents[1], 4096, seed, 0);
    Require(result.outcome == Outcome::kEditedMalformed, "malformed mode did not edit");
    if (!syntax.Parses(result.source) || result.edit == EditKind::kInjectEarlyError) {
      ++broken_outputs;
    }
  }
  Require(broken_outputs == 300, "malformed mode produced valid output");

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
  TestUnicodeTables(syntax);
  TestRawUnicode(syntax);
  TestLiterals(syntax);
  TestMagicComments(syntax);
  std::cout << "mutator contract: ok\n";
  return 0;
}
