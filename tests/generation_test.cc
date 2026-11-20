// End-goal acceptance: generation from scratch, growth of absent constructs
// from a simple seed, recursive composition, and reuse of in-scope names.
#include "test_util.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using js_mutator::EditKind;
using js_mutator::Outcome;
using test::Require;

namespace {

// Constructs the generator must be able to produce, both from empty input and
// by growing a seed that contains none of them.
const char* const kConstructs[] = {
    "function_declaration", "generator_function_declaration", "function_expression",
    "generator_function", "arrow_function", "class_declaration", "class",
    "class_heritage", "class_static_block", "field_definition", "method_definition",
    "private_property_identifier", "computed_property_name", "if_statement",
    "for_statement", "for_in_statement", "while_statement", "do_statement",
    "switch_statement", "try_statement", "catch_clause", "finally_clause",
    "labeled_statement", "break_statement", "continue_statement", "return_statement",
    "throw_statement", "with_statement", "debugger_statement", "lexical_declaration",
    "variable_declaration", "object_pattern", "array_pattern", "assignment_pattern",
    "rest_pattern", "pair_pattern", "object_assignment_pattern", "spread_element",
    "template_string", "template_substitution", "regex", "regex_flags",
    "escape_sequence", "binary_expression", "unary_expression", "update_expression",
    "ternary_expression", "assignment_expression", "augmented_assignment_expression",
    "sequence_expression", "call_expression", "new_expression", "member_expression",
    "subscript_expression", "optional_chain", "await_expression", "yield_expression",
    "meta_property", "super", "this", "object", "array", "pair",
    "shorthand_property_identifier", "comment", "html_comment", "string", "number",
    "parenthesized_expression"};

bool Is(TSNode node, const char* type) {
  return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), type) == 0;
}

bool ContainsType(TSNode node, const char* type) {
  if (Is(node, type)) return true;
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    if (ContainsType(ts_node_child(node, i), type)) return true;
  }
  return false;
}

bool InsideFunction(TSNode node) {
  for (TSNode p = ts_node_parent(node); !ts_node_is_null(p); p = ts_node_parent(p)) {
    if (Is(p, "function_declaration") || Is(p, "function_expression") ||
        Is(p, "generator_function") || Is(p, "generator_function_declaration") ||
        Is(p, "method_definition")) {
      return true;
    }
  }
  return false;
}

// The nesting shape of:
//   function f1() { (arg = class AAA { static { this; } }) => {} }
// an arrow inside a function, whose parameter default is a class expression
// with a static block that uses `this`.
bool HasNestedShape(test::Syntax& syntax, const std::string& source) {
  bool found = false;
  syntax.Visit(source, [&](TSNode node, std::string_view) {
    if (found || !Is(node, "assignment_pattern")) return;
    TSNode params = ts_node_parent(node);
    if (!Is(params, "formal_parameters") || !Is(ts_node_parent(params), "arrow_function")) {
      return;
    }
    TSNode right = ts_node_child_by_field_name(node, "right", 5);
    if (!Is(right, "class") || !InsideFunction(params)) return;
    TSNode body = ts_node_child_by_field_name(right, "body", 4);
    const uint32_t count = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode member = ts_node_named_child(body, i);
      if (Is(member, "class_static_block") && ContainsType(member, "this")) found = true;
    }
  });
  return found;
}

void RequireConstructs(const std::map<std::string, unsigned>& counts,
                       const char* where) {
  for (const char* type : kConstructs) {
    auto it = counts.find(type);
    Require(it != counts.end() && it->second > 0,
            std::string(where) + " never produced " + type);
  }
}

// The regexp grammar reaches back references planned in a second pass,
// duplicate names in sibling alternatives, modifiers, Annex B escapes, and
// the v-mode class set grammar.
void RequireRegexFeatures(const std::vector<std::string>& regexes) {
  unsigned forward_named = 0, duplicate = 0, modifiers = 0, legacy = 0, sets = 0,
           strings = 0, class_strings = 0;
  for (const auto& r : regexes) {
    const std::string flags = r.substr(r.rfind('/') + 1);
    const bool v = flags.find('v') != std::string::npos;
    const bool u = flags.find('u') != std::string::npos;
    const size_t ref = r.find("\\k<");
    const size_t group = r.find("(?<");
    if (ref != std::string::npos && group != std::string::npos) {
      const size_t close = r.find('>', ref);
      const std::string name = r.substr(ref + 3, close - ref - 3);
      const size_t def = r.find("(?<" + name + ">");
      if (def != std::string::npos && def > ref) ++forward_named;
    }
    std::map<std::string, unsigned> names;
    for (size_t at = r.find("(?<"); at != std::string::npos; at = r.find("(?<", at + 1)) {
      if (r[at + 3] == '=' || r[at + 3] == '!') continue;
      if (++names[r.substr(at + 3, r.find('>', at) - at - 3)] == 2) ++duplicate;
    }
    if (r.find("(?i") != std::string::npos || r.find("(?-") != std::string::npos ||
        r.find("(?m") != std::string::npos || r.find("(?s") != std::string::npos) {
      ++modifiers;
    }
    if (!u && !v && (r.find("\\c]") != std::string::npos || r.find("{,}") != std::string::npos ||
                     r.find("\\8") != std::string::npos)) {
      ++legacy;
    }
    Require(!(u || v) || r.find("{,}") == std::string::npos,
            "an Annex B quantifier fallback in u/v mode: " + r);
    if (v && (r.find("&&") != std::string::npos || r.find("--") != std::string::npos)) ++sets;
    if (r.find("\\p{RGI_") != std::string::npos) {
      Require(v, "a property of strings outside v mode: " + r);
      ++strings;
    }
    Require(r.find("\\P{RGI_") == std::string::npos, "a negated property of strings: " + r);
    if (v && r.find("\\q{") != std::string::npos) ++class_strings;
  }
  const unsigned floor = static_cast<unsigned>(regexes.size() / 200);
  Require(regexes.size() > 2000, "generation rarely produces regexps");
  Require(forward_named > floor, "no forward named back references");
  Require(duplicate > floor, "no duplicate group names in sibling alternatives");
  Require(modifiers > floor, "no modifier groups");
  Require(legacy > floor, "no Annex B regexp syntax");
  Require(sets > floor, "no v-mode set operations");
  Require(strings > floor && class_strings > floor, "no v-mode strings");
}

}  // namespace

int main() {
  test::Syntax syntax;

  // 1. Generation from scratch.
  std::map<std::string, unsigned> empty_counts;
  unsigned empty_nested = 0;
  std::set<std::string> distinct;
  std::vector<std::string> regexes;
  for (uint32_t seed = 1; seed <= 20000; ++seed) {
    const auto result = test::Generate(2048, seed);
    Require(result.outcome == Outcome::kGeneratedValidLeaning, "empty input did not generate");
    Require(syntax.Parses(result.source), "generated program did not parse:\n" + result.source);
    for (const auto& type : syntax.Types(result.source)) ++empty_counts[type];
    syntax.Visit(result.source, [&](TSNode node, std::string_view source) {
      if (!Is(node, "regex")) return;
      const uint32_t start = ts_node_start_byte(node);
      regexes.emplace_back(source.substr(start, ts_node_end_byte(node) - start));
    });
    if (HasNestedShape(syntax, result.source)) ++empty_nested;
    distinct.insert(result.source);
  }
  RequireConstructs(empty_counts, "generation from empty input");
  Require(distinct.size() >= 18000, "generated programs are not diverse");
  Require(empty_nested > 0, "generation never produced the nested arrow/class/static shape");
  RequireRegexFeatures(regexes);

  // 2. Growth: a seed with only arithmetic grows constructs it lacks, and the
  //    retained output is the next parent (the libFuzzer coverage loop).
  const std::string arithmetic = "let x = 1 + 2;";
  std::map<std::string, unsigned> grown_counts;
  unsigned grown_nested = 0;
  for (uint32_t run = 1; run <= 400; ++run) {
    std::string parent = arithmetic;
    for (uint32_t generation = 1; generation <= 30; ++generation) {
      const auto result = test::Mutate(parent, 2048, run * 7919 + generation);
      if (result.outcome == Outcome::kFallback) continue;
      Require(syntax.Parses(result.source), "grown program did not parse:\n" + result.source);
      parent = result.source;
      if (HasNestedShape(syntax, parent)) ++grown_nested;
    }
    for (const auto& type : syntax.Types(parent)) ++grown_counts[type];
  }
  RequireConstructs(grown_counts, "growth from an arithmetic seed");
  Require(grown_nested > 0, "growth never produced the nested arrow/class/static shape");

  // 3. Scope reuse: code generated inside a function uses its parameters.
  const std::string seed = "function total(price, qty) { return price * qty; }\ntotal(2, 3);";
  const size_t body_start = seed.find('{');
  const size_t body_end = seed.find('}');
  unsigned inside = 0;
  unsigned reused = 0;
  for (uint32_t s = 1; s <= 2000; ++s) {
    const auto result = test::Mutate(seed, 4096, s);
    if (result.edit != EditKind::kGenerateStatement &&
        result.edit != EditKind::kGenerateExpression) {
      continue;
    }
    size_t prefix = 0;
    while (prefix < seed.size() && seed[prefix] == result.source[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < seed.size() - prefix &&
           seed[seed.size() - 1 - suffix] == result.source[result.source.size() - 1 - suffix]) {
      ++suffix;
    }
    if (prefix <= body_start || seed.size() - suffix > body_end) continue;
    ++inside;
    const std::string added = result.source.substr(prefix, result.source.size() - suffix - prefix);
    for (const char* name : {"price", "qty", "\\u0070rice", "\\u0071ty"}) {
      if (added.find(name) != std::string::npos) {
        ++reused;
        break;
      }
    }
  }
  Require(inside >= 100, "few generated edits landed inside the function");
  Require(reused * 100 >= inside * 40,
          "generated code inside a function rarely uses its parameters");

  std::cout << "generation: " << distinct.size() << " distinct programs, nested shape "
            << empty_nested << "/20000 from empty, " << grown_nested
            << " of 12000 grown steps; parameter reuse " << reused << "/" << inside << '\n';
  return 0;
}
