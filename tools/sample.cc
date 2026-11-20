// Writes generated and mutated programs, one per record, for external checks:
//   js_mutator_sample [count] > samples.txt
// Records are separated by a NUL byte; each starts with "valid\n" or
// "malformed\n".
#include "js_mutator/mutator.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  const unsigned count = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 2000;
  const std::string seeds[] = {
      "function total(price, qty) { return price * qty; }\ntotal(2, 3);",
      "class Item { #id = 1; get id() { return this.#id; } static { this.n = 0; } }",
      "let x = 1 + 2;",
      "/*M*/\nimport d from \"/dev/null\";\nexport const k = d;\n{ using r = null; }",
      "class Outer { #a = 1; static #m() {} f() { return class { #a; g(o) { return o.#a; } }; } }"};
  const unsigned n = sizeof(seeds) / sizeof(seeds[0]);
  auto emit = [](const char* kind, const std::string& source) {
    std::fputs(kind, stdout);
    std::fputc('\n', stdout);
    std::fwrite(source.data(), 1, source.size(), stdout);
    std::fputc('\0', stdout);
  };
  for (unsigned seed = 1; seed <= count; ++seed) {
    emit("valid", js_mutator::Mutate(nullptr, 0, 2048, seed, {100}).source);
    std::string parent = seeds[seed % n];
    for (unsigned g = 0; g < 8; ++g) {
      auto result = js_mutator::Mutate(reinterpret_cast<const uint8_t*>(parent.data()),
                                       parent.size(), 2048, seed * 31 + g, {100});
      parent = result.source;
    }
    emit("valid", parent);
    auto broken = js_mutator::Mutate(reinterpret_cast<const uint8_t*>(seeds[seed % n].data()),
                                     seeds[seed % n].size(), 2048, seed, {0});
    emit("malformed", broken.source);
  }
  return 0;
}
