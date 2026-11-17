#include "js_mutator/mutator.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-javascript.h>

namespace js_mutator {
namespace {

constexpr size_t kMaxSourceBytes = 64 * 1024;
constexpr unsigned kMaxAttempts = 4;
constexpr unsigned kMaxProgressCalls = 1024;
constexpr unsigned kMaxNodeVisits = 65536;

struct Budget {
  unsigned progress_calls = 0;
  unsigned node_visits = 0;
  bool exhausted = false;
};

struct Input {
  std::string_view source;
};

struct Edit {
  size_t start;
  size_t end;
  std::string_view replacement;
  EditKind kind;
};

class Random {
 public:
  explicit Random(uint32_t seed) : state_(seed == 0 ? 0x9e3779b9u : seed) {}

  uint32_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }

 private:
  uint32_t state_;
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
    if (!visit(node, parent)) {
      return false;
    }
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

bool HasSyntaxError(TSNode root, Budget* budget) {
  bool error = false;
  Walk(root, budget, [&](TSNode node, TSNode) {
    error = ts_node_is_error(node) || ts_node_is_missing(node);
    return !error;
  });
  return error || budget->exhausted;
}

std::optional<unsigned> CountSyntaxErrors(TSNode root, Budget* budget) {
  unsigned errors = 0;
  if (!Walk(root, budget, [&](TSNode node, TSNode) {
        if (ts_node_is_error(node) || ts_node_is_missing(node)) ++errors;
        return true;
      })) {
    return std::nullopt;
  }
  return errors;
}

std::optional<Edit> EditForNode(TSNode node, TSNode parent,
                                std::string_view source, EditKind kind) {
  const size_t start = ts_node_start_byte(node);
  const size_t end = ts_node_end_byte(node);
  if (start > end || end > source.size()) return std::nullopt;

  const char* type = ts_node_type(node);
  if (kind == EditKind::kLiteral) {
    if (std::strcmp(type, "number") == 0) {
      return Edit{start, end,
                  source.substr(start, end - start) == "0" ? "1" : "0", kind};
    }
    if (std::strcmp(type, "string") == 0) {
      return Edit{start, end,
                  source.substr(start, end - start) == "\"\"" ? "\"x\"" : "\"\"",
                  kind};
    }
    if (std::strcmp(type, "true") == 0) return Edit{start, end, "false", kind};
    if (std::strcmp(type, "false") == 0) return Edit{start, end, "true", kind};
  }

  if (kind == EditKind::kBinaryOperator) {
    if (ts_node_is_null(parent) ||
        std::strcmp(ts_node_type(parent), "binary_expression") != 0) {
      return std::nullopt;
    }
    const std::string_view old = source.substr(start, end - start);
    if (old == "+") return Edit{start, end, "-", kind};
    if (old == "-") return Edit{start, end, "+", kind};
    if (old == "*") return Edit{start, end, "/", kind};
    if (old == "/") return Edit{start, end, "*", kind};
    if (old == "==") return Edit{start, end, "!=", kind};
    if (old == "!=") return Edit{start, end, "==", kind};
    if (old == "===") return Edit{start, end, "!==", kind};
    if (old == "!==") return Edit{start, end, "===", kind};
  }

  if (kind == EditKind::kWrapExpression &&
      std::strcmp(type, "binary_expression") == 0) {
    return Edit{start, end, {}, kind};
  }

  if (kind == EditKind::kInsertStatement) {
    if (std::strcmp(type, "statement_block") == 0 && end > start &&
        source[end - 1] == '}') {
      return Edit{end - 1, end - 1, "\n0;", kind};
    }
    if (std::strcmp(type, "program") == 0) {
      return Edit{source.size(), source.size(), "\n0;", kind};
    }
  }

  if (kind == EditKind::kDisruptSyntax) {
    if ((std::strcmp(type, "statement_block") == 0 ||
         std::strcmp(type, "arguments") == 0) &&
        end > start && (source[end - 1] == '}' || source[end - 1] == ')')) {
      return Edit{end - 1, end, "", kind};
    }
    if (std::strcmp(type, "program") == 0) {
      return Edit{source.size(), source.size(), "(", kind};
    }
  }
  return std::nullopt;
}

std::optional<Edit> SelectEdit(TSNode root, std::string_view source,
                               EditKind kind, Budget* budget, Random* random) {
  std::optional<Edit> selected;
  uint32_t eligible = 0;
  Walk(root, budget, [&](TSNode node, TSNode parent) {
    auto edit = EditForNode(node, parent, source, kind);
    if (edit && random->Next() % ++eligible == 0) selected = std::move(edit);
    return true;
  });
  if (budget->exhausted) return std::nullopt;
  return selected;
}

std::optional<std::string> ApplyEdit(std::string_view source, const Edit& edit,
                                     size_t max_size) {
  if (edit.start > edit.end || edit.end > source.size()) return std::nullopt;
  const size_t unchanged = source.size() - (edit.end - edit.start);
  const size_t limit = std::min(max_size, kMaxSourceBytes);
  const size_t replacement_size = edit.kind == EditKind::kWrapExpression
      ? edit.end - edit.start + 2 : edit.replacement.size();
  if (unchanged > limit || replacement_size > limit - unchanged) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(unchanged + replacement_size);
  result.append(source.data(), edit.start);
  if (edit.kind == EditKind::kWrapExpression) {
    result.push_back('(');
    result.append(source.data() + edit.start, edit.end - edit.start);
    result.push_back(')');
  } else {
    result.append(edit.replacement);
  }
  result.append(source.data() + edit.end, source.size() - edit.end);
  return result;
}

}  // namespace

static Result MutateImpl(const uint8_t* data, size_t size, size_t max_size,
                         uint32_t seed, Config config) {
  if (max_size == 0) return Fallback(max_size);
  if (size > kMaxSourceBytes || (size != 0 && data == nullptr)) {
    return Fallback(max_size);
  }
  if (size == 0) return Fallback(max_size);

  Parser parser(ts_parser_new(), ts_parser_delete);
  if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_javascript())) {
    return Fallback(max_size);
  }

  Budget budget;
  const std::string_view source(reinterpret_cast<const char*>(data), size);
  Tree parent = Parse(parser.get(), source, &budget);
  if (!parent || budget.exhausted) return Fallback(max_size);

  Random random(seed);
  const bool valid_leaning = random.Next() % 100 < std::min(config.valid_percent, 100u);
  std::optional<unsigned> parent_errors;
  if (!valid_leaning) {
    parent_errors = CountSyntaxErrors(ts_tree_root_node(parent.get()), &budget);
    if (!parent_errors) return Fallback(max_size);
  }
  const unsigned first = random.Next() % kMaxAttempts;
  for (unsigned attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const EditKind kind = valid_leaning
        ? static_cast<EditKind>(1 + (first + attempt) % kMaxAttempts)
        : EditKind::kDisruptSyntax;
    auto edit = SelectEdit(ts_tree_root_node(parent.get()), source, kind,
                           &budget, &random);
    if (budget.exhausted) return Fallback(max_size);
    if (!edit) continue;
    auto candidate = ApplyEdit(source, *edit, max_size);
    if (!candidate) continue;
    Tree tree = Parse(parser.get(), *candidate, &budget);
    if (!tree || budget.exhausted) return Fallback(max_size);
    if (valid_leaning) {
      const bool has_error = HasSyntaxError(ts_tree_root_node(tree.get()), &budget);
      if (budget.exhausted) return Fallback(max_size);
      if (has_error) continue;
    } else {
      auto candidate_errors = CountSyntaxErrors(ts_tree_root_node(tree.get()),
                                                &budget);
      if (!candidate_errors) return Fallback(max_size);
      if (*candidate_errors <= *parent_errors) continue;
    }
    return {std::move(*candidate),
            valid_leaning ? Outcome::kEditedValidLeaning : Outcome::kEditedMalformed,
            kind};
  }
  return Fallback(max_size);
}

Result Mutate(const uint8_t* data, size_t size, size_t max_size,
              uint32_t seed, Config config) {
  try {
    return MutateImpl(data, size, max_size, seed, config);
  } catch (const std::bad_alloc&) {
    return Fallback(max_size);
  }
}

}  // namespace js_mutator
