#pragma once

#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <string>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-javascript.h>

#include "js_mutator/mutator.h"

namespace test {

inline void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

class Syntax {
 public:
  Syntax() : parser_(ts_parser_new()) {
    Require(parser_ && ts_parser_set_language(parser_, tree_sitter_javascript()),
            "cannot initialize Tree-sitter");
  }
  ~Syntax() { ts_parser_delete(parser_); }

  // Visits every node; returns false when the source has syntax errors.
  bool Visit(const std::string& source,
             const std::function<void(TSNode, std::string_view)>& visit) {
    TSTree* tree = ts_parser_parse_string(parser_, nullptr, source.data(),
                                          static_cast<uint32_t>(source.size()));
    if (!tree) return false;
    TSNode root = ts_tree_root_node(tree);
    const bool ok = !ts_node_has_error(root);
    Walk(root, source, visit);
    ts_tree_delete(tree);
    return ok;
  }

  bool Parses(const std::string& source) {
    return Visit(source, [](TSNode, std::string_view) {});
  }

  std::set<std::string> Types(const std::string& source) {
    std::set<std::string> types;
    Visit(source, [&](TSNode node, std::string_view) {
      if (ts_node_is_named(node)) types.insert(ts_node_type(node));
    });
    return types;
  }

  unsigned Errors(const std::string& source) {
    unsigned errors = 0;
    Visit(source, [&](TSNode node, std::string_view) {
      if (ts_node_is_error(node) || ts_node_is_missing(node)) ++errors;
    });
    return errors;
  }

 private:
  static void Walk(TSNode node, std::string_view source,
                   const std::function<void(TSNode, std::string_view)>& visit) {
    visit(node, source);
    const uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) Walk(ts_node_child(node, i), source, visit);
  }

  TSParser* parser_;
};

inline js_mutator::Result Mutate(const std::string& source, size_t max_size,
                                 uint32_t seed, unsigned valid_percent = 100) {
  return js_mutator::Mutate(reinterpret_cast<const uint8_t*>(source.data()),
                            source.size(), max_size, seed, {valid_percent});
}

inline js_mutator::Result Generate(size_t max_size, uint32_t seed) {
  return js_mutator::Mutate(nullptr, 0, max_size, seed, {100});
}

}  // namespace test
