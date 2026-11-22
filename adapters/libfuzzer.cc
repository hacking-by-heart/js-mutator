#include "js_mutator/mutator.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef JS_MUTATOR_YIELD_LOG
#include <cstdio>
#include <string>

#include <unistd.h>
#endif

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

#ifdef JS_MUTATOR_YIELD_LOG
// Yield-log build (CMake option JS_MUTATOR_YIELD_LOG, a separate fuzzer
// binary): one line per emitted unit, "sha1 edit outcome callback", in
// js_mutator_yield.<pid>.log in the working directory. libFuzzer names corpus
// files by SHA1, so tools/yield.py joins the log with the corpus to count
// what each EditKind emitted and what was kept.
std::string Sha1Hex(const uint8_t* data, size_t size) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string message(reinterpret_cast<const char*>(data), size);
  const uint64_t bits = static_cast<uint64_t>(size) * 8;
  message += static_cast<char>(0x80);
  while (message.size() % 64 != 56) message += '\0';
  for (int i = 7; i >= 0; --i) message += static_cast<char>((bits >> (i * 8)) & 0xff);
  auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = 0;
      for (int j = 0; j < 4; ++j) {
        w[i] = (w[i] << 8) | static_cast<uint8_t>(message[chunk + i * 4 + j]);
      }
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  char out[41];
  for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
  return std::string(out, 40);
}

void LogYield(const js_mutator::Result& result, const char* callback) {
  static std::FILE* log = [] {
    char name[64];
    std::snprintf(name, sizeof(name), "js_mutator_yield.%ld.log", static_cast<long>(getpid()));
    return std::fopen(name, "a");
  }();
  if (log == nullptr || result.source.empty()) return;
  const std::string sha1 =
      Sha1Hex(reinterpret_cast<const uint8_t*>(result.source.data()), result.source.size());
  std::fprintf(log, "%s %d %d %s\n", sha1.c_str(), static_cast<int>(result.edit),
               static_cast<int>(result.outcome), callback);
  std::fflush(log);
}
#else
void LogYield(const js_mutator::Result&, const char*) {}
#endif

}  // namespace

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                           size_t max_size, unsigned int seed) {
  if (data == nullptr) return 0;
  auto result = js_mutator::Mutate(data, size, max_size, seed, GetConfig());
  LogYield(result, "mutate");
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
  LogYield(result, "crossover");
  std::memcpy(out, result.source.data(), result.source.size());
  return result.source.size();
}
