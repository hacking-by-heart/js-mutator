#include "js_mutator/mutator.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

unsigned ValidPercent() {
  const char* value = std::getenv("JS_MUTATOR_VALID_PERCENT");
  if (!value) return 90;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 0 || parsed > 100) return 90;
  return static_cast<unsigned>(parsed);
}

}  // namespace

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                           size_t max_size, unsigned int seed) {
  if (data == nullptr) return 0;
  static const js_mutator::Config config{ValidPercent()};
  auto result = js_mutator::Mutate(data, size, max_size, seed, config);
  if (!result.source.empty()) {
    std::memcpy(data, result.source.data(), result.source.size());
  }
  return result.source.size();
}
