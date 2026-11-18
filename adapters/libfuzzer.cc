#include "js_mutator/mutator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Provided by libFuzzer when linked into a fuzzer; absent in unit tests. Mach-O
// does not resolve undefined weak references by default, so delegation to the
// built-in mutator is only available on ELF platforms such as Linux.
#if defined(__ELF__)
extern "C" size_t LLVMFuzzerMutate(uint8_t* data, size_t size, size_t max_size)
    __attribute__((weak));
static size_t (*const kDefaultMutate)(uint8_t*, size_t, size_t) = &LLVMFuzzerMutate;
#else
static size_t (*const kDefaultMutate)(uint8_t*, size_t, size_t) = nullptr;
#endif

namespace {

long EnvNumber(const char* name, long fallback, long min, long max) {
  const char* value = std::getenv(name);
  if (!value) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < min || parsed > max) return fallback;
  return parsed;
}

struct Settings {
  js_mutator::Config config;
  // Percentage of calls handed to libFuzzer's built-in byte mutator. 100
  // disables js-mutator entirely, for A/B comparisons in one binary.
  unsigned default_percent;
  // Upper bound on output size; 0 means libFuzzer's max_size only.
  size_t max_len;
};

const Settings& GetSettings() {
  static const Settings settings{
      {static_cast<unsigned>(EnvNumber("JS_MUTATOR_VALID_PERCENT", 90, 0, 100))},
      static_cast<unsigned>(EnvNumber("JS_MUTATOR_DEFAULT_PERCENT", 0, 0, 100)),
      static_cast<size_t>(EnvNumber("JS_MUTATOR_MAX_LEN", 0, 0, 1 << 20))};
  return settings;
}

size_t Limit(size_t max_size) {
  const size_t cap = GetSettings().max_len;
  return cap == 0 ? max_size : std::min(max_size, cap);
}

bool UseDefault(unsigned seed) {
  const unsigned percent = GetSettings().default_percent;
  return percent != 0 && kDefaultMutate != nullptr &&
         (seed * 2654435761u >> 16) % 100 < percent;
}

}  // namespace

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                           size_t max_size, unsigned int seed) {
  if (data == nullptr) return 0;
  if (UseDefault(seed)) return kDefaultMutate(data, size, max_size);
  auto result = js_mutator::Mutate(data, size, Limit(max_size), seed,
                                   GetSettings().config);
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
  auto result = js_mutator::CrossOver(data1, size1, data2, size2,
                                      Limit(max_out_size), seed,
                                      GetSettings().config);
  if (result.source.size() > max_out_size) return 0;
  std::memcpy(out, result.source.data(), result.source.size());
  return result.source.size();
}
