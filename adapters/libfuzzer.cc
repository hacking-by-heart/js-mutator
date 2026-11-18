#include "js_mutator/mutator.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// JS_MUTATOR_VALID_PERCENT (0-100, default 90): how often output is valid
// JavaScript; the rest is deliberately malformed.
const js_mutator::Config& GetConfig() {
  static const js_mutator::Config config = [] {
    js_mutator::Config c;
    if (const char* value = std::getenv("JS_MUTATOR_VALID_PERCENT")) {
      char* end = nullptr;
      const long parsed = std::strtol(value, &end, 10);
      if (end != value && *end == '\0' && parsed >= 0 && parsed <= 100) {
        c.valid_percent = static_cast<unsigned>(parsed);
      }
    }
    return c;
  }();
  return config;
}

}  // namespace

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                           size_t max_size, unsigned int seed) {
  if (data == nullptr) return 0;
  auto result = js_mutator::Mutate(data, size, max_size, seed, GetConfig());
  if (!result.source.empty()) {
    std::memcpy(data, result.source.data(), result.source.size());
  }
  return result.source.size();
}

extern "C" size_t LLVMFuzzerCustomCrossOver(const uint8_t* data1, size_t size1,
                                             const uint8_t* data2, size_t size2,
                                             uint8_t* out, size_t max_out_size,
                                             unsigned int seed) {
  if (out == nullptr || max_out_size == 0) return 0;
  auto result = js_mutator::CrossOver(data1, size1, data2, size2, max_out_size,
                                      seed, GetConfig());
  if (result.source.size() > max_out_size) return 0;
  std::memcpy(out, result.source.data(), result.source.size());
  return result.source.size();
}
