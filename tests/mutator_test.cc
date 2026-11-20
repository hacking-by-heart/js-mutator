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

  // Malformed mode breaks syntax (or injects an early error). A token-level
  // disruption may also leave only an early error, such as `await f()` in a
  // script; those stay rare.
  unsigned broken_outputs = 0;
  unsigned early_disruptions = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const auto result = test::Mutate(parents[1], 4096, seed, 0);
    Require(result.outcome == Outcome::kEditedMalformed, "malformed mode did not edit");
    if (!syntax.Parses(result.source) || result.edit == EditKind::kInjectEarlyError ||
        result.edit == EditKind::kFlipGoal) {
      ++broken_outputs;
    } else if (result.edit == EditKind::kDisruptSyntax) {
      ++early_disruptions;
    }
  }
  Require(broken_outputs + early_disruptions == 300, "malformed mode produced valid output");
  Require(early_disruptions <= 15, "malformed mode rarely breaks the syntax itself");

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
  for (uint32_t seed = 1; seed <= 600; ++seed) {
    const auto up = test::Mutate(parents[1], 4096, seed);
    if (up.edit == EditKind::kFlipGoal && up.source.rfind("/*M*/", 0) == 0) ++to_module;
    const auto down = test::Mutate("/*M*/\nlet x = 1;\n{ using r = null; }", 4096, seed);
    if (down.edit == EditKind::kFlipGoal && down.source.rfind("/*M*/", 0) != 0) ++to_script;
    const auto sloppy = test::Mutate("with (a) { b; }\nvar c = 010;", 4096, seed);
    Require(sloppy.edit != EditKind::kFlipGoal, "sloppy code was flipped to a module");
  }
  Require(to_module > 0 && to_script > 0, "the goal flip never happens");
  std::cout << "mutator contract: ok\n";
  return 0;
}
