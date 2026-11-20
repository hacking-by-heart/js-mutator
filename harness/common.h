// Shared helpers for the js-mutator V8 fuzzer targets in this directory.
// These files build inside a V8 checkout (see docs/V8_INTEGRATION.md).

#ifndef JS_MUTATOR_HARNESS_COMMON_H_
#define JS_MUTATOR_HARNESS_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "include/v8-isolate.h"

namespace js_mutator_harness {

// Inputs above this size tend to find timeouts, not bugs.
constexpr size_t kMaxInputSize = 2048;

// A full GC after every input is slow; one every kGcEvery inputs keeps
// memory bounded.
constexpr unsigned kGcEvery = 64;

// Parse goal, chosen only by a leading tag comment. The tag stays in the
// source; it is an ordinary comment.
enum class Goal { kScript, kModule, kRepl, kEval };

inline bool HasTag(const uint8_t* data, size_t size, const char* tag) {
  return size >= 5 && memcmp(data, tag, 5) == 0;
}

inline Goal GoalOf(const uint8_t* data, size_t size) {
  if (HasTag(data, size, "/*M*/")) return Goal::kModule;
  if (HasTag(data, size, "/*R*/")) return Goal::kRepl;
  if (HasTag(data, size, "/*E*/")) return Goal::kEval;
  return Goal::kScript;
}

// FNV-1a over the input; harness choices that are not goals use its bits,
// so every choice is a pure function of the input.
inline uint64_t InputHash(const uint8_t* data, size_t size) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

inline bool IsAscii(const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (data[i] >= 0x80) return false;
  }
  return true;
}

inline void MaybeCollectGarbage(v8::Isolate* isolate) {
  static unsigned count = 0;
  if (++count % kGcEvery == 0) {
    isolate->RequestGarbageCollectionForTesting(
        v8::Isolate::kFullGarbageCollection);
  }
}

}  // namespace js_mutator_harness

#endif  // JS_MUTATOR_HARNESS_COMMON_H_
