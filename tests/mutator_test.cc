// Interface contract: limits, determinism, fallback, validity, crossover.
#include "test_util.h"

#include <string>
#include <vector>

using js_mutator::EditKind;
using js_mutator::Outcome;
using test::Require;

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
  Require(captures >= 30, "capture edits are rare");
  Require(evals >= 10, "direct eval statements are rare");
  Require(lazy_errors >= 10, "early errors in lazy bodies are rare");
  unsigned generated_eval = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    if (test::Generate(2048, seed).source.find("eval(") != std::string::npos) ++generated_eval;
  }
  Require(generated_eval >= 20, "generation rarely calls eval");
  std::cout << "mutator contract: ok\n";
  return 0;
}
