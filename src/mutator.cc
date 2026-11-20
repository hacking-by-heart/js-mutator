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

using internal::GenContext;
using internal::Generator;
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
    if (!name.empty() && seen.insert(name).second) reserved->push_back(std::move(name));
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

// Code that is only legal in sloppy mode.
bool HasSloppyOnly(TSNode root, std::string_view source, Budget* budget) {
  static constexpr std::string_view kStrictReserved[] = {
      "let", "static", "yield", "implements", "interface", "package",
      "private", "protected", "public"};
  bool found = false;
  Walk(root, budget, [&](TSNode node, TSNode) {
    const char* type = ts_node_type(node);
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
    } else if (std::strcmp(type, "identifier") == 0) {
      const std::string_view text = NodeText(node, source);
      for (std::string_view word : kStrictReserved) {
        if (text == word) found = true;
      }
    } else if (std::strcmp(type, "unary_expression") == 0 &&
               HasDirectToken(node, "delete")) {
      TSNode argument = Field(node, "argument");
      while (Is(argument, "parenthesized_expression")) {
        argument = ts_node_named_child(argument, 0);
      }
      if (Is(argument, "identifier")) found = true;
    }
    return !found;
  });
  return found;
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
  };

  std::string Text(TSNode node) { return std::string(NodeText(node, source_)); }

  void CheckList(const std::vector<TSNode>& statements, bool function_level,
                 bool strict, const std::vector<std::string>& outer_lexical) {
    std::set<std::string> lexical(outer_lexical.begin(), outer_lexical.end());
    std::set<std::string> vars;
    for (TSNode statement : statements) {
      std::vector<std::string> names;
      const char* type = ts_node_type(statement);
      const bool function = std::strcmp(type, "function_declaration") == 0 ||
                            std::strcmp(type, "generator_function_declaration") == 0;
      if (std::strcmp(type, "lexical_declaration") == 0 ||
          std::strcmp(type, "class_declaration") == 0 ||
          (function && (!function_level))) {
        CollectRaw(statement, &names);
        for (const std::string& name : names) {
          // Sloppy Annex B permits duplicate plain function declarations in
          // blocks; everything else is a redeclaration error.
          if (function && !strict && std::strcmp(type, "function_declaration") == 0 &&
              plain_functions_.count(name) != 0 && lexical.count(name) != 0) {
            continue;
          }
          if (!lexical.insert(name).second || vars.count(name) != 0) error_ = true;
          if (function && std::strcmp(type, "function_declaration") == 0) {
            plain_functions_.insert(name);
          }
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
      if (!ts_node_is_null(name)) names->push_back(Text(name));
    }
  }

  void CollectPattern(TSNode node, std::vector<std::string>* names, unsigned depth) {
    if (ts_node_is_null(node) || depth > 64) return;
    const char* type = ts_node_type(node);
    if (std::strcmp(type, "identifier") == 0 ||
        std::strcmp(type, "shorthand_property_identifier_pattern") == 0) {
      names->push_back(Text(node));
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
    State& next = changed ? inner : state;

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count && !error_; ++i) {
      TSNode child = ts_node_named_child(node, i);
      // Declaration names of private members are not references.
      if (Is(child, "private_property_identifier") &&
          (std::strcmp(type, "field_definition") == 0 ||
           std::strcmp(type, "method_definition") == 0)) {
        continue;
      }
      Visit(child, next, depth + 1);
    }
  }

  std::string_view source_;
  Budget* budget_;
  bool error_ = false;
  std::set<std::string> plain_functions_;
  std::set<std::string> outer_lexical_params_;
};

bool HasEarlyError(TSNode root, std::string_view source, Budget* budget) {
  EarlyErrors checker(source, budget);
  return checker.Check(root);
}

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
      {"(async () => " + body + ")();", !deps.yield_ && !deps.jump},
      {"(function () " + body + ")();",
       !deps.yield_ && !deps.await_ && !deps.jump && !deps.super_},
      {"(function* () " + body + ")().next();",
       !deps.await_ && !deps.jump && !deps.super_},
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
      "(function () { \"use strict\"; implements = 1; });"});
  const std::string* option = PickFrom(options, m.random);
  std::string out = InsertStatement(m.source, *point, *option);
  if (out.size() > m.limit) return std::nullopt;
  return out;
}

// One regexp early error: a single-point corruption of a seed regexp literal
// (or of a fresh one when the seed has none), or a flag flip. Each case is a
// SyntaxError on its own, whatever the original pattern was.
std::optional<std::string> EditRegexMalformed(Mutation& m) {
  std::vector<TSNode> regexes;
  Walk(m.root, m.budget, [&](TSNode node, TSNode) {
    if (Is(node, "regex") && !ts_node_has_error(node)) regexes.push_back(node);
    return regexes.size() < 512;
  });
  const TSNode* node = PickFrom(regexes, m.random);
  std::string literal;
  if (node) {
    literal = std::string(NodeText(*node, m.source));
  } else {
    Generator generator(m.random, m.limit, SeedInfo{});
    literal = generator.Literal("regex", false, false);
  }
  const size_t slash = literal.rfind('/');
  if (literal.size() < 3 || literal[0] != '/' || slash == 0) return std::nullopt;
  std::string pattern = literal.substr(1, slash - 1);
  std::string flags = literal.substr(slash + 1);
  std::string plain;  // flags without u and v
  for (char c : flags) {
    if (c != 'u' && c != 'v') plain += c;
  }
  struct Corruption {
    std::string_view text;
    char mode;  // 0: any flags, 'u': needs u or v, 'v': needs v
  };
  static constexpr Corruption kCorruptions[] = {
      {"(", 0},                     // Unterminated group
      {")", 0},                     // Unmatched ')'
      {"|^*", 0},                   // Nothing to repeat
      {"|(?:{2})", 0},              // Nothing to repeat / lone quantifier
      {"a{3,1}", 0},                // numbers out of order in {} quantifier
      {"a**", 0},                   // Nothing to repeat
      {"(?a)", 0},                  // Invalid group
      {"(?<dup>a)(?<dup>b)", 0},    // Duplicate capture group name
      {"(?<q>a)\\k<zq_missing>", 0},  // Invalid named capture referenced
      {"(?<1a>x)", 0},              // Invalid capture group name
      {"(?<\\u0030a>x)", 0},        // Invalid capture group name
      {"[z-a]", 0},                 // Range out of order in character class
      {"(?<=a)*", 0},               // Nothing to repeat (lookbehind)
      {"(?ii:a)", 0},               // Repeated flag in modifiers
      {"(?-:a)", 0},                // Invalid group (no modifier flags)
      {"(?i-i:a)", 0},              // Repeated flag in modifiers
      {"(?i)a", 0},                 // Invalid group
      {"\\a", 'u'},                 // Invalid escape
      {"\\c", 'u'},                 // Invalid unicode escape
      {"{", 'u'},                   // Lone quantifier brackets
      {"]", 'u'},                   // Lone quantifier brackets
      {"\\99", 'u'},                // Invalid decimal escape
      {"\\01", 'u'},                // Invalid decimal escape
      {"\\u{110000}", 'u'},         // Invalid Unicode escape
      {"(?=a)+", 'u'},              // Nothing to repeat (lookahead)
      {"[\\d-z]", 'u'},             // Invalid character class
      {"\\pL", 'u'},                // Invalid property name
      {"\\p{Script=Greekk}", 'u'},  // Invalid property name
      {"\\p{lu}", 'u'},             // Invalid property name
      {"\\p{General_Category}", 'u'},  // Invalid property name
      {"\\p{Block=Basic_Latin}", 'u'},  // Invalid property name
      {"\\p{ASCII=Y}", 'u'},        // Invalid property name
      {"\\p{}", 'u'},               // Invalid property name
      {"[a&&b--c]", 'v'},           // Invalid set operation
      {"[a&&&b]", 'v'},             // Invalid set operation
      {"[!!]", 'v'},                // Invalid character in character class
      {"[(]", 'v'},                 // Invalid character in character class
      {"\\P{RGI_Emoji}", 'v'},      // Negated property of strings
      {"[^\\p{RGI_Emoji}]", 'v'},   // Negated character class with strings
      {"[^\\q{ab}]", 'v'},          // Negated character class with strings
  };
  const unsigned pick = m.random->Below(100);
  if (pick < 15) {
    // Flag flips that are invalid on their own.
    static constexpr char kFlags[] = "dgimsyuv";
    switch (m.random->Below(3)) {
      case 0: {
        const char c = kFlags[m.random->Below(8)];
        flags += c;
        if (std::count(flags.begin(), flags.end(), c) < 2) flags += c;
        break;
      }
      case 1:
        flags = plain + (m.random->Chance(50) ? "uv" : "vu");
        break;
      default: {
        static constexpr char kUnknown[] = {'a', 'z', 'Q', 'e', '_'};
        flags += m.random->Pick(kUnknown);
        break;
      }
    }
  } else {
    const Corruption& c = m.random->Pick(kCorruptions);
    if (c.mode == 'u') {
      flags = plain + (m.random->Chance(70) ? "u" : "v");
    } else if (c.mode == 'v') {
      flags = plain + "v";
    }
    // `\c` before a letter would be a valid control escape.
    if (pick < 60 || c.text == "\\c") {
      pattern += c.text;
    } else {
      pattern = std::string(c.text) + pattern;
    }
  }
  const std::string corrupted = "/" + pattern + "/" + flags;
  if (node) {
    return Replace(m.source, ts_node_start_byte(*node), ts_node_end_byte(*node), corrupted,
                   m.limit);
  }
  auto points = InsertionPoints(m.root, m.source, m.budget);
  const InsertionPoint* point = PickFrom(points, m.random);
  if (!point) return std::nullopt;
  std::string out = InsertStatement(m.source, *point, corrupted + ";");
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
    case EditKind::kRegexMalformed:
      return EditRegexMalformed(m);
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
             {EditKind::kSwapStatements, 5},     {EditKind::kDeleteStatement, 4}};
    if (*parent_errors != 0) rules.push_back({EditKind::kRepairSyntax, 60});
    if (donor_tree) rules.push_back({EditKind::kCrossOver, 70});
  } else {
    rules = {{EditKind::kDisruptSyntax, 60}, {EditKind::kInjectEarlyError, 40},
             {EditKind::kRegexMalformed, 25}};
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
                       kind == EditKind::kInjectEarlyError ||
                       kind == EditKind::kRegexMalformed;
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
