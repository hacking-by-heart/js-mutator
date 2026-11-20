#include "js_mutator/mutator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-javascript.h>

#include "generator.h"

namespace js_mutator {
namespace {

using internal::DecodeIdentifier;
using internal::GenContext;
using internal::Generator;
using internal::IsSpecialName;
using internal::IsStrictReservedWord;
using internal::NeedsStatementParens;
using internal::Random;
using internal::SeedInfo;

constexpr size_t kMaxSourceBytes = 64 * 1024;
// Generation from empty input targets this size, independent of MaxSize, so
// a program is reproduced byte for byte when replayed at its exact size.
constexpr size_t kGenerationBytes = 2048;
constexpr unsigned kMaxAttempts = 8;
constexpr unsigned kMaxProgressCalls = 4096;
constexpr unsigned kMaxNodeVisits = 1u << 18;
constexpr size_t kMaxSeedNames = 64;
constexpr size_t kMaxSeedExpressions = 64;
constexpr size_t kMaxBlocks = 16;
constexpr size_t kMaxReusableExpressionBytes = 192;
constexpr size_t kMaxBlockBytes = 768;
constexpr unsigned kMaxCheckDepth = 1500;

struct Budget {
  unsigned progress_calls = 0;
  unsigned node_visits = 0;
  bool exhausted = false;
};

struct Input {
  std::string_view source;
};

Result Fallback(size_t max_size) {
  return {max_size == 0 ? "" : ";", Outcome::kFallback, EditKind::kNone};
}

const char* ReadInput(void* payload, uint32_t offset, TSPoint,
                      uint32_t* bytes_read) {
  const auto source = static_cast<Input*>(payload)->source;
  if (offset >= source.size()) {
    *bytes_read = 0;
    return "";
  }
  *bytes_read = static_cast<uint32_t>(source.size() - offset);
  return source.data() + offset;
}

bool OnProgress(TSParseState* state) {
  auto* budget = static_cast<Budget*>(state->payload);
  if (++budget->progress_calls >= kMaxProgressCalls) {
    budget->exhausted = true;
    return true;
  }
  return false;
}

using Parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
using Tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct Cursor {
  explicit Cursor(TSNode root) : value(ts_tree_cursor_new(root)) {}
  ~Cursor() { ts_tree_cursor_delete(&value); }
  Cursor(const Cursor&) = delete;
  Cursor& operator=(const Cursor&) = delete;
  TSTreeCursor value;
};

Tree Parse(TSParser* parser, std::string_view source, Budget* budget) {
  ts_parser_reset(parser);
  Input payload{source};
  TSInput input{&payload, ReadInput, TSInputEncodingUTF8, nullptr};
  TSParseOptions options{budget, OnProgress};
  return Tree(ts_parser_parse_with_options(parser, nullptr, input, options),
              ts_tree_delete);
}

template <typename Visit>
bool Walk(TSNode root, Budget* budget, Visit visit) {
  Cursor cursor(root);
  std::vector<TSNode> ancestors;
  for (;;) {
    if (budget->node_visits >= kMaxNodeVisits) {
      budget->exhausted = true;
      return false;
    }
    ++budget->node_visits;
    TSNode node = ts_tree_cursor_current_node(&cursor.value);
    TSNode parent = ancestors.empty() ? TSNode{} : ancestors.back();
    if (!visit(node, parent)) return false;
    if (ts_tree_cursor_goto_first_child(&cursor.value)) {
      ancestors.push_back(node);
      continue;
    }
    while (!ts_tree_cursor_goto_next_sibling(&cursor.value)) {
      if (!ts_tree_cursor_goto_parent(&cursor.value)) return true;
      ancestors.pop_back();
    }
  }
}

bool Is(TSNode node, const char* type) {
  return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), type) == 0;
}

TSNode Field(TSNode node, const char* name) {
  return ts_node_child_by_field_name(node, name,
                                     static_cast<uint32_t>(std::strlen(name)));
}

std::string_view NodeText(TSNode node, std::string_view source) {
  const size_t start = ts_node_start_byte(node);
  const size_t end = ts_node_end_byte(node);
  if (start > end || end > source.size()) return {};
  return source.substr(start, end - start);
}

std::optional<unsigned> CountSyntaxErrors(TSNode root, Budget* budget) {
  unsigned errors = 0;
  Walk(root, budget, [&](TSNode node, TSNode) {
    if (ts_node_is_error(node) || ts_node_is_missing(node)) ++errors;
    return true;
  });
  if (budget->exhausted) return std::nullopt;
  return errors;
}

bool HasDirectToken(TSNode node, const char* token) {
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    if (std::strcmp(ts_node_type(ts_node_child(node, i)), token) == 0) return true;
  }
  return false;
}

bool IsFunctionType(const char* type) {
  return std::strcmp(type, "function_declaration") == 0 ||
         std::strcmp(type, "function_expression") == 0 ||
         std::strcmp(type, "generator_function_declaration") == 0 ||
         std::strcmp(type, "generator_function") == 0 ||
         std::strcmp(type, "arrow_function") == 0 ||
         std::strcmp(type, "method_definition") == 0;
}

bool IsLoopType(const char* type) {
  return std::strcmp(type, "for_statement") == 0 ||
         std::strcmp(type, "for_in_statement") == 0 ||
         std::strcmp(type, "while_statement") == 0 ||
         std::strcmp(type, "do_statement") == 0;
}

bool IsDeclarationType(const char* type) {
  return std::strcmp(type, "lexical_declaration") == 0 ||
         std::strcmp(type, "class_declaration") == 0 ||
         std::strcmp(type, "function_declaration") == 0 ||
         std::strcmp(type, "generator_function_declaration") == 0;
}

bool IsExtra(TSNode node) {
  return Is(node, "comment") || Is(node, "html_comment") ||
         Is(node, "hash_bang_line");
}

bool IsDirective(TSNode statement) {
  return Is(statement, "expression_statement") &&
         ts_node_named_child_count(statement) == 1 &&
         Is(ts_node_named_child(statement, 0), "string");
}

bool IsUseStrict(TSNode statement, std::string_view source) {
  if (!IsDirective(statement)) return false;
  std::string_view text = NodeText(ts_node_named_child(statement, 0), source);
  return text == "\"use strict\"" || text == "'use strict'";
}

std::vector<TSNode> ListStatements(TSNode list) {
  std::vector<TSNode> out;
  const uint32_t count = ts_node_named_child_count(list);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode child = ts_node_named_child(list, i);
    if (!IsExtra(child)) out.push_back(child);
  }
  return out;
}

// Number of leading directive statements (a directive prologue only exists in
// programs and function bodies, but treating every list alike is harmless).
size_t PrologueLength(const std::vector<TSNode>& statements) {
  size_t n = 0;
  while (n < statements.size() && IsDirective(statements[n])) ++n;
  return n;
}

bool ListIsStrict(TSNode list, std::string_view source) {
  const auto statements = ListStatements(list);
  const size_t n = PrologueLength(statements);
  for (size_t i = 0; i < n; ++i) {
    if (IsUseStrict(statements[i], source)) return true;
  }
  return false;
}

bool ClassIsDerived(TSNode class_node) {
  const uint32_t count = ts_node_child_count(class_node);
  for (uint32_t i = 0; i < count; ++i) {
    if (Is(ts_node_child(class_node, i), "class_heritage")) return true;
  }
  return false;
}

bool IsReservedContextualName(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "await", "yield", "let", "static", "get", "set", "of", "async",
      "eval", "arguments", "implements", "interface", "package", "private",
      "protected", "public", "undefined"};
  for (std::string_view candidate : kNames) {
    if (name == candidate) return true;
  }
  return false;
}

void CollectPrivateDeclarations(TSNode class_body, std::string_view source,
                                std::vector<std::string>* out) {
  const uint32_t count = ts_node_named_child_count(class_body);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode member = ts_node_named_child(class_body, i);
    TSNode name = Is(member, "field_definition") ? Field(member, "property")
                  : Is(member, "method_definition") ? Field(member, "name")
                                                    : TSNode{};
    if (Is(name, "private_property_identifier")) {
      std::string text(NodeText(name, source));
      if (std::find(out->begin(), out->end(), text) == out->end()) {
        out->push_back(text);
      }
    }
  }
}

bool IsStaticMember(TSNode member) { return HasDirectToken(member, "static"); }

std::string MemberName(TSNode member, std::string_view source) {
  TSNode name = Is(member, "field_definition") ? Field(member, "property")
                                                : Field(member, "name");
  return ts_node_is_null(name) ? std::string() : std::string(NodeText(name, source));
}

// The syntactic context of `node`: what return, await, yield, break,
// continue, super, new.target, arguments, private names, and strictness mean
// at that position.
GenContext ContextFor(TSNode node, std::string_view source) {
  GenContext c;
  bool barrier = false;   // crossed a function or class-element boundary
  bool fn_set = false;    // return/await/yield decided
  bool settled = false;   // super/new.target/arguments decided
  TSNode child = node;
  for (TSNode p = node; !ts_node_is_null(p); child = p, p = ts_node_parent(p)) {
    const char* type = ts_node_type(p);
    if (!barrier) {
      if (IsLoopType(type) && !ts_node_eq(p, node)) c.loop = true;
      if (std::strcmp(type, "switch_body") == 0) c.switch_stmt = true;
      if (std::strcmp(type, "labeled_statement") == 0) {
        std::string label(NodeText(Field(p, "label"), source));
        c.labels.push_back(label);
        TSNode body = Field(p, "body");
        if (!ts_node_is_null(body) && IsLoopType(ts_node_type(body))) {
          c.loop_labels.push_back(label);
        }
      }
    }
    if (std::strcmp(type, "class_body") == 0) {
      CollectPrivateDeclarations(p, source, &c.private_names);
    }
    // All parts of a class, including its heritage, are strict code.
    if (std::strcmp(type, "class") == 0 || std::strcmp(type, "class_declaration") == 0) {
      c.strict = true;
    }
    if ((std::strcmp(type, "program") == 0 ||
         (std::strcmp(type, "statement_block") == 0 &&
          !ts_node_is_null(ts_node_parent(p)) &&
          IsFunctionType(ts_node_type(ts_node_parent(p))))) &&
        ListIsStrict(p, source)) {
      c.strict = true;
    }
    if (settled) continue;
    const bool static_block = std::strcmp(type, "class_static_block") == 0;
    const bool field_value = std::strcmp(type, "field_definition") == 0 &&
                             ts_node_eq(child, Field(p, "value"));
    if (static_block || field_value) {
      if (!fn_set) {
        fn_set = true;
        c.function = false;
        c.async = false;
        c.generator = false;
      }
      barrier = true;
      settled = true;
      c.class_field = true;
      c.super_property = true;
      c.new_target = true;
      continue;
    }
    if (!IsFunctionType(type)) continue;
    const bool in_body = ts_node_eq(child, Field(p, "body"));
    const bool in_params = ts_node_eq(child, Field(p, "parameters")) ||
                           ts_node_eq(child, Field(p, "parameter"));
    if (!in_body && !in_params) continue;  // e.g. a computed method name
    if (!fn_set) {
      fn_set = true;
      c.function = in_body;
      c.async = in_body && HasDirectToken(p, "async");
      c.generator = in_body && (std::strstr(type, "generator") != nullptr ||
                                HasDirectToken(p, "*"));
    }
    barrier = true;
    if (std::strcmp(type, "arrow_function") == 0) continue;
    settled = true;
    c.new_target = true;
    if (std::strcmp(type, "method_definition") == 0) {
      c.super_property = true;
      TSNode owner = ts_node_parent(p);
      if (Is(owner, "class_body") && !IsStaticMember(p)) {
        const std::string name = MemberName(p, source);
        if (name == "constructor" || name == "'constructor'" ||
            name == "\"constructor\"") {
          c.super_call = ClassIsDerived(ts_node_parent(owner));
        }
      }
    }
  }
  return c;
}

void AddName(std::vector<std::string>* names, std::string_view text) {
  if (text.empty() || names->size() >= kMaxSeedNames) return;
  if (IsReservedContextualName(text)) return;
  const std::string name(text);
  if (std::find(names->begin(), names->end(), name) == names->end()) {
    names->push_back(name);
  }
}

void CollectBindingNames(TSNode node, std::string_view source,
                         std::vector<std::string>* names, Budget* budget,
                         unsigned depth = 0) {
  if (ts_node_is_null(node) || depth > 64) return;
  if (budget->node_visits >= kMaxNodeVisits) {
    budget->exhausted = true;
    return;
  }
  ++budget->node_visits;
  const char* type = ts_node_type(node);
  if (std::strcmp(type, "identifier") == 0 ||
      std::strcmp(type, "shorthand_property_identifier_pattern") == 0) {
    AddName(names, NodeText(node, source));
    return;
  }
  if (std::strcmp(type, "assignment_pattern") == 0 ||
      std::strcmp(type, "object_assignment_pattern") == 0) {
    CollectBindingNames(Field(node, "left"), source, names, budget, depth + 1);
    return;
  }
  if (std::strcmp(type, "pair_pattern") == 0) {
    CollectBindingNames(Field(node, "value"), source, names, budget, depth + 1);
    return;
  }
  if (std::strcmp(type, "computed_property_name") == 0) return;
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count && !budget->exhausted; ++i) {
    CollectBindingNames(ts_node_named_child(node, i), source, names, budget,
                        depth + 1);
  }
}

// Names declared directly by a statement in its own list.
void DeclaredNames(TSNode statement, std::string_view source,
                   std::vector<std::string>* names, Budget* budget) {
  const char* type = ts_node_type(statement);
  if (std::strcmp(type, "lexical_declaration") == 0 ||
      std::strcmp(type, "variable_declaration") == 0) {
    const uint32_t count = ts_node_named_child_count(statement);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode declarator = ts_node_named_child(statement, i);
      if (Is(declarator, "variable_declarator")) {
        CollectBindingNames(Field(declarator, "name"), source, names, budget);
      }
    }
  } else if (std::strcmp(type, "function_declaration") == 0 ||
             std::strcmp(type, "generator_function_declaration") == 0 ||
             std::strcmp(type, "class_declaration") == 0) {
    TSNode name = Field(statement, "name");
    if (!ts_node_is_null(name)) AddName(names, NodeText(name, source));
  }
}

// Every binding visible at `node`, innermost scope first.
std::vector<std::string> VisibleNames(TSNode node, std::string_view source,
                                      Budget* budget) {
  std::vector<std::string> names;
  TSNode child = node;
  for (TSNode p = node; !ts_node_is_null(p); child = p, p = ts_node_parent(p)) {
    const char* type = ts_node_type(p);
    if (std::strcmp(type, "program") == 0 ||
        std::strcmp(type, "statement_block") == 0) {
      for (TSNode statement : ListStatements(p)) {
        DeclaredNames(statement, source, &names, budget);
      }
    } else if (std::strcmp(type, "switch_body") == 0) {
      const uint32_t cases = ts_node_named_child_count(p);
      for (uint32_t i = 0; i < cases; ++i) {
        TSNode clause = ts_node_named_child(p, i);
        const uint32_t count = ts_node_named_child_count(clause);
        for (uint32_t j = 0; j < count; ++j) {
          DeclaredNames(ts_node_named_child(clause, j), source, &names, budget);
        }
      }
    } else if (IsFunctionType(type)) {
      CollectBindingNames(Field(p, "parameters"), source, &names, budget);
      CollectBindingNames(Field(p, "parameter"), source, &names, budget);
      if (std::strcmp(type, "function_expression") == 0 ||
          std::strcmp(type, "generator_function") == 0) {
        TSNode name = Field(p, "name");
        if (!ts_node_is_null(name)) AddName(&names, NodeText(name, source));
      }
    } else if (std::strcmp(type, "for_statement") == 0) {
      TSNode init = Field(p, "initializer");
      if (Is(init, "lexical_declaration") || Is(init, "variable_declaration")) {
        DeclaredNames(init, source, &names, budget);
      }
    } else if (std::strcmp(type, "for_in_statement") == 0) {
      if (!ts_node_is_null(Field(p, "kind"))) {
        CollectBindingNames(Field(p, "left"), source, &names, budget);
      }
    } else if (std::strcmp(type, "catch_clause") == 0) {
      CollectBindingNames(Field(p, "parameter"), source, &names, budget);
    } else if (std::strcmp(type, "class") == 0) {
      TSNode name = Field(p, "name");
      if (!ts_node_is_null(name)) AddName(&names, NodeText(name, source));
    }
    if (budget->exhausted) break;
  }
  (void)child;
  return names;
}

bool IsIdentifierLike(const char* type) {
  return std::strcmp(type, "identifier") == 0 ||
         std::strcmp(type, "property_identifier") == 0 ||
         std::strcmp(type, "private_property_identifier") == 0 ||
         std::strcmp(type, "shorthand_property_identifier") == 0 ||
         std::strcmp(type, "shorthand_property_identifier_pattern") == 0 ||
         std::strcmp(type, "statement_identifier") == 0;
}

void CollectAllNames(TSNode root, std::string_view source,
                     std::vector<std::string>* reserved, Budget* budget) {
  std::set<std::string> seen(reserved->begin(), reserved->end());
  Walk(root, budget, [&](TSNode node, TSNode) {
    if (!IsIdentifierLike(ts_node_type(node))) return true;
    std::string name(NodeText(node, source));
    if (!name.empty() && name[0] == '#') name.erase(0, 1);
    // An escaped spelling also reserves the name it decodes to.
    std::string decoded = DecodeIdentifier(name);
    if (!name.empty() && seen.insert(name).second) reserved->push_back(std::move(name));
    if (!decoded.empty() && seen.insert(decoded).second) {
      reserved->push_back(std::move(decoded));
    }
    return true;
  });
}

bool HasLegacySyntax(TSNode node, std::string_view source) {
  const char* type = ts_node_type(node);
  const std::string_view text = NodeText(node, source);
  if (std::strcmp(type, "number") == 0) {
    return text.size() > 1 && text[0] == '0' &&
           ((text[1] >= '0' && text[1] <= '9') || text[1] == '_');
  }
  if (std::strcmp(type, "string") == 0 || std::strcmp(type, "template_string") == 0 ||
      std::strcmp(type, "escape_sequence") == 0) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
      if (text[i] == '\\') {
        const char next = text[i + 1];
        if (next >= '1' && next <= '9') return true;
        if (next == '0' && i + 2 < text.size() && text[i + 2] >= '0' &&
            text[i + 2] <= '9') {
          return true;
        }
        ++i;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Names and assignment targets.

bool IsIdentifierNode(TSNode node) {
  return Is(node, "identifier") || Is(node, "shorthand_property_identifier") ||
         Is(node, "shorthand_property_identifier_pattern");
}

bool IsEvalOrArguments(std::string_view name) {
  return name == "eval" || name == "arguments";
}

// Reserved words that are never identifiers, escaped or not.
bool IsAlwaysReservedWord(std::string_view name) {
  static constexpr std::string_view kWords[] = {
      "break", "case", "catch", "class", "const", "continue", "debugger",
      "default", "delete", "do", "else", "enum", "export", "extends", "false",
      "finally", "for", "function", "if", "import", "in", "instanceof", "new",
      "null", "return", "super", "switch", "this", "throw", "true", "try",
      "typeof", "var", "void", "while", "with"};
  for (std::string_view word : kWords) {
    if (name == word) return true;
  }
  return false;
}

TSNode SkipParens(TSNode node) {
  while (Is(node, "parenthesized_expression") && ts_node_named_child_count(node) == 1) {
    node = ts_node_named_child(node, 0);
  }
  return node;
}

// Whether an identifier node is bound or assigned (not merely read).
bool IsBindingOrTarget(TSNode id) {
  if (Is(id, "shorthand_property_identifier_pattern")) return true;
  TSNode child = id;
  TSNode p = ts_node_parent(id);
  while (Is(p, "parenthesized_expression")) {
    child = p;
    p = ts_node_parent(p);
  }
  if (ts_node_is_null(p)) return false;
  auto field_is = [&](const char* name) { return ts_node_eq(child, Field(p, name)); };
  const char* type = ts_node_type(p);
  if (std::strcmp(type, "formal_parameters") == 0 || std::strcmp(type, "array_pattern") == 0 ||
      std::strcmp(type, "rest_pattern") == 0 || std::strcmp(type, "object_pattern") == 0 ||
      std::strcmp(type, "update_expression") == 0) {
    return true;
  }
  if (std::strcmp(type, "variable_declarator") == 0 || IsFunctionType(type) ||
      std::strcmp(type, "class") == 0 || std::strcmp(type, "class_declaration") == 0) {
    return field_is("name");
  }
  if (std::strcmp(type, "assignment_pattern") == 0 ||
      std::strcmp(type, "object_assignment_pattern") == 0 ||
      std::strcmp(type, "assignment_expression") == 0 ||
      std::strcmp(type, "augmented_assignment_expression") == 0 ||
      std::strcmp(type, "for_in_statement") == 0) {
    return field_is("left");
  }
  if (std::strcmp(type, "pair_pattern") == 0) return field_is("value");
  if (std::strcmp(type, "catch_clause") == 0) return field_is("parameter");
  if (std::strcmp(type, "arrow_function") == 0) return field_is("parameter");
  return false;
}

// A function call used as an assignment target: a runtime error in sloppy
// code (Annex B), an early error everywhere else.
bool IsCallTarget(TSNode target) {
  TSNode inner = SkipParens(target);
  return Is(inner, "call_expression");
}

bool IsGeneratorFunction(TSNode node) {
  const char* type = ts_node_type(node);
  return std::strstr(type, "generator") != nullptr ||
         (std::strcmp(type, "method_definition") == 0 && HasDirectToken(node, "*"));
}

bool IsAsyncFunction(TSNode node) {
  return IsFunctionType(ts_node_type(node)) && HasDirectToken(node, "async");
}

// Bound names of a pattern, decoded, duplicates kept.
void CollectPatternNames(TSNode node, std::string_view source,
                         std::vector<std::string>* names, unsigned depth = 0) {
  if (ts_node_is_null(node) || depth > 64) return;
  const char* type = ts_node_type(node);
  if (std::strcmp(type, "identifier") == 0 ||
      std::strcmp(type, "shorthand_property_identifier_pattern") == 0) {
    names->push_back(DecodeIdentifier(NodeText(node, source)));
    return;
  }
  if (std::strcmp(type, "assignment_pattern") == 0 ||
      std::strcmp(type, "object_assignment_pattern") == 0) {
    CollectPatternNames(Field(node, "left"), source, names, depth + 1);
    return;
  }
  if (std::strcmp(type, "pair_pattern") == 0) {
    CollectPatternNames(Field(node, "value"), source, names, depth + 1);
    return;
  }
  if (std::strcmp(type, "computed_property_name") == 0) return;
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count; ++i) {
    CollectPatternNames(ts_node_named_child(node, i), source, names, depth + 1);
  }
}

void ParameterNames(TSNode function, std::string_view source,
                    std::vector<std::string>* names) {
  CollectPatternNames(Field(function, "parameters"), source, names);
  CollectPatternNames(Field(function, "parameter"), source, names);
}

bool HasDuplicate(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  return std::adjacent_find(names.begin(), names.end()) != names.end();
}

// Only plain identifiers, no defaults, patterns, or rest.
bool HasSimpleParameters(TSNode function) {
  TSNode params = Field(function, "parameters");
  if (ts_node_is_null(params)) return Is(Field(function, "parameter"), "identifier");
  const uint32_t count = ts_node_named_child_count(params);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode param = ts_node_named_child(params, i);
    if (!Is(param, "identifier") && !IsExtra(param)) return false;
  }
  return true;
}

// Code that is only legal in sloppy mode. `in_generator` and `in_async` say
// whether the node is inside a generator or async function of the subtree.
bool IsSloppyOnlyNode(TSNode node, std::string_view source, bool in_generator,
                      bool in_async) {
  static constexpr std::string_view kStrictReserved[] = {
      "let", "static", "yield", "implements", "interface", "package",
      "private", "protected", "public", "await", "using"};
  bool found = false;
  {
    const char* type = ts_node_type(node);
    if (IsIdentifierNode(node)) {
      const std::string name = DecodeIdentifier(NodeText(node, source));
      for (std::string_view word : kStrictReserved) {
        if (name == word) found = true;
      }
      if (IsEvalOrArguments(name) && IsBindingOrTarget(node)) found = true;
      return found;
    }
    // Tree-sitter reads `yield` and `await` identifiers as operators.
    if (std::strcmp(type, "yield_expression") == 0 && !in_generator) {
      found = true;
    } else if (std::strcmp(type, "await_expression") == 0 && !in_async) {
      found = true;
    } else if ((std::strcmp(type, "assignment_expression") == 0 ||
                std::strcmp(type, "augmented_assignment_expression") == 0 ||
                std::strcmp(type, "for_in_statement") == 0) &&
               IsCallTarget(Field(node, "left"))) {
      found = true;
    } else if (std::strcmp(type, "update_expression") == 0 &&
               IsCallTarget(Field(node, "argument"))) {
      found = true;
    } else if (IsFunctionType(type) && !HasSimpleParameters(node)) {
      // Non-simple lists never allow duplicates; the checker rejects those.
    } else if (IsFunctionType(type)) {
      std::vector<std::string> params;
      ParameterNames(node, source, &params);
      if (params.size() > 1 && HasDuplicate(params)) found = true;
    }
    if (found) return found;
    if (std::strcmp(type, "with_statement") == 0 || HasLegacySyntax(node, source)) {
      found = true;
    } else if (std::strcmp(type, "for_in_statement") == 0 &&
               !ts_node_is_null(Field(node, "value"))) {
      found = true;  // Annex B: for (var x = init in obj)
    } else if ((std::strcmp(type, "if_statement") == 0 ||
                std::strcmp(type, "labeled_statement") == 0 ||
                std::strcmp(type, "else_clause") == 0) &&
               (Is(Field(node, "consequence"), "function_declaration") ||
                Is(Field(node, "alternative"), "function_declaration") ||
                Is(Field(node, "body"), "function_declaration") ||
                Is(ts_node_named_child(node, 0), "function_declaration"))) {
      found = true;  // Annex B function declaration as a statement body
    } else if (std::strcmp(type, "unary_expression") == 0 &&
               HasDirectToken(node, "delete")) {
      TSNode argument = Field(node, "argument");
      while (Is(argument, "parenthesized_expression")) {
        argument = ts_node_named_child(argument, 0);
      }
      if (Is(argument, "identifier")) found = true;
    }
  }
  return found;
}

bool HasSloppyOnly(TSNode root, std::string_view source, Budget* budget) {
  struct Item {
    TSNode node;
    bool in_generator;
    bool in_async;
  };
  std::vector<Item> stack = {{root, false, false}};
  while (!stack.empty()) {
    if (budget->node_visits >= kMaxNodeVisits) {
      budget->exhausted = true;
      return true;
    }
    ++budget->node_visits;
    Item item = stack.back();
    stack.pop_back();
    if (IsSloppyOnlyNode(item.node, source, item.in_generator, item.in_async)) return true;
    bool in_generator = item.in_generator;
    bool in_async = item.in_async;
    if (IsFunctionType(ts_node_type(item.node))) {
      const bool arrow = Is(item.node, "arrow_function");
      in_generator = arrow ? in_generator : IsGeneratorFunction(item.node);
      in_async = (arrow && in_async) || HasDirectToken(item.node, "async");
    }
    const uint32_t count = ts_node_child_count(item.node);
    for (uint32_t i = count; i-- > 0;) {
      stack.push_back({ts_node_child(item.node, i), in_generator, in_async});
    }
  }
  return false;
}

template <typename Pred>
bool ContainsNode(TSNode root, Budget* budget, Pred pred) {
  bool found = false;
  Walk(root, budget, [&](TSNode node, TSNode) {
    found = pred(node);
    return !found;
  });
  return found;
}

// Whether a subtree uses constructs whose meaning depends on the enclosing
// function: await, yield, super, arguments, break/continue, return.
struct Dependencies {
  bool await_ = false;
  bool yield_ = false;
  bool super_ = false;
  bool super_call = false;
  bool arguments = false;
  bool jump = false;
  bool return_ = false;
};

Dependencies FindDependencies(TSNode root, std::string_view source,
                              Budget* budget) {
  Dependencies d;
  Walk(root, budget, [&](TSNode node, TSNode parent) {
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "await_expression") == 0) d.await_ = true;
    if (std::strcmp(type, "for_in_statement") == 0 && HasDirectToken(node, "await")) {
      d.await_ = true;
    }
    if (std::strcmp(type, "yield_expression") == 0) d.yield_ = true;
    if (std::strcmp(type, "super") == 0) {
      d.super_ = true;
      if (Is(parent, "call_expression")) d.super_call = true;
    }
    if (std::strcmp(type, "identifier") == 0 &&
        NodeText(node, source) == "arguments") {
      d.arguments = true;
    }
    // `await` and `yield` used as identifiers cannot move into async or
    // generator code.
    if (IsIdentifierNode(node)) {
      const std::string name = DecodeIdentifier(NodeText(node, source));
      if (name == "await") d.await_ = true;
      if (name == "yield") d.yield_ = true;
    }
    if (std::strcmp(type, "break_statement") == 0 ||
        std::strcmp(type, "continue_statement") == 0) {
      d.jump = true;
    }
    if (std::strcmp(type, "return_statement") == 0) d.return_ = true;
    return true;
  });
  return d;
}

bool IsReusableExpressionType(const char* type) {
  static constexpr std::array<const char*, 22> kTypes = {
      "number", "string", "true", "false", "null", "regex", "template_string",
      "array", "object", "parenthesized_expression", "binary_expression",
      "unary_expression", "ternary_expression", "call_expression",
      "member_expression", "subscript_expression", "new_expression",
      "sequence_expression", "assignment_expression",
      "augmented_assignment_expression", "update_expression", "identifier"};
  for (const char* candidate : kTypes) {
    if (std::strcmp(type, candidate) == 0) return true;
  }
  return false;
}

bool IsSafeSeedExpression(TSNode root, std::string_view source,
                          const std::vector<std::string>& allowed,
                          const GenContext& ctx, Budget* budget) {
  bool ok = true;
  Walk(root, budget, [&](TSNode node, TSNode) {
    if (ts_node_is_error(node) || ts_node_is_missing(node)) ok = false;
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "super") == 0 || std::strcmp(type, "await_expression") == 0 ||
        std::strcmp(type, "yield_expression") == 0 ||
        std::strcmp(type, "meta_property") == 0 ||
        std::strcmp(type, "private_property_identifier") == 0 ||
        IsFunctionType(type) || std::strcmp(type, "class") == 0) {
      ok = false;
    }
    // Generated code may place this expression in strict code (a class).
    if (HasLegacySyntax(node, source)) ok = false;
    if (std::strcmp(type, "unary_expression") == 0 && HasDirectToken(node, "delete")) {
      ok = false;
    }
    if (std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "shorthand_property_identifier") == 0) {
      const std::string name(NodeText(node, source));
      if (IsReservedContextualName(name) ||
          std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
        ok = false;
      }
    }
    return ok;
  });
  return ok && !budget->exhausted;
}

bool IsBlockType(const char* type) {
  return std::strcmp(type, "function_declaration") == 0 ||
         std::strcmp(type, "function_expression") == 0 ||
         std::strcmp(type, "generator_function_declaration") == 0 ||
         std::strcmp(type, "generator_function") == 0 ||
         std::strcmp(type, "class_declaration") == 0 ||
         std::strcmp(type, "class") == 0;
}

// A function or class that can be moved anywhere: it carries its own
// bindings and does not refer to private names of an enclosing class.
bool IsPortableBlock(TSNode node, std::string_view source, bool strict,
                     Budget* budget) {
  if (!IsBlockType(ts_node_type(node))) return false;
  if (ts_node_has_error(node)) return false;
  const size_t size = ts_node_end_byte(node) - ts_node_start_byte(node);
  if (size == 0 || size > kMaxBlockBytes) return false;
  // Private names declared inside the block itself are fine.
  std::vector<std::string> declared;
  Walk(node, budget, [&](TSNode n, TSNode) {
    if (Is(n, "class_body")) CollectPrivateDeclarations(n, source, &declared);
    return true;
  });
  const bool foreign_private = ContainsNode(node, budget, [&](TSNode n) {
    if (!Is(n, "private_property_identifier")) return false;
    const std::string name(NodeText(n, source));
    return std::find(declared.begin(), declared.end(), name) == declared.end();
  });
  if (foreign_private) return false;
  if (strict && HasSloppyOnly(node, source, budget)) return false;
  return !budget->exhausted;
}

constexpr std::string_view kGlobalNames[] = {
    "undefined", "NaN", "Infinity", "Object", "Array", "String", "Number",
    "Boolean", "Symbol", "Promise", "Map", "Set", "Math", "JSON", "Error",
    "RegExp", "Date", "Reflect", "Proxy", "globalThis"};

// Seed facts relative to `dest`.
void BuildSeedInfo(TSNode root, std::string_view source, TSNode dest,
                   const GenContext& ctx, Budget* budget, SeedInfo* info,
                   bool collect_expressions = true) {
  CollectAllNames(root, source, &info->reserved, budget);
  info->names = VisibleNames(dest, source, budget);
  if (budget->exhausted) return;
  std::vector<std::string> allowed = info->names;
  for (std::string_view name : kGlobalNames) allowed.emplace_back(name);
  Walk(root, budget, [&](TSNode node, TSNode) {
    const char* type = ts_node_type(node);
    const size_t size = ts_node_end_byte(node) - ts_node_start_byte(node);
    // A tagged template may hold escapes that are invalid without its tag.
    TSNode parent_node = ts_node_parent(node);
    const bool tagged_template = std::strcmp(type, "template_string") == 0 &&
                                 Is(parent_node, "call_expression");
    if (!tagged_template && collect_expressions &&
        info->expressions.size() < kMaxSeedExpressions &&
        IsReusableExpressionType(type) && size > 0 &&
        size <= kMaxReusableExpressionBytes &&
        IsSafeSeedExpression(node, source, allowed, ctx, budget)) {
      std::string text(NodeText(node, source));
      if (std::find(info->expressions.begin(), info->expressions.end(), text) ==
          info->expressions.end()) {
        info->expressions.push_back(std::move(text));
      }
    }
    // Blocks must be strict-safe: the generator may nest them in a class.
    if (info->blocks.size() < kMaxBlocks && IsBlockType(type) &&
        IsPortableBlock(node, source, true, budget)) {
      info->blocks.emplace_back(NodeText(node, source));
    }
    return !budget->exhausted;
  });
}

// ---------------------------------------------------------------------------
// Early (parse-time) errors that Tree-sitter's grammar does not report.

class EarlyErrors {
 public:
  EarlyErrors(std::string_view source, Budget* budget)
      : source_(source), budget_(budget) {}

  bool Check(TSNode root) {
    State state;
    Visit(root, state, 0);
    if (budget_->exhausted) error_ = true;  // unknown counts as invalid
    return error_;
  }

 private:
  struct State {
    bool in_function = false;
    unsigned loops = 0;
    unsigned switches = 0;
    std::vector<std::string> labels;
    std::vector<std::string> loop_labels;
    std::vector<std::vector<std::string>> privates;
    bool strict = false;
    bool generator = false;       // directly in a generator body
    bool await_reserved = false;  // async code, static blocks, class bodies
    bool yield_reserved = false;  // generator code
  };

  // Lexically declared names of one scope, for var-hoisting conflicts.
  struct LexicalFrame {
    std::set<std::string> names;
    bool boundary = false;      // a function or static block: vars stop here
    bool simple_catch = false;  // catch (e): `var e` in the body is allowed
  };

  std::string Text(TSNode node) { return std::string(NodeText(node, source_)); }

  // A labelled function declaration declares like a plain one.
  static TSNode Unlabel(TSNode statement) {
    for (unsigned i = 0; i < 64 && Is(statement, "labeled_statement"); ++i) {
      statement = Field(statement, "body");
    }
    return statement;
  }

  // In a statement list, `let` + line break + `x = 1` is one declaration to
  // V8, two statements to Tree-sitter. Returns false when V8 would read some
  // other declaration (or a syntax error); `*name` is empty when statement i
  // is not such a `let`.
  bool LetLineDeclaration(const std::vector<TSNode>& statements, size_t i,
                          std::string* name) {
    name->clear();
    if (!Is(statements[i], "expression_statement") ||
        NodeText(statements[i], source_) != "let" || i + 1 >= statements.size()) {
      return true;
    }
    TSNode next = statements[i + 1];
    TSNode expression = ts_node_named_child(next, 0);
    if (Is(next, "expression_statement") && Is(expression, "assignment_expression") &&
        Is(Field(expression, "left"), "identifier")) {
      *name = DecodeIdentifier(NodeText(Field(expression, "left"), source_));
      return !IsSpecialName(*name);
    }
    const std::string_view text = NodeText(next, source_);
    const char c = text.empty() ? '\0' : text[0];
    return !(std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
             c == '\\' || c == '[' || c == '{');
  }

  void CheckList(const std::vector<TSNode>& statements, bool function_level,
                 bool strict, const std::vector<std::string>& outer_lexical) {
    std::set<std::string> lexical(outer_lexical.begin(), outer_lexical.end());
    std::set<std::string> vars;
    for (size_t index = 0; index < statements.size(); ++index) {
      const TSNode labelled = statements[index];
      std::string let_name;
      if (!LetLineDeclaration(statements, index, &let_name)) error_ = true;
      if (!let_name.empty() && (!lexical.insert(let_name).second || vars.count(let_name) != 0)) {
        error_ = true;
      }
      std::vector<std::string> names;
      const TSNode statement = Unlabel(labelled);
      if (ts_node_is_null(statement)) continue;
      const char* type = ts_node_type(statement);
      const bool function = std::strcmp(type, "function_declaration") == 0 ||
                            std::strcmp(type, "generator_function_declaration") == 0;
      const bool plain = std::strcmp(type, "function_declaration") == 0 &&
                         !HasDirectToken(statement, "async");
      if (std::strcmp(type, "lexical_declaration") == 0 ||
          std::strcmp(type, "class_declaration") == 0 ||
          (function && (!function_level))) {
        CollectRaw(statement, &names);
        for (const std::string& name : names) {
          // Sloppy Annex B permits duplicate plain function declarations in
          // blocks; everything else is a redeclaration error.
          if (plain && !strict && plain_functions_.count(name) != 0 &&
              lexical.count(name) != 0) {
            continue;
          }
          if (!lexical.insert(name).second || vars.count(name) != 0) error_ = true;
          if (plain) plain_functions_.insert(name);
        }
      } else if (std::strcmp(type, "variable_declaration") == 0 || function) {
        CollectRaw(statement, &names);
        for (const std::string& name : names) {
          if (lexical.count(name) != 0 && outer_lexical_params_.count(name) == 0) {
            error_ = true;
          }
          vars.insert(name);
        }
      }
    }
    plain_functions_.clear();
  }

  void CollectRaw(TSNode statement, std::vector<std::string>* names) {
    const char* type = ts_node_type(statement);
    if (std::strcmp(type, "lexical_declaration") == 0 ||
        std::strcmp(type, "variable_declaration") == 0) {
      const uint32_t count = ts_node_named_child_count(statement);
      for (uint32_t i = 0; i < count; ++i) {
        TSNode declarator = ts_node_named_child(statement, i);
        if (Is(declarator, "variable_declarator")) {
          CollectPattern(Field(declarator, "name"), names, 0);
        }
      }
    } else {
      TSNode name = Field(statement, "name");
      if (!ts_node_is_null(name)) names->push_back(DecodeIdentifier(Text(name)));
    }
  }

  void CollectPattern(TSNode node, std::vector<std::string>* names, unsigned depth) {
    if (ts_node_is_null(node) || depth > 64) return;
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "shorthand_property_identifier_pattern") == 0) {
      names->push_back(DecodeIdentifier(Text(node)));
      return;
    }
    if (std::strcmp(type, "assignment_pattern") == 0 ||
        std::strcmp(type, "object_assignment_pattern") == 0) {
      CollectPattern(Field(node, "left"), names, depth + 1);
      return;
    }
    if (std::strcmp(type, "pair_pattern") == 0) {
      CollectPattern(Field(node, "value"), names, depth + 1);
      return;
    }
    if (std::strcmp(type, "computed_property_name") == 0) return;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      CollectPattern(ts_node_named_child(node, i), names, depth + 1);
    }
  }

  // The names a scope node declares lexically, or false when the node is not
  // a scope with lexical declarations.
  bool LexicalFrameFor(TSNode node, LexicalFrame* frame) {
    const char* type = ts_node_type(node);
    std::vector<std::string> names;
    auto add_statement = [&](TSNode labelled, bool functions) {
      TSNode statement = Unlabel(labelled);
      if (Is(statement, "lexical_declaration") || Is(statement, "class_declaration") ||
          (functions && (Is(statement, "function_declaration") ||
                         Is(statement, "generator_function_declaration")))) {
        CollectRaw(statement, &names);
      }
    };
    if (std::strcmp(type, "program") == 0 || std::strcmp(type, "statement_block") == 0) {
      TSNode parent = ts_node_parent(node);
      const bool function_level = std::strcmp(type, "program") == 0 ||
                                  (!ts_node_is_null(parent) &&
                                   IsFunctionType(ts_node_type(parent)));
      const auto statements = ListStatements(node);
      for (size_t i = 0; i < statements.size(); ++i) {
        add_statement(statements[i], !function_level);
        std::string let_name;
        LetLineDeclaration(statements, i, &let_name);
        if (!let_name.empty()) names.push_back(let_name);
      }
    } else if (std::strcmp(type, "switch_body") == 0) {
      const uint32_t cases = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < cases; ++i) {
        TSNode clause = ts_node_named_child(node, i);
        const uint32_t count = ts_node_named_child_count(clause);
        for (uint32_t j = 0; j < count; ++j) add_statement(ts_node_named_child(clause, j), true);
      }
    } else if (std::strcmp(type, "for_statement") == 0) {
      TSNode init = Field(node, "initializer");
      if (!Is(init, "lexical_declaration")) return false;
      CollectRaw(init, &names);
    } else if (std::strcmp(type, "for_in_statement") == 0) {
      TSNode kind = Field(node, "kind");
      if (ts_node_is_null(kind) || NodeText(kind, source_) == "var") return false;
      CollectPattern(Field(node, "left"), &names, 0);
    } else if (std::strcmp(type, "catch_clause") == 0) {
      TSNode param = Field(node, "parameter");
      CollectPattern(param, &names, 0);
      frame->simple_catch = Is(param, "identifier");
    } else {
      return false;
    }
    frame->names.insert(names.begin(), names.end());
    return true;
  }

  // A var-declared name conflicts with every lexical declaration of the same
  // name in the scopes it hoists through.
  void CheckVarNames(const std::vector<std::string>& names) {
    for (const std::string& name : names) {
      for (size_t i = frames_.size(); i-- > 0;) {
        const LexicalFrame& frame = frames_[i];
        if (frame.boundary) break;
        if (frame.names.count(name) != 0 && !frame.simple_catch) error_ = true;
      }
    }
  }

  // An optional chain along the object path of a member or call.
  static bool HasOptionalChain(TSNode node) {
    for (unsigned i = 0; i < 4096 && !ts_node_is_null(node); ++i) {
      if (HasDirectToken(node, "optional_chain") || HasDirectToken(node, "?.")) return true;
      if (Is(node, "member_expression") || Is(node, "subscript_expression")) {
        node = Field(node, "object");
      } else if (Is(node, "call_expression")) {
        node = Field(node, "function");
      } else {
        break;
      }
    }
    return false;
  }

  // Valid targets of `=`, compound assignment, update, and for-in/of heads.
  enum class TargetOp { kAssign, kCompound, kLogical, kUpdate };
  void CheckTarget(TSNode target, const State& state, TargetOp op) {
    const bool logical = op == TargetOp::kLogical;
    TSNode inner = SkipParens(target);
    const bool parenthesized = !ts_node_eq(inner, target);
    const char* type = ts_node_type(inner);
    if (std::strcmp(type, "identifier") == 0) {
      if (state.strict && IsEvalOrArguments(DecodeIdentifier(Text(inner)))) error_ = true;
      return;
    }
    if (std::strcmp(type, "member_expression") == 0 ||
        std::strcmp(type, "subscript_expression") == 0) {
      // An optional chain is never a target.
      if (HasOptionalChain(inner)) error_ = true;
      return;
    }
    if (std::strcmp(type, "call_expression") == 0) {
      TSNode callee = Field(inner, "function");
      const bool tagged = Is(Field(inner, "arguments"), "template_string");
      if (state.strict || logical || tagged || Is(callee, "import") ||
          Is(callee, "super") || HasOptionalChain(inner)) {
        error_ = true;
      }
      return;
    }
    // Patterns are targets only without parentheses.
    if (!parenthesized && (std::strcmp(type, "object_pattern") == 0 ||
                           std::strcmp(type, "array_pattern") == 0 ||
                           std::strcmp(type, "object") == 0 ||
                           std::strcmp(type, "array") == 0)) {
      if (op != TargetOp::kAssign) error_ = true;
      return;
    }
    error_ = true;
  }

  void CheckIdentifier(TSNode node, const State& state) {
    const std::string_view raw = NodeText(node, source_);
    const std::string name = DecodeIdentifier(raw);
    // Tree-sitter reads `for (let [a] of b)` as a `let[a]` target.
    if (name == "let") {
      TSNode parent = ts_node_parent(node);
      if (Is(parent, "subscript_expression") &&
          Is(ts_node_parent(parent), "for_in_statement")) {
        return;
      }
    }
    if (raw.find('\\') != std::string_view::npos && IsAlwaysReservedWord(name)) {
      error_ = true;
    } else if (state.strict && IsStrictReservedWord(name)) {
      error_ = true;
    } else if (state.await_reserved && name == "await") {
      error_ = true;
    } else if (state.yield_reserved && name == "yield") {
      error_ = true;
    } else if (state.strict && IsEvalOrArguments(name) && IsBindingOrTarget(node)) {
      error_ = true;
    }
  }

  void CheckFunction(TSNode node, const State& inner) {
    std::vector<std::string> params;
    ParameterNames(node, source_, &params);
    const bool simple = HasSimpleParameters(node);
    TSNode body = Field(node, "body");
    const bool own_strict = Is(body, "statement_block") && ListIsStrict(body, source_);
    // "use strict" requires a simple parameter list.
    if (own_strict && !simple) error_ = true;
    if (!HasDuplicate(params)) return;
    const char* type = ts_node_type(node);
    const bool plain = std::strcmp(type, "function_declaration") == 0 ||
                       std::strcmp(type, "function_expression") == 0 ||
                       std::strcmp(type, "generator_function_declaration") == 0 ||
                       std::strcmp(type, "generator_function") == 0;
    if (!plain || !simple || inner.strict) error_ = true;
  }

  // Function declarations as the body of if, labels, or loops (Annex B).
  void CheckStatementPosition(TSNode node, const State& state) {
    const char* type = ts_node_type(node);
    const bool annex_b = std::strcmp(type, "if_statement") == 0 ||
                         std::strcmp(type, "else_clause") == 0 ||
                         std::strcmp(type, "labeled_statement") == 0;
    const bool loop = IsLoopType(type) || std::strcmp(type, "with_statement") == 0;
    if (!annex_b && !loop) return;
    TSNode bodies[] = {Field(node, "consequence"), Field(node, "body"),
                       std::strcmp(type, "else_clause") == 0 ? ts_node_named_child(node, 0)
                                                             : TSNode{}};
    for (TSNode body : bodies) {
      if (Is(body, "generator_function_declaration") || Is(body, "class_declaration") ||
          Is(body, "lexical_declaration")) {
        error_ = true;
      } else if (Is(body, "function_declaration")) {
        if (loop || state.strict || HasDirectToken(body, "async")) error_ = true;
      }
    }
  }

  bool PrivateVisible(const State& state, const std::string& name) {
    for (const auto& scope : state.privates) {
      if (std::find(scope.begin(), scope.end(), name) != scope.end()) return true;
    }
    return false;
  }

  void CheckClassBody(TSNode body) {
    struct Entry {
      std::string name;
      int kind;  // 0 other, 1 get, 2 set
      bool is_static;
    };
    std::vector<Entry> entries;
    const uint32_t count = ts_node_named_child_count(body);
    bool constructor = false;
    for (uint32_t i = 0; i < count; ++i) {
      TSNode member = ts_node_named_child(body, i);
      if (!Is(member, "field_definition") && !Is(member, "method_definition")) continue;
      const std::string name = MemberName(member, source_);
      const bool is_static = IsStaticMember(member);
      if (Is(member, "method_definition") && !is_static &&
          (name == "constructor" || name == "'constructor'" ||
           name == "\"constructor\"")) {
        if (constructor) error_ = true;
        constructor = true;
      }
      if (name.empty() || name[0] != '#') continue;
      const int kind = HasDirectToken(member, "get") ? 1
                       : HasDirectToken(member, "set") ? 2 : 0;
      for (const Entry& entry : entries) {
        if (entry.name != name) continue;
        const bool pair = entry.kind != 0 && kind != 0 && entry.kind != kind &&
                          entry.is_static == is_static;
        if (!pair) error_ = true;
      }
      entries.push_back({name, kind, is_static});
    }
  }

  void Visit(TSNode node, State& state, unsigned depth) {
    if (error_ || ts_node_is_null(node) || depth > kMaxCheckDepth) return;
    if (budget_->node_visits >= kMaxNodeVisits) {
      budget_->exhausted = true;
      return;
    }
    ++budget_->node_visits;
    const char* type = ts_node_type(node);

    if (std::strcmp(type, "break_statement") == 0) {
      TSNode label = Field(node, "label");
      if (!ts_node_is_null(label)) {
        const std::string name = Text(label);
        if (std::find(state.labels.begin(), state.labels.end(), name) ==
            state.labels.end()) {
          error_ = true;
        }
      } else if (state.loops == 0 && state.switches == 0) {
        error_ = true;
      }
      return;
    }
    if (std::strcmp(type, "continue_statement") == 0) {
      TSNode label = Field(node, "label");
      if (!ts_node_is_null(label)) {
        const std::string name = Text(label);
        if (std::find(state.loop_labels.begin(), state.loop_labels.end(), name) ==
            state.loop_labels.end()) {
          error_ = true;
        }
      } else if (state.loops == 0) {
        error_ = true;
      }
      return;
    }
    if (std::strcmp(type, "return_statement") == 0 && !state.in_function) {
      error_ = true;
      return;
    }
    if (std::strcmp(type, "private_property_identifier") == 0) {
      if (!PrivateVisible(state, Text(node))) error_ = true;
      return;
    }

    if (std::strcmp(type, "program") == 0 || std::strcmp(type, "statement_block") == 0) {
      TSNode parent = ts_node_parent(node);
      const bool function_level = std::strcmp(type, "program") == 0 ||
                                  (!ts_node_is_null(parent) &&
                                   IsFunctionType(ts_node_type(parent)));
      std::vector<std::string> outer;
      outer_lexical_params_.clear();
      if (!ts_node_is_null(parent) && IsFunctionType(ts_node_type(parent))) {
        // Parameters conflict with lexical declarations in the body.
        CollectPattern(Field(parent, "parameters"), &outer, 0);
        CollectPattern(Field(parent, "parameter"), &outer, 0);
        outer_lexical_params_.insert(outer.begin(), outer.end());
      } else if (Is(parent, "catch_clause")) {
        CollectPattern(Field(parent, "parameter"), &outer, 0);
        outer_lexical_params_.insert(outer.begin(), outer.end());
      }
      CheckList(ListStatements(node), function_level, ContextFor(node, source_).strict,
                outer);
      outer_lexical_params_.clear();
    } else if (std::strcmp(type, "switch_body") == 0) {
      std::vector<TSNode> statements;
      const uint32_t cases = ts_node_named_child_count(node);
      for (uint32_t i = 0; i < cases; ++i) {
        TSNode clause = ts_node_named_child(node, i);
        TSNode value = Field(clause, "value");
        const uint32_t count = ts_node_named_child_count(clause);
        for (uint32_t j = 0; j < count; ++j) {
          TSNode child = ts_node_named_child(clause, j);
          if (!ts_node_eq(child, value) && !IsExtra(child)) statements.push_back(child);
        }
      }
      CheckList(statements, false, ContextFor(node, source_).strict, {});
    } else if (std::strcmp(type, "class_body") == 0) {
      CheckClassBody(node);
    }

    State inner = state;
    bool changed = false;
    if (IsFunctionType(type)) {
      inner.in_function = true;
      inner.loops = 0;
      inner.switches = 0;
      inner.labels.clear();
      inner.loop_labels.clear();
      changed = true;
    } else if (std::strcmp(type, "class_static_block") == 0) {
      inner.in_function = false;
      inner.loops = 0;
      inner.switches = 0;
      inner.labels.clear();
      inner.loop_labels.clear();
      changed = true;
    } else if (IsLoopType(type)) {
      ++inner.loops;
      changed = true;
    } else if (std::strcmp(type, "switch_statement") == 0) {
      ++inner.switches;
      changed = true;
    } else if (std::strcmp(type, "labeled_statement") == 0) {
      const std::string label = Text(Field(node, "label"));
      if (std::find(inner.labels.begin(), inner.labels.end(), label) !=
          inner.labels.end()) {
        error_ = true;
        return;
      }
      inner.labels.push_back(label);
      TSNode body = Field(node, "body");
      if (!ts_node_is_null(body) && IsLoopType(ts_node_type(body))) {
        inner.loop_labels.push_back(label);
      }
      changed = true;
    } else if (std::strcmp(type, "class_body") == 0) {
      std::vector<std::string> declared;
      CollectPrivateDeclarations(node, source_, &declared);
      inner.privates.push_back(declared);
      changed = true;
    }
    if (UpdateNameState(node, &inner)) changed = true;
    State& next = changed ? inner : state;
    CheckNames(node, state, next);
    if (error_) return;

    // Scopes whose lexical names conflict with var declarations inside.
    const size_t frames = frames_.size();
    if (IsFunctionType(type) || std::strcmp(type, "class_static_block") == 0) {
      frames_.emplace_back();
      frames_.back().boundary = true;
    }
    LexicalFrame frame;
    if (LexicalFrameFor(node, &frame)) frames_.push_back(std::move(frame));

    // A method's computed key is outside its (possibly strict) body.
    const TSNode method_key = std::strcmp(type, "method_definition") == 0
                                  ? Field(node, "name") : TSNode{};
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count && !error_; ++i) {
      TSNode child = ts_node_named_child(node, i);
      // Declaration names of private members are not references.
      if (Is(child, "private_property_identifier") &&
          (std::strcmp(type, "field_definition") == 0 ||
           std::strcmp(type, "method_definition") == 0)) {
        continue;
      }
      Visit(child, !ts_node_is_null(method_key) && ts_node_eq(child, method_key) ? state : next,
            depth + 1);
    }
    frames_.resize(frames);
  }

  // Strictness and whether await/yield are reserved inside `node`.
  bool UpdateNameState(TSNode node, State* inner) {
    const char* type = ts_node_type(node);
    const bool module = source_.substr(0, 5) == "/*M*/";
    if (std::strcmp(type, "program") == 0) {
      inner->strict = module || ListIsStrict(node, source_);
      inner->await_reserved = module;
      return true;
    }
    if (IsFunctionType(type)) {
      TSNode body = Field(node, "body");
      if (Is(body, "statement_block") && ListIsStrict(body, source_)) inner->strict = true;
      if (std::strcmp(type, "arrow_function") == 0) {
        inner->generator = false;
      } else {
        inner->generator = IsGeneratorFunction(node);
        inner->yield_reserved = inner->generator;
        inner->await_reserved = module;
      }
      if (IsAsyncFunction(node)) inner->await_reserved = true;
      return true;
    }
    if (std::strcmp(type, "class") == 0 || std::strcmp(type, "class_declaration") == 0) {
      inner->strict = true;
      return true;
    }
    if (std::strcmp(type, "class_body") == 0) {
      inner->generator = false;
      return true;
    }
    if (std::strcmp(type, "class_static_block") == 0) {
      inner->generator = false;
      inner->await_reserved = true;
      return true;
    }
    return false;
  }

  // Identifier, assignment-target, parameter, and declaration rules.
  void CheckNames(TSNode node, const State& state, const State& inner) {
    const char* type = ts_node_type(node);
    if (IsIdentifierNode(node) || std::strcmp(type, "statement_identifier") == 0) {
      CheckIdentifier(node, state);
    } else if (std::strcmp(type, "yield_expression") == 0) {
      // Outside generators this is the identifier `yield`.
      if (state.strict && !state.generator) error_ = true;
    } else if (std::strcmp(type, "assignment_expression") == 0) {
      CheckTarget(Field(node, "left"), state, TargetOp::kAssign);
    } else if (std::strcmp(type, "augmented_assignment_expression") == 0) {
      const std::string_view op = NodeText(Field(node, "operator"), source_);
      CheckTarget(Field(node, "left"), state,
                  op == "&&=" || op == "||=" || op == "?\?=" ? TargetOp::kLogical
                                                             : TargetOp::kCompound);
    } else if (std::strcmp(type, "update_expression") == 0) {
      CheckTarget(Field(node, "argument"), state, TargetOp::kUpdate);
    } else if (std::strcmp(type, "for_in_statement") == 0) {
      TSNode kind = Field(node, "kind");
      if (ts_node_is_null(kind)) {
        CheckTarget(Field(node, "left"), state, TargetOp::kAssign);
      } else if (NodeText(kind, source_) == "var") {
        std::vector<std::string> names;
        CollectPattern(Field(node, "left"), &names, 0);
        CheckVarNames(names);
      } else {
        std::vector<std::string> names;
        CollectPattern(Field(node, "left"), &names, 0);
        if (std::find(names.begin(), names.end(), "let") != names.end()) error_ = true;
      }
    } else if (std::strcmp(type, "variable_declaration") == 0) {
      std::vector<std::string> names;
      CollectRaw(node, &names);
      CheckVarNames(names);
    } else if (std::strcmp(type, "lexical_declaration") == 0) {
      std::vector<std::string> names;
      CollectRaw(node, &names);
      if (std::find(names.begin(), names.end(), "let") != names.end()) error_ = true;
    } else if (std::strcmp(type, "catch_clause") == 0) {
      // A destructured catch parameter is lexically bound; a plain one is not.
      TSNode param = Field(node, "parameter");
      std::vector<std::string> names;
      if (!Is(param, "identifier")) CollectPattern(param, &names, 0);
      if (std::find(names.begin(), names.end(), "let") != names.end()) error_ = true;
    } else if (IsFunctionType(type)) {
      CheckFunction(node, inner);
    }
    CheckStatementPosition(node, state);
  }

  std::string_view source_;
  Budget* budget_;
  bool error_ = false;
  std::set<std::string> plain_functions_;
  std::set<std::string> outer_lexical_params_;
  std::vector<LexicalFrame> frames_;
};

bool HasEarlyError(TSNode root, std::string_view source, Budget* budget) {
  EarlyErrors checker(source, budget);
  return checker.Check(root);
}

}  // namespace

namespace internal {

bool SourceHasEarlyError(std::string_view source) {
  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_javascript())) return true;
  Budget budget;
  Tree tree = Parse(parser.get(), source, &budget);
  if (!tree || budget.exhausted) return true;
  const TSNode root = ts_tree_root_node(tree.get());
  const auto errors = CountSyntaxErrors(root, &budget);
  return !errors || *errors != 0 || HasEarlyError(root, source, &budget);
}

}  // namespace internal

namespace {

// ---------------------------------------------------------------------------
// Positions.

struct InsertionPoint {
  size_t position;
  TSNode list;
  bool at_end;
};

std::vector<InsertionPoint> InsertionPoints(TSNode root, std::string_view source,
                                            Budget* budget) {
  std::vector<InsertionPoint> points;
  Walk(root, budget, [&](TSNode node, TSNode) {
    const bool program = Is(node, "program");
    if (!program && !Is(node, "statement_block")) return true;
    if (ts_node_has_error(node) && !program) return true;
    const size_t end = ts_node_end_byte(node);
    if (!program && (end == 0 || end > source.size() || source[end - 1] != '}')) {
      return true;
    }
    const auto statements = ListStatements(node);
    const size_t prologue = PrologueLength(statements);
    for (size_t i = prologue; i < statements.size(); ++i) {
      points.push_back({ts_node_start_byte(statements[i]), node, false});
    }
    points.push_back({program ? source.size() : end - 1, node, true});
    return points.size() < 4096;
  });
  // Never insert before a hashbang line.
  if (source.substr(0, 2) == "#!") {
    const size_t line_end = source.find('\n');
    const size_t floor = line_end == std::string_view::npos ? source.size() : line_end + 1;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [&](const InsertionPoint& p) { return p.position < floor; }),
                 points.end());
  }
  return points;
}

std::string InsertStatement(std::string_view source, const InsertionPoint& point,
                            std::string statement) {
  // A statement starting with one of these could continue a previous
  // statement that relies on automatic semicolon insertion.
  if (!statement.empty() && std::strchr("([`+-/", statement[0]) != nullptr) {
    statement = ";" + statement;
  }
  std::string out(source.substr(0, point.position));
  if (point.at_end) {
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += statement;
    out += '\n';
  } else {
    out += statement;
    out += '\n';
  }
  out.append(source.substr(point.position));
  return out;
}

// Positions where an expression may be replaced by another expression.
std::vector<TSNode> ExpressionCandidates(TSNode root, Budget* budget) {
  static constexpr std::array<const char*, 28> kValueTypes = {
      "identifier", "number", "string", "true", "false", "null", "undefined",
      "regex", "template_string", "array", "object", "parenthesized_expression",
      "binary_expression", "unary_expression", "ternary_expression",
      "call_expression", "member_expression", "subscript_expression",
      "new_expression", "sequence_expression", "function_expression",
      "generator_function", "arrow_function", "class", "yield_expression",
      "await_expression", "this", "update_expression"};
  static constexpr std::array<const char*, 27> kValueParents = {
      "arguments", "expression_statement", "return_statement", "throw_statement",
      "variable_declarator", "binary_expression", "unary_expression",
      "ternary_expression", "array", "pair", "subscript_expression",
      "parenthesized_expression", "template_substitution", "if_statement",
      "while_statement", "do_statement", "switch_statement", "switch_case",
      "yield_expression", "await_expression", "spread_element",
      "sequence_expression", "arrow_function", "field_definition",
      "assignment_expression", "augmented_assignment_expression",
      "assignment_pattern"};
  auto in = [](const char* type, const auto& list) {
    for (const char* candidate : list) {
      if (std::strcmp(type, candidate) == 0) return true;
    }
    return false;
  };
  std::vector<TSNode> out;
  Walk(root, budget, [&](TSNode node, TSNode parent) {
    if (out.size() >= 256) return false;
    if (ts_node_is_null(parent) || ts_node_has_error(node)) return true;
    const char* type = ts_node_type(node);
    const char* parent_type = ts_node_type(parent);
    if (!in(type, kValueTypes) || !in(parent_type, kValueParents)) return true;
    // Only value positions: not names, keys, or assignment targets.
    if (Is(parent, "variable_declarator") && !ts_node_eq(node, Field(parent, "value"))) {
      return true;
    }
    if ((Is(parent, "assignment_expression") ||
         Is(parent, "augmented_assignment_expression") ||
         Is(parent, "assignment_pattern")) &&
        !ts_node_eq(node, Field(parent, "right"))) {
      return true;
    }
    if (Is(parent, "pair") && !ts_node_eq(node, Field(parent, "value"))) return true;
    if (Is(parent, "field_definition") && !ts_node_eq(node, Field(parent, "value"))) {
      return true;
    }
    if (Is(parent, "arrow_function") && !ts_node_eq(node, Field(parent, "body"))) {
      return true;
    }
    if (Is(parent, "unary_expression") && HasDirectToken(parent, "delete")) return true;
    if (Is(parent, "subscript_expression") && !ts_node_eq(node, Field(parent, "index"))) {
      return true;
    }
    // A member/call object must stay a valid callee; avoid the optional
    // chain head and assignment-target subtrees.
    for (TSNode a = parent; !ts_node_is_null(a); a = ts_node_parent(a)) {
      if (Is(a, "for_in_statement") && ts_node_start_byte(node) <
                                           ts_node_end_byte(Field(a, "left"))) {
        return true;
      }
      if (Is(a, "update_expression")) return true;
      // Assignment targets must stay targets.
      if ((Is(a, "assignment_expression") ||
           Is(a, "augmented_assignment_expression")) &&
          ts_node_end_byte(node) <= ts_node_end_byte(Field(a, "left"))) {
        return true;
      }
      if (Is(a, "statement_block") || Is(a, "program")) break;
    }
    out.push_back(node);
    return true;
  });
  return out;
}

// ---------------------------------------------------------------------------
// Edits. Each returns a candidate program or nothing.

struct Mutation {
  std::string_view source;
  TSNode root;
  Budget* budget;
  Random* random;
  size_t limit;
  TSNode donor_root{};
  std::string_view donor_source;
};

std::string UniqueName(std::string_view base, const SeedInfo& seed,
                       std::initializer_list<std::string_view> texts,
                       uint32_t salt) {
  for (uint32_t n = salt % 97;; ++n) {
    std::string name = std::string(base) + std::to_string(n);
    if (std::find(seed.reserved.begin(), seed.reserved.end(), name) !=
        seed.reserved.end()) {
      continue;
    }
    bool clash = false;
    for (std::string_view text : texts) {
      if (text.find(name) != std::string_view::npos) clash = true;
    }
    if (!clash) return name;
  }
}

std::optional<std::string> Replace(std::string_view source, size_t start,
                                   size_t end, std::string_view text,
                                   size_t limit) {
  if (start > end || end > source.size()) return std::nullopt;
  const size_t size = source.size() - (end - start) + text.size();
  if (size > limit) return std::nullopt;
  std::string out;
  out.reserve(size);
  out.append(source.substr(0, start));
  out.append(text);
  out.append(source.substr(end));
  return out;
}

template <typename T>
const T* PickFrom(const std::vector<T>& items, Random* random) {
  if (items.empty()) return nullptr;
  return &items[random->Below(static_cast<uint32_t>(items.size()))];
}

std::optional<std::string> EditLiteral(Mutation& m) {
  std::vector<TSNode> literals;
  Walk(m.root, m.budget, [&](TSNode node, TSNode parent) {
    const char* type = ts_node_type(node);
    const bool literal = std::strcmp(type, "number") == 0 ||
                         std::strcmp(type, "string") == 0 ||
                         std::strcmp(type, "regex") == 0 ||
                         std::strcmp(type, "true") == 0 ||
                         std::strcmp(type, "false") == 0 ||
                         std::strcmp(type, "null") == 0 ||
                         (std::strcmp(type, "template_string") == 0 &&
                          ts_node_named_child_count(node) == 0);
    // Class member names are not values: renaming 'constructor' changes
    // which method may call super().
    const bool member_name = Is(parent, "method_definition") ||
                             Is(parent, "field_definition");
    if (literal && !member_name && !ts_node_has_error(node) &&
        !Is(parent, "import_statement")) {
      literals.push_back(node);
    }
    return literals.size() < 512;
  });
  const TSNode* node = PickFrom(literals, m.random);
  if (!node) return std::nullopt;
  const GenContext ctx = ContextFor(*node, m.source);
  Generator generator(m.random, m.limit, SeedInfo{});
  TSNode call = ts_node_parent(*node);
  const bool tagged = Is(call, "call_expression") &&
                      ts_node_eq(*node, Field(call, "arguments"));
  std::string text = generator.Literal(ts_node_type(*node), ctx.strict, tagged);
  if (text.empty() || text == NodeText(*node, m.source)) return std::nullopt;
  // A number followed directly by `.` would absorb it as a decimal point.
  const size_t end = ts_node_end_byte(*node);
  if (Is(*node, "number") && end < m.source.size() && m.source[end] == '.') {
    text = "(" + text + ")";
  }
  return Replace(m.source, ts_node_start_byte(*node), end, text, m.limit);
}

std::optional<std::string> EditOperator(Mutation& m) {
  static constexpr std::string_view kArithmetic[] = {
      "+", "-", "*", "/", "%", "<<", ">>", ">>>", "&", "|", "^", "==", "!=",
      "===", "!==", "<", ">", "<=", ">=", "instanceof"};
  static constexpr std::string_view kUnary[] = {"!", "~", "-", "+", "typeof", "void"};
  static constexpr std::string_view kAugmented[] = {
      "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=",
      "^=", "&&=", "||=", "?\?="};
  auto contains = [](const auto& list, std::string_view text) {
    for (std::string_view item : list) {
      if (item == text) return true;
    }
    return false;
  };
  struct Site {
    TSNode token;
    int family;  // 0 arithmetic, 1 logical, 2 unary, 3 update, 4 augmented
  };
  std::vector<Site> sites;
  Walk(m.root, m.budget, [&](TSNode node, TSNode parent) {
    if (ts_node_has_error(node)) return true;
    TSNode op = Field(node, "operator");
    if (ts_node_is_null(op)) return true;
    const std::string_view text = NodeText(op, m.source);
    auto private_operand = [&](TSNode n) {
      return Is(n, "private_property_identifier") ||
             (Is(n, "binary_expression") &&
              Is(Field(n, "left"), "private_property_identifier"));
    };
    if (Is(node, "binary_expression") &&
        (private_operand(Field(node, "left")) || private_operand(Field(node, "right")))) {
      return true;
    }
    if (Is(node, "binary_expression")) {
      if (contains(kArithmetic, text) || text == "**") {
        sites.push_back({op, 0});
      } else if (text == "&&" || text == "||") {
        // Never create a ?? mixed with && or || without parentheses.
        auto coalesce = [&](TSNode n) {
          return Is(n, "binary_expression") &&
                 NodeText(Field(n, "operator"), m.source) == "??";
        };
        if (!coalesce(parent) && !coalesce(Field(node, "left")) &&
            !coalesce(Field(node, "right"))) {
          sites.push_back({op, 1});
        }
      }
    } else if (Is(node, "unary_expression") && contains(kUnary, text)) {
      sites.push_back({op, 2});
    } else if (Is(node, "update_expression")) {
      sites.push_back({op, 3});
    } else if (Is(node, "augmented_assignment_expression") && contains(kAugmented, text)) {
      sites.push_back({op, 4});
    }
    return sites.size() < 512;
  });
  const Site* site = PickFrom(sites, m.random);
  if (!site) return std::nullopt;
  const std::string_view old_text = NodeText(site->token, m.source);
  std::string text;
  switch (site->family) {
    case 0:
      text = m.random->Pick(kArithmetic);
      break;
    case 1:
      text = old_text == "&&" ? "||" : "&&";
      break;
    case 2:
      text = m.random->Pick(kUnary);
      break;
    case 3:
      text = old_text == "++" ? "--" : "++";
      break;
    default:
      text = m.random->Pick(kAugmented);
      break;
  }
  if (text == old_text) return std::nullopt;
  const size_t start = ts_node_start_byte(site->token);
  const size_t end = ts_node_end_byte(site->token);
  // Keep word operators separate from their operands.
  if ((text == "typeof" || text == "void" || text == "instanceof") &&
      end < m.source.size() && m.source[end] != ' ') {
    text += ' ';
  }
  if (text == "instanceof" && start > 0 && m.source[start - 1] != ' ') text = " " + text;
  // `-` before `-x` would become the decrement operator.
  // `-` next to `-x` (or `-` next to `++`) would merge into other tokens.
  auto sign = [](char c) { return c == '-' || c == '+'; };
  if (sign(text.back()) && end < m.source.size() && sign(m.source[end])) text += ' ';
  if (sign(text.front()) && start > 0 && sign(m.source[start - 1])) text = " " + text;
  return Replace(m.source, start, end, text, m.limit);
}

std::optional<std::string> EditGenerateStatement(Mutation& m) {
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  const GenContext ctx = ContextFor(point->list, m.source);
  SeedInfo seed;
  BuildSeedInfo(m.root, m.source, point->list, ctx, m.budget, &seed);
  if (m.budget->exhausted || m.source.size() + 2 >= m.limit) return std::nullopt;
  Generator generator(m.random, m.limit - m.source.size() - 2, seed);
  std::string statement;
  if (!generator.Statement(&statement, ctx, 2 + m.random->Below(6))) return std::nullopt;
  std::string out = InsertStatement(m.source, *point, statement);
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

std::optional<std::string> EditGenerateExpression(Mutation& m) {
  auto candidates = ExpressionCandidates(m.root, m.budget);
  const TSNode* node = PickFrom(candidates, m.random);
  if (!node) return std::nullopt;
  const GenContext ctx = ContextFor(*node, m.source);
  SeedInfo seed;
  BuildSeedInfo(m.root, m.source, *node, ctx, m.budget, &seed);
  const size_t start = ts_node_start_byte(*node);
  const size_t end = ts_node_end_byte(*node);
  const size_t unchanged = m.source.size() - (end - start);
  if (m.budget->exhausted || unchanged >= m.limit) return std::nullopt;
  Generator generator(m.random, m.limit - unchanged, seed);
  std::string expression;
  if (!generator.Expression(&expression, ctx, 1 + m.random->Below(6))) {
    return std::nullopt;
  }
  return Replace(m.source, start, end, expression, m.limit);
}

std::optional<std::string> EditWrapExpression(Mutation& m) {
  auto candidates = ExpressionCandidates(m.root, m.budget);
  const TSNode* node = PickFrom(candidates, m.random);
  if (!node) return std::nullopt;
  const GenContext ctx = ContextFor(*node, m.source);
  const Dependencies deps = FindDependencies(*node, m.source, m.budget);
  const std::string e = "(" + std::string(NodeText(*node, m.source)) + ")";
  SeedInfo seed;
  BuildSeedInfo(m.root, m.source, *node, ctx, m.budget, &seed);
  Generator generator(m.random, 512, seed);
  std::string other;
  if (!generator.Expression(&other, ctx, 2)) other = "0";
  const std::string param = UniqueName("p", seed, {e, other}, m.random->Next());
  const std::string key = UniqueName("k", seed, {e, other}, m.random->Next());
  std::vector<std::string> forms = {
      e, "[" + e + "][0]", "({" + key + ": " + e + "})." + key, "`${" + e + "}`",
      "(0, " + e + ")", "(" + other + " ? " + e + " : " + other + ")",
      "(" + e + " ?? " + other + ")", "(" + other + ", " + e + ")",
      "(void " + e + ", " + e + ")", "({..." + e + "})", "[...[" + e + "]][0]"};
  const bool arrow_ok = !deps.await_ && !deps.yield_;
  if (arrow_ok) {
    forms.push_back("(() => " + e + ")()");
    forms.push_back("((" + param + " = " + e + ") => " + param + ")()");
  }
  if (!deps.yield_ && ctx.async) forms.push_back("(await (async () => " + e + ")())");
  // A class body is strict, so sloppy-only code cannot move into one.
  const bool strict_safe = ctx.strict || !HasSloppyOnly(*node, m.source, m.budget);
  if (arrow_ok && !deps.arguments && !deps.super_call && strict_safe) {
    forms.push_back("(class { static " + key + " = " + e + "; })." + key);
  }
  if (arrow_ok && !deps.arguments && !deps.super_) {
    forms.push_back("(function () { return " + e + "; })()");
    forms.push_back("(function* () { yield " + e + "; })().next().value");
  }
  if (m.budget->exhausted) return std::nullopt;
  const std::string& form = forms[m.random->Below(static_cast<uint32_t>(forms.size()))];
  return Replace(m.source, ts_node_start_byte(*node), ts_node_end_byte(*node),
                 "(" + form + ")", m.limit);
}

struct StatementSite {
  TSNode list;
  std::vector<TSNode> statements;  // excluding the directive prologue
};

std::vector<StatementSite> StatementSites(TSNode root, Budget* budget) {
  std::vector<StatementSite> sites;
  Walk(root, budget, [&](TSNode node, TSNode) {
    if (!Is(node, "program") && !Is(node, "statement_block")) return true;
    if (ts_node_has_error(node)) return true;
    auto statements = ListStatements(node);
    const size_t prologue = PrologueLength(statements);
    statements.erase(statements.begin(), statements.begin() + prologue);
    if (!statements.empty()) sites.push_back({node, std::move(statements)});
    return sites.size() < 1024;
  });
  return sites;
}

std::optional<std::string> EditWrapStatement(Mutation& m, bool unsafe = false) {
  auto sites = StatementSites(m.root, m.budget);
  const StatementSite* site = PickFrom(sites, m.random);
  if (!site) return std::nullopt;
  const TSNode* statement = PickFrom(site->statements, m.random);
  const GenContext ctx = ContextFor(site->list, m.source);
  const Dependencies deps = FindDependencies(*statement, m.source, m.budget);
  const std::string s(NodeText(*statement, m.source));
  SeedInfo seed;
  BuildSeedInfo(m.root, m.source, site->list, ctx, m.budget, &seed);
  Generator generator(m.random, 768, seed);
  std::string condition;
  if (!generator.Expression(&condition, ctx, 2)) condition = "1";
  std::string other;
  if (!generator.Statement(&other, ctx, 2)) other = ";";
  const std::string label = UniqueName("L", seed, {s, other, condition}, m.random->Next());
  const std::string index = UniqueName("i", seed, {s, other, condition}, m.random->Next());
  const std::string error = UniqueName("e", seed, {s, other, condition}, m.random->Next());
  const std::string body = "{\n" + s + "\n}";

  struct Form {
    std::string text;
    bool safe;
  };
  const bool no_flow = !deps.jump && !deps.return_;
  std::vector<Form> forms = {
      {"if (" + condition + ") " + body, true},
      {"if (" + condition + ") " + body + " else {\n" + other + "\n}", true},
      {"if (" + condition + ") {\n" + other + "\n} else " + body, true},
      {"try " + body + " catch (" + error + ") {\n" + other + "\n}", true},
      {"try " + body + " finally {\n" + other + "\n}", true},
      {body, true},
      {label + ": " + body, true},
      {"for (let " + index + " = 0; " + index + " < 1; " + index + "++) " + body, true},
      {"do " + body + " while (" + condition + ");", true},
      {"while (" + condition + ") {\n" + s + "\nbreak;\n}", true},
      {"switch (" + condition + ") {\ncase " + condition + ":\n" + s + "\n}", true},
      {"(() => " + body + ")();", !deps.yield_ && !deps.await_ && !deps.jump},
      {"(async () => " + body + ")();",
       !deps.yield_ && !deps.jump && (ctx.async || !deps.await_)},
      {"(function () " + body + ")();",
       !deps.yield_ && !deps.await_ && !deps.jump && !deps.super_},
      {"(function* () " + body + ")().next();",
       !deps.await_ && !deps.jump && !deps.super_ && (ctx.generator || !deps.yield_)},
      {"(class { static " + body + " });",
       !deps.yield_ && !deps.await_ && no_flow && !deps.arguments && !deps.super_call &&
           (ctx.strict || !HasSloppyOnly(*statement, m.source, m.budget))},
  };
  if (!ctx.strict) forms.push_back({"with (" + condition + ") " + body, true});
  if (!ctx.strict) {
    forms.push_back({"(function () {\n\"use strict\";\n" + s + "\n})();",
                     !deps.yield_ && !deps.await_ && !deps.jump && !deps.super_ &&
                         !HasSloppyOnly(*statement, m.source, m.budget)});
  }
  if (m.budget->exhausted) return std::nullopt;
  std::vector<const Form*> eligible;
  for (const Form& form : forms) {
    if (form.safe == unsafe) continue;
    eligible.push_back(&form);
  }
  if (eligible.empty()) return std::nullopt;
  const Form* form = eligible[m.random->Below(static_cast<uint32_t>(eligible.size()))];
  return Replace(m.source, ts_node_start_byte(*statement),
                 ts_node_end_byte(*statement), form->text, m.limit);
}

std::optional<std::string> EditStatements(Mutation& m, EditKind kind) {
  auto sites = StatementSites(m.root, m.budget);
  const StatementSite* site = PickFrom(sites, m.random);
  if (!site) return std::nullopt;
  const TSNode* statement = PickFrom(site->statements, m.random);
  const size_t start = ts_node_start_byte(*statement);
  const size_t end = ts_node_end_byte(*statement);
  if (kind == EditKind::kDeleteStatement) {
    // Deleting the only statement of a list would leave a dead end.
    if (site->statements.size() < 2) return std::nullopt;
    return Replace(m.source, start, end, ";", m.limit);
  }
  if (kind == EditKind::kDuplicateStatement) {
    if (IsDeclarationType(ts_node_type(*statement))) return std::nullopt;
    const std::string text(NodeText(*statement, m.source));
    // Keep the copy a separate statement even when the original relies on
    // automatic semicolon insertion.
    const bool terminated = end > 0 && (m.source[end - 1] == ';' || m.source[end - 1] == '}');
    return Replace(m.source, end, end, (terminated ? "\n" : ";\n") + text, m.limit);
  }
  // Swap with another statement in the same list.
  if (site->statements.size() < 2) return std::nullopt;
  const TSNode* other = PickFrom(site->statements, m.random);
  if (ts_node_eq(*other, *statement)) return std::nullopt;
  TSNode first = ts_node_start_byte(*statement) < ts_node_start_byte(*other) ? *statement : *other;
  TSNode second = ts_node_eq(first, *statement) ? *other : *statement;
  const std::string a(NodeText(first, m.source));
  const std::string b(NodeText(second, m.source));
  std::string out(m.source.substr(0, ts_node_start_byte(first)));
  out += b;
  out.append(m.source.substr(ts_node_end_byte(first),
                             ts_node_start_byte(second) - ts_node_end_byte(first)));
  out += a;
  out.append(m.source.substr(ts_node_end_byte(second)));
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

std::optional<std::string> EditReuseSubtree(Mutation& m) {
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  const GenContext ctx = ContextFor(point->list, m.source);
  SeedInfo seed;
  TSNode source_root = ts_node_is_null(m.donor_root) ? m.root : m.donor_root;
  std::string_view source_text = ts_node_is_null(m.donor_root) ? m.source : m.donor_source;
  BuildSeedInfo(source_root, source_text, source_root, ctx, m.budget, &seed, false);
  const std::string* block = PickFrom(seed.blocks, m.random);
  if (!block) return std::nullopt;
  const unsigned form = m.random->Below(3);
  std::string statement = form == 0 ? "(" + *block + ");"
                          : form == 1 ? "(" + *block + ")();"
                                      : "new (" + *block + ")();";
  std::string out = InsertStatement(m.source, *point, statement);
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

std::optional<std::string> EditRepair(Mutation& m) {
  std::vector<TSNode> problems;
  Walk(m.root, m.budget, [&](TSNode node, TSNode) {
    if (ts_node_is_error(node) || ts_node_is_missing(node)) problems.push_back(node);
    return problems.size() < 256;
  });
  const TSNode* node = PickFrom(problems, m.random);
  if (!node) return std::nullopt;
  const size_t start = ts_node_start_byte(*node);
  if (ts_node_is_missing(*node)) {
    // A missing token's type is its spelling, e.g. ")" or ";".
    const std::string token = ts_node_type(*node);
    if (token.empty() || token.size() > 4 || std::isalpha(static_cast<unsigned char>(token[0]))) {
      return std::nullopt;
    }
    return Replace(m.source, start, start, token, m.limit);
  }
  return Replace(m.source, start, ts_node_end_byte(*node), " ", m.limit);
}

// Code from a second input, transplanted into the parent.
std::optional<std::string> EditCrossOver(Mutation& m) {
  if (ts_node_is_null(m.donor_root)) return std::nullopt;
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  const GenContext ctx = ContextFor(point->list, m.source);
  std::vector<std::string> names;
  CollectAllNames(m.root, m.source, &names, m.budget);
  std::vector<TSNode> donors;
  for (TSNode statement : ListStatements(m.donor_root)) {
    if (IsDirective(statement) || ts_node_has_error(statement)) continue;
    donors.push_back(statement);
  }
  const TSNode* donor = PickFrom(donors, m.random);
  if (!donor) return std::nullopt;
  const Dependencies deps = FindDependencies(*donor, m.donor_source, m.budget);
  if (ctx.class_field && deps.arguments) return std::nullopt;
  if (ctx.strict && HasSloppyOnly(*donor, m.donor_source, m.budget)) return std::nullopt;
  // A transplanted declaration must not collide with any parent name.
  std::vector<std::string> declared;
  DeclaredNames(*donor, m.donor_source, &declared, m.budget);
  for (const std::string& name : declared) {
    if (std::find(names.begin(), names.end(), name) != names.end()) return std::nullopt;
  }
  if (IsDeclarationType(ts_node_type(*donor)) && declared.empty()) return std::nullopt;
  std::string out = InsertStatement(m.source, *point,
                                    std::string(NodeText(*donor, m.donor_source)));
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

// Rebind a visible name: shadow it in a block, redeclare it as a var,
// reuse it for parameters, a catch parameter, or an Annex B function. The
// early-error check that every candidate passes rejects the illegal ones.
// Both an eagerly called and a lazily compiled function form are used.
std::optional<std::string> EditRebind(Mutation& m) {
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  const GenContext ctx = ContextFor(point->list, m.source);
  SeedInfo seed;
  BuildSeedInfo(m.root, m.source, point->list, ctx, m.budget, &seed);
  if (m.budget->exhausted) return std::nullopt;
  const std::string* visible = PickFrom(seed.names, m.random);
  const std::string n = visible ? *visible : UniqueName("n", seed, {}, m.random->Next());
  Generator generator(m.random, 512, seed);
  std::string e;
  if (!generator.Expression(&e, ctx, 2)) e = "0";
  // The body of a new plain function nested at this point.
  GenContext fn = ctx;
  fn.function = true;
  fn.async = false;
  fn.generator = false;
  fn.loop = false;
  fn.switch_stmt = false;
  fn.labels.clear();
  fn.loop_labels.clear();
  fn.super_property = false;
  fn.super_call = false;
  fn.new_target = true;
  fn.class_field = false;
  std::string fe;
  if (!generator.Expression(&fe, fn, 2)) fe = "0";
  const uint32_t salt = m.random->Next();
  const std::string p = UniqueName("p", seed, {e, fe, n}, salt);
  const std::string c = UniqueName("c", seed, {e, fe, n}, salt + 1);
  const std::string f = UniqueName("F", seed, {e, fe, n}, salt + 2);
  const std::string label = UniqueName("L", seed, {e, fe, n}, salt + 3);
  const std::string body = "{\nvar " + n + " = " + fe + ";\n{ function " + p +
                           "() {} }\ntry {} catch (" + c + ") { var " + c + "; }\nreturn " +
                           n + ";\n}";
  std::vector<std::string> forms = {
      "{\nlet " + n + " = " + e + ";\n" + n + ";\n}",
      "{\nconst " + n + " = " + e + ";\n(() => " + n + ")();\n}",
      "{\nclass " + n + " {}\n}",
      "{\nfunction " + n + "() { return " + n + "; }\n}",
      "var " + n + " = " + e + ";",
      "for (var " + n + " in " + e + ");",
      "try {} catch (" + n + ") {\nvar " + n + " = " + e + ";\n}",
      "try {} catch (" + n + ") {\n{ let " + n + "; }\n}",
      // Eager: called at once, so the full parser handles the scopes.
      "(function (" + n + ", " + p + " = () => " + n + ") " + body + ")(" + e + ");",
      // Lazy: only preparsed until called.
      "function " + f + "(" + n + ", " + p + " = () => " + n + ") " + body,
      "(" + n + " => {\nvar " + c + " = " + n + ";\n{ let " + n + " = " + c + "; }\n})(" + e + ");",
  };
  if (!ctx.strict) {
    forms.insert(forms.end(), {
        "{\nfunction " + n + "() {}\n}",
        "if (" + e + ") function " + n + "() {}",
        label + ": function " + n + "() {}",
        "switch (" + e + ") {\ncase 0:\nfunction " + n + "() {}\n}",
        "(function (" + n + ", " + p + ", " + n + ") {\nvar " + p + ";\nreturn " + n + ";\n})(" +
            e + ");",
        "function " + f + "(" + n + ", " + n + ") { return " + n + "; }",
        "(function (eval, arguments) {\nreturn eval(" + n + ");\n})(" + e + ");",
    });
  }
  const std::string* form = PickFrom(forms, m.random);
  std::string out = InsertStatement(m.source, *point, *form);
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

// ---------------------------------------------------------------------------
// Malformed mode.

std::vector<TSNode> Leaves(TSNode root, Budget* budget) {
  std::vector<TSNode> leaves;
  Walk(root, budget, [&](TSNode node, TSNode) {
    if (ts_node_child_count(node) == 0 && ts_node_end_byte(node) > ts_node_start_byte(node)) {
      leaves.push_back(node);
    }
    return leaves.size() < 4096;
  });
  return leaves;
}

std::optional<std::string> EditDisrupt(Mutation& m) {
  auto leaves = Leaves(m.root, m.budget);
  if (leaves.empty()) {
    return Replace(m.source, m.source.size(), m.source.size(), "(", m.limit);
  }
  const size_t index = m.random->Below(static_cast<uint32_t>(leaves.size()));
  TSNode leaf = leaves[index];
  const size_t start = ts_node_start_byte(leaf);
  const size_t end = ts_node_end_byte(leaf);
  const std::string text(NodeText(leaf, m.source));
  auto token = [&]() {
    return m.random->Chance(50) ? std::string(m.random->Pick(internal::kKeywords))
                                : std::string(m.random->Pick(internal::kPunctuators));
  };
  static constexpr std::string_view kOpeners[] = {"\"", "'", "`", "/*", "/", "${",
                                                  "(", "[", "{", "\\u{", "#"};
  switch (m.random->Below(8)) {
    case 0:
      return Replace(m.source, start, end, "", m.limit);
    case 1:
      return Replace(m.source, end, end, " " + text, m.limit);
    case 2:
      return Replace(m.source, start, end, token(), m.limit);
    case 3:
      return Replace(m.source, start, start, token() + " ", m.limit);
    case 4: {
      if (index + 1 >= leaves.size()) return std::nullopt;
      TSNode next = leaves[index + 1];
      const std::string next_text(NodeText(next, m.source));
      std::string out(m.source.substr(0, start));
      out += next_text;
      out.append(m.source.substr(end, ts_node_start_byte(next) - end));
      out += text;
      out.append(m.source.substr(ts_node_end_byte(next)));
      if (out.size() > m.limit) return std::nullopt;
      return out;
    }
    case 5:
      return std::string(m.source.substr(0, start));
    case 6:
      return Replace(m.source, start, start, std::string(m.random->Pick(kOpeners)), m.limit);
    default: {
      // Remove one closing bracket.
      for (size_t i = index; i < leaves.size(); ++i) {
        const std::string_view t = NodeText(leaves[i], m.source);
        if (t == "}" || t == ")" || t == "]") {
          return Replace(m.source, ts_node_start_byte(leaves[i]),
                         ts_node_end_byte(leaves[i]), "", m.limit);
        }
      }
      return Replace(m.source, m.source.size(), m.source.size(), "(", m.limit);
    }
  }
}

// Parse-time errors built from the seed's own code and context.
std::optional<std::string> EditEarlyError(Mutation& m) {
  const unsigned pick = m.random->Below(100);
  if (pick < 25) {
    // Move code across a boundary it depends on.
    if (auto out = EditWrapStatement(m, true)) return out;
  }
  if (pick < 45) {
    // Redeclare a seed's lexical binding in the same scope.
    auto sites = StatementSites(m.root, m.budget);
    std::vector<TSNode> lexical;
    for (const auto& site : sites) {
      for (TSNode s : site.statements) {
        if (Is(s, "lexical_declaration") || Is(s, "class_declaration")) lexical.push_back(s);
      }
    }
    if (const TSNode* s = PickFrom(lexical, m.random)) {
      std::vector<std::string> names;
      DeclaredNames(*s, m.source, &names, m.budget);
      if (!names.empty()) {
        const size_t end = ts_node_end_byte(*s);
        const std::string decl = m.random->Chance(50) ? "\nvar " + names[0] + ";"
                                                      : "\nlet " + names[0] + ";";
        return Replace(m.source, end, end, decl, m.limit);
      }
    }
  }
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  const GenContext ctx = ContextFor(point->list, m.source);
  std::vector<std::string> options;
  if (!ctx.loop && !ctx.switch_stmt) options.push_back("break;");
  if (!ctx.loop) options.push_back("continue;");
  options.push_back("break nowhere;");
  if (!ctx.function) options.push_back("return;");
  if (!ctx.new_target) options.push_back("new.target;");
  if (!ctx.super_property) options.push_back("super.x;");
  if (!ctx.super_call) options.push_back("super();");
  options.push_back("this.#undeclared;");
  if (!ctx.async) options.push_back("for await (const x of y) {}");
  options.insert(options.end(), {
      "let let = 1;", "const c;", "1 = 2;", "a?.b = 1;", "++1;", "a ?? b || c;",
      "-a ** b;", "new import(x);", "import.meta;", "export default 1;",
      "({__proto__: 1, __proto__: 2});", "(a, a) => 1;",
      "function f(a = 1) { \"use strict\"; }", "(function () { \"use strict\"; 010; });",
      "(function () { \"use strict\"; with (a) {} });", "/a/gg;", "/(/;", "/a/uv;",
      "/(?<n>a)(?<n>b)/;", "class K { constructor() {} constructor() {} }",
      "class K { m() { super(); } }", "(class { x = arguments; });",
      "(class { static { await; } });", "(class { static { return; } });",
      "L: L: ;", "async function f() { var await; }", "function* g() { var yield; }",
      "for (let x of y, z) {}", "for (async of x) {}", "let [a];", "({a = 1});",
      "`${`", "0b2;", "1__0;", "1_;", "08n;", "\"\\u{110000}\";", "x\n=> 1;",
      "(class { #a; #a; });", "(class { constructor; });", "class C extends D { static prototype() {} }",
      "({ get x(a) {} });", "({ set x() {} });", "async () => await;",
      "function f() { 'use strict'; delete x; }",
      "(function () { \"use strict\"; implements = 1; });",
      // Names: hoisting conflicts, parameters, escaped keywords.
      "let x; { var x; }", "{ function f() {} var f; }",
      "try {} catch ([e]) { var e; }", "function f(a, a) { \"use strict\"; }",
      "function f(a, [a]) {}", "({ m(a, a) {} });", "(class { m(a, a) {} });",
      "(function () { \"use strict\"; L: function f() {} });",
      "if (1) function* g() {}", "function* g() { yi\\u0065ld 1; }",
      "async function f() { aw\\u0061it 1; }", "l\\u0065t x = 1;",
      "class C { st\\u0061tic m() {} }", "function f() { n\\u0065w.target; }",
      "class C { *constructor() {} }", "class C { #constructor; }",
      "class C { static prototype = 1; }", "(function () { \"use strict\"; var let; });",
      "async function f() { var await; }", "(function eval() { \"use strict\"; });",
      // Assignment targets.
      "[f()] = x;", "({a}) = 1;", "(f``) = 1;", "(f()) ?\?= 1;", "(new f()) = 1;",
      "(function () { \"use strict\"; f() = 1; });", "(a?.b) = 1;"});
  const std::string* option = PickFrom(options, m.random);
  std::string out = InsertStatement(m.source, *point, *option);
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

// ---------------------------------------------------------------------------

struct Weighted {
  EditKind kind;
  unsigned weight;
};

std::vector<EditKind> Order(std::vector<Weighted> rules, Random* random) {
  std::vector<EditKind> order;
  while (!rules.empty()) {
    unsigned total = 0;
    for (const auto& r : rules) total += r.weight;
    unsigned roll = random->Below(total);
    size_t i = 0;
    while (roll >= rules[i].weight) roll -= rules[i++].weight;
    order.push_back(rules[i].kind);
    rules.erase(rules.begin() + static_cast<long>(i));
  }
  return order;
}

std::optional<std::string> Apply(Mutation& m, EditKind kind) {
  switch (kind) {
    case EditKind::kLiteral:
      return EditLiteral(m);
    case EditKind::kBinaryOperator:
      return EditOperator(m);
    case EditKind::kWrapExpression:
      return EditWrapExpression(m);
    case EditKind::kGenerateStatement:
      return EditGenerateStatement(m);
    case EditKind::kGenerateExpression:
      return EditGenerateExpression(m);
    case EditKind::kWrapStatement:
      return EditWrapStatement(m);
    case EditKind::kDeleteStatement:
    case EditKind::kDuplicateStatement:
    case EditKind::kSwapStatements:
      return EditStatements(m, kind);
    case EditKind::kReuseSubtree:
      return EditReuseSubtree(m);
    case EditKind::kRepairSyntax:
      return EditRepair(m);
    case EditKind::kCrossOver:
      return EditCrossOver(m);
    case EditKind::kDisruptSyntax:
      return EditDisrupt(m);
    case EditKind::kInjectEarlyError:
      return EditEarlyError(m);
    case EditKind::kRebind:
      return EditRebind(m);
    default:
      return std::nullopt;
  }
}

bool IsBlank(std::string_view source) {
  return source.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos;
}

std::optional<Result> GenerateProgram(TSParser* parser, uint32_t seed,
                                      size_t max_size, Budget* budget) {
  const size_t limit = std::min(max_size, kMaxSourceBytes);
  const SeedInfo empty;
  // Budgets shrink across attempts; each attempt depends only on the seed.
  for (unsigned attempt = 0; attempt < 7; ++attempt) {
    Random random(seed ^ (0x9e3779b9u * (attempt + 1)));
    Generator generator(&random, kGenerationBytes >> attempt, empty);
    std::string script;
    if (!generator.Program(&script, attempt < 2 ? 6 : 5 - std::min(attempt, 4u), 5)) continue;
    if (script.size() > limit) continue;
    Budget local;
    Tree tree = Parse(parser, script, &local);
    if (!tree || local.exhausted) continue;
    TSNode root = ts_tree_root_node(tree.get());
    auto errors = CountSyntaxErrors(root, &local);
    if (errors && *errors == 0 && !HasEarlyError(root, script, &local)) {
      return Result{std::move(script), Outcome::kGeneratedValidLeaning, EditKind::kNone};
    }
  }
  (void)budget;
  for (std::string_view compact : {"if (1) {}", "0;", ";"}) {
    if (compact.size() <= limit) {
      return Result{std::string(compact), Outcome::kGeneratedValidLeaning, EditKind::kNone};
    }
  }
  return std::nullopt;
}

Result MutateImpl(const uint8_t* data, size_t size, const uint8_t* donor,
                  size_t donor_size, size_t max_size, uint32_t seed,
                  Config config) {
  if (max_size == 0) return Fallback(max_size);
  if (size > kMaxSourceBytes || (size != 0 && data == nullptr)) return Fallback(max_size);
  if (donor_size > kMaxSourceBytes || (donor_size != 0 && donor == nullptr)) {
    donor = nullptr;
    donor_size = 0;
  }
  Random random(seed);
  const bool valid = random.Next() % 100 < std::min(config.valid_percent, 100u);

  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_javascript())) {
    return Fallback(max_size);
  }
  Budget budget;
  const std::string_view source =
      size == 0 ? std::string_view{}
                : std::string_view(reinterpret_cast<const char*>(data), size);
  if (IsBlank(source)) {
    if (!valid) return Fallback(max_size);
    if (auto generated = GenerateProgram(parser.get(), seed, max_size, &budget)) {
      return *generated;
    }
    return Fallback(max_size);
  }

  Tree parent = Parse(parser.get(), source, &budget);
  if (!parent || budget.exhausted) return Fallback(max_size);
  const TSNode root = ts_tree_root_node(parent.get());
  const auto parent_errors = CountSyntaxErrors(root, &budget);
  if (!parent_errors) return Fallback(max_size);
  const bool parent_early = HasEarlyError(root, source, &budget);

  // The donor tree lives for the whole call.
  std::unique_ptr<TSParser, decltype(&ts_parser_delete)> donor_parser(
      ts_parser_new(), ts_parser_delete);
  Tree donor_tree(nullptr, ts_tree_delete);
  std::string_view donor_source;
  if (donor_size != 0 && donor_parser &&
      ts_parser_set_language(donor_parser.get(), tree_sitter_javascript())) {
    donor_source = std::string_view(reinterpret_cast<const char*>(donor), donor_size);
    donor_tree = Parse(donor_parser.get(), donor_source, &budget);
  }

  Mutation m{source, root, &budget, &random, std::min(max_size, kMaxSourceBytes)};
  if (donor_tree) {
    m.donor_root = ts_tree_root_node(donor_tree.get());
    m.donor_source = donor_source;
  }

  std::vector<Weighted> rules;
  if (valid) {
    rules = {{EditKind::kGenerateStatement, 22}, {EditKind::kGenerateExpression, 16},
             {EditKind::kWrapStatement, 12},     {EditKind::kWrapExpression, 10},
             {EditKind::kLiteral, 8},            {EditKind::kBinaryOperator, 8},
             {EditKind::kReuseSubtree, 6},       {EditKind::kDuplicateStatement, 5},
             {EditKind::kSwapStatements, 5},     {EditKind::kDeleteStatement, 4},
             {EditKind::kRebind, 7}};
    if (*parent_errors != 0) rules.push_back({EditKind::kRepairSyntax, 60});
    if (donor_tree) rules.push_back({EditKind::kCrossOver, 70});
  } else {
    rules = {{EditKind::kDisruptSyntax, 60}, {EditKind::kInjectEarlyError, 40}};
  }
  const auto order = Order(rules, &random);
  // Occasionally start over from the grammar, keeping fresh programs in the mix.
  if (valid && random.Chance(3)) {
    if (auto generated = GenerateProgram(parser.get(), random.Next(), max_size, &budget)) {
      generated->edit = EditKind::kGenerateProgram;
      return *generated;
    }
  }

  for (size_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const EditKind kind = order[attempt % order.size()];
    auto candidate = Apply(m, kind);
    if (budget.exhausted) break;
    if (!candidate || *candidate == source || candidate->size() > m.limit) continue;
    Budget check;
    Tree tree = Parse(parser.get(), *candidate, &check);
    if (!tree || check.exhausted) continue;
    const TSNode candidate_root = ts_tree_root_node(tree.get());
    const auto errors = CountSyntaxErrors(candidate_root, &check);
    if (!errors) continue;
    const bool early = HasEarlyError(candidate_root, *candidate, &check);
    if (valid) {
      // Never make the parent worse; an invalid parent may only improve.
      if (*errors > *parent_errors || (early && !parent_early)) continue;
      const bool clean = *errors == 0 && !early;
      return {std::move(*candidate),
              clean ? Outcome::kEditedValidLeaning : Outcome::kEditedMalformed, kind};
    }
    const bool worse = *errors > *parent_errors || (early && !parent_early) ||
                       kind == EditKind::kInjectEarlyError;
    if (!worse) continue;
    return {std::move(*candidate), Outcome::kEditedMalformed, kind};
  }
  if (valid) {
    // Never return a dead end: regenerate rather than emit `;`.
    if (auto generated = GenerateProgram(parser.get(), random.Next(), max_size, &budget)) {
      generated->edit = EditKind::kGenerateProgram;
      return *generated;
    }
  }
  return Fallback(max_size);
}

}  // namespace

Result Mutate(const uint8_t* data, size_t size, size_t max_size, uint32_t seed,
              Config config) {
  try {
    return MutateImpl(data, size, nullptr, 0, max_size, seed, config);
  } catch (const std::bad_alloc&) {
    return Fallback(max_size);
  }
}

Result CrossOver(const uint8_t* data, size_t size, const uint8_t* donor,
                 size_t donor_size, size_t max_size, uint32_t seed,
                 Config config) {
  try {
    return MutateImpl(data, size, donor, donor_size, max_size, seed, config);
  } catch (const std::bad_alloc&) {
    return Fallback(max_size);
  }
}

}  // namespace js_mutator
