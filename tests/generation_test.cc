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
    "parenthesized_expression", "using_declaration"};

// Constructs only module programs contain.
const char* const kModuleConstructs[] = {
    "import_statement", "import_clause", "namespace_import", "named_imports",
    "import_specifier", "import_attribute", "export_statement", "export_clause",
    "export_specifier", "namespace_export"};

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

// Name and target shapes (each group: any one spelling suffices).
const std::vector<std::vector<std::string>> kNameShapes = {
    {"var await", "var yield", "var let", "var static", "var eval", "var arguments",
     "let await", "let static", "const yield", "let implements", "var package"},
    {"await: "},
    {"(let)["},
    {"using\n", "using["},
    {") let\n"},
    {"\\u{0"},
    {"\nstatic static", "\nget;", "\nstatic;", "\nasync;", "\naccessor;", "\nset;",
     "\nget = ", "\nstatic("},
    {")) = ", ")) += ", ")) -= "},
    {": function f"},
    {") function f"},
};

std::vector<std::string> DeclaredPrivates(TSNode body, std::string_view source) {
  std::vector<std::string> out;
  const uint32_t count = ts_node_named_child_count(body);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode member = ts_node_named_child(body, i);
    TSNode name = Is(member, "field_definition") ? ts_node_child_by_field_name(member, "property", 8)
                                                 : ts_node_child_by_field_name(member, "name", 4);
    if (Is(name, "private_property_identifier")) {
      out.emplace_back(source.substr(ts_node_start_byte(name),
                                     ts_node_end_byte(name) - ts_node_start_byte(name)));
    }
  }
  return out;
}

// Class shapes of item 19: a leading computed field keyed by a function
// literal, an inner class redeclaring an outer #name, and a heritage arrow
// that reads an outer #name.
struct ClassShapes {
  unsigned lead_function_key = 0;
  unsigned shadowed_private = 0;
  unsigned heritage_private_arrow = 0;
};

void CountClassShapes(test::Syntax& syntax, const std::string& source, ClassShapes* shapes) {
  syntax.Visit(source, [&](TSNode node, std::string_view text) {
    if (Is(node, "class_body")) {
      TSNode first = ts_node_named_child(node, 0);
      while (Is(first, "comment")) first = ts_node_next_named_sibling(first);
      TSNode key = Is(first, "field_definition")
                       ? ts_node_child_by_field_name(first, "property", 8)
                       : TSNode{};
      if (Is(key, "computed_property_name")) {
        TSNode inner = ts_node_named_child(key, 0);
        if (Is(inner, "arrow_function") || Is(inner, "function_expression") ||
            Is(inner, "generator_function") || Is(inner, "class")) {
          ++shapes->lead_function_key;
        }
      }
      const auto own = DeclaredPrivates(node, text);
      for (TSNode p = ts_node_parent(node); !ts_node_is_null(p); p = ts_node_parent(p)) {
        if (!Is(p, "class_body")) continue;
        bool shadowed = false;
        for (const auto& name : DeclaredPrivates(p, text)) {
          for (const auto& mine : own) shadowed = shadowed || name == mine;
        }
        if (shadowed) {
          ++shapes->shadowed_private;
          break;
        }
      }
    }
    if (Is(node, "class_heritage") && ContainsType(node, "arrow_function") &&
        ContainsType(node, "private_property_identifier")) {
      ++shapes->heritage_private_arrow;
    }
  });
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
  std::vector<bool> shapes(kNameShapes.size(), false);
  std::vector<std::string> regexes;
  // Cover-grammar finishes, yield terminators, import() options, and
  // parameter defaults.
  const char* const kForms[] = {
      "async(await", "async(yield", ") => ", "async (", "} = ", "[yield]", "(yield)",
      "? yield : yield", "${yield}", "{with: ", "eval(", "arguments[0]",
      "class { static {"};
  std::map<std::string, unsigned> form_counts;
  std::map<std::string, unsigned> module_counts;
  unsigned modules = 0;
  unsigned await_using = 0;
  ClassShapes class_shapes;
  for (uint32_t seed = 1; seed <= 20000; ++seed) {
    const auto result = test::Generate(2048, seed);
    Require(result.outcome == Outcome::kGeneratedValidLeaning, "empty input did not generate");
    Require(syntax.Parses(result.source), "generated program did not parse:\n" + result.source);
    for (size_t g = 0; g < kNameShapes.size(); ++g) {
      for (const std::string& spelling : kNameShapes[g]) {
        if (result.source.find(spelling) != std::string::npos) shapes[g] = true;
      }
    }
    for (const auto& type : syntax.Types(result.source)) ++empty_counts[type];
    syntax.Visit(result.source, [&](TSNode node, std::string_view source) {
      if (!Is(node, "regex")) return;
      const uint32_t start = ts_node_start_byte(node);
      regexes.emplace_back(source.substr(start, ts_node_end_byte(node) - start));
    });
    const auto types = syntax.Types(result.source);
    for (const auto& type : types) ++empty_counts[type];
    if (result.source.rfind("/*M*/", 0) == 0) {
      ++modules;
      for (const auto& type : types) ++module_counts[type];
    } else {
      Require(types.count("import_statement") == 0 && types.count("export_statement") == 0,
              "a script contains a module item:\n" + result.source);
    }
    if (result.source.find("await using ") != std::string::npos) ++await_using;
    CountClassShapes(syntax, result.source, &class_shapes);
    if (HasNestedShape(syntax, result.source)) ++empty_nested;
    // Not valid without flags (`x.import.meta` is a property); import.meta is
    // fine in modules.
    const bool is_module = result.source.rfind("/*M*/", 0) == 0;
    for (const char* banned : {"import.meta", "import.defer", "import.source"}) {
      if (is_module && std::string_view(banned) == "import.meta") continue;
      for (size_t at = result.source.find(banned); at != std::string::npos;
           at = result.source.find(banned, at + 1)) {
        Require(at > 0 && result.source[at - 1] == '.',
                std::string("valid output uses ") + banned + ":\n" + result.source);
      }
    }
    for (const char* form : kForms) {
      if (result.source.find(form) != std::string::npos) ++form_counts[form];
    }
    distinct.insert(result.source);
  }
  for (const char* form : kForms) {
    Require(form_counts[form] > 0, std::string("generation never produced ") + form);
  }
  RequireConstructs(empty_counts, "generation from empty input");
  for (size_t g = 0; g < kNameShapes.size(); ++g) {
    Require(shapes[g], "generation never produced the name shape " + kNameShapes[g][0]);
  }
  Require(modules >= 1400 && modules <= 2600, "module programs are not about 10% of generation");
  for (const char* type : kModuleConstructs) {
    Require(module_counts[type] > 0, std::string("module generation never produced ") + type);
  }
  Require(await_using > 0, "generation never produced `await using`");
  Require(class_shapes.lead_function_key >= 2000, "classes rarely lead with a function-literal key");
  Require(class_shapes.shadowed_private >= 500, "inner classes rarely shadow an outer #name");
  Require(class_shapes.heritage_private_arrow >= 150, "heritages never read an outer #name");
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

  std::cout << "modules " << modules << "/20000; class shapes: lead key "
            << class_shapes.lead_function_key << ", shadowed " << class_shapes.shadowed_private
            << ", heritage arrow " << class_shapes.heritage_private_arrow << '\n';
  std::cout << "generation: " << distinct.size() << " distinct programs, nested shape "
            << empty_nested << "/20000 from empty, " << grown_nested
            << " of 12000 grown steps; parameter reuse " << reused << "/" << inside << '\n';
  return 0;
}
