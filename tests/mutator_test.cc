#include "js_mutator/mutator.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

js_mutator::Result Mutate(const std::string& source, size_t max_size,
                          uint32_t seed, unsigned valid_percent = 100) {
  return js_mutator::Mutate(
      reinterpret_cast<const uint8_t*>(source.data()), source.size(),
      max_size, seed, {valid_percent});
}

}  // namespace

int main() {
  using js_mutator::EditKind;
  using js_mutator::Outcome;

  const auto zero = js_mutator::Mutate(nullptr, 100000, 0, 1);
  Require(zero.source.empty() && zero.outcome == Outcome::kFallback,
          "MaxSize=0 must return an empty fallback");

  const auto one = Mutate(";", 1, 1);
  Require(one.source == ";" && one.outcome == Outcome::kFallback,
          "one-byte input with MaxSize=1 must return ';'");

  const auto oversized = js_mutator::Mutate(nullptr, 65537, 8, 1);
  Require(oversized.source == ";" && oversized.outcome == Outcome::kFallback,
          "oversized parent must be rejected before it is read");

  const std::string parent = "function f() { return 1 + 2; }\n";
  std::array<bool, 4> seen{};
  for (uint32_t seed = 1; seed <= 256; ++seed) {
    const auto first = Mutate(parent, 4096, seed);
    const auto again = Mutate(parent, 4096, seed);
    Require(first.source == again.source && first.edit == again.edit &&
                first.outcome == again.outcome,
            "same seed and input must produce the same result");
    Require(first.source.size() <= 4096, "result exceeds MaxSize");
    if (first.outcome != Outcome::kEditedValidLeaning) continue;
    Require(first.source != parent, "edited output must differ from parent");
    Require(first.source.find("function f()") != std::string::npos,
            "edit must preserve the surrounding seed");
    const unsigned index = static_cast<unsigned>(first.edit) - 1;
    Require(index < seen.size(), "unexpected edit kind in valid mode");
    seen[index] = true;
  }
  for (bool found : seen) Require(found, "an initial edit rule was never used");

  bool malformed = false;
  for (uint32_t seed = 1; seed <= 32; ++seed) {
    const auto result = Mutate(parent, 4096, seed, 0);
    if (result.outcome == Outcome::kEditedMalformed) {
      Require(result.edit == EditKind::kDisruptSyntax,
              "malformed mode used an unexpected edit");
      malformed = true;
      break;
    }
  }
  Require(malformed, "malformed mode produced no edit");

  const auto comment_only = Mutate("const = ; // comment", 4096, 1, 0);
  Require(comment_only.source == ";" &&
              comment_only.outcome == Outcome::kFallback &&
              comment_only.edit == EditKind::kNone,
          "comment-only append must not count as a syntax disruption");

  for (size_t max_size = 0; max_size <= 8; ++max_size) {
    Require(Mutate(parent, max_size, 3).source.size() <= max_size,
            "small MaxSize was exceeded");
  }

  const std::string many_statements(32768, ';');
  const auto work_limited = Mutate(many_statements, 65536, 7);
  Require(work_limited.source == ";" &&
              work_limited.outcome == Outcome::kFallback,
          "work exhaustion must return the fallback");
  return 0;
}
