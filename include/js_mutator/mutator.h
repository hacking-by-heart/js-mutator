#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace js_mutator {

enum class Outcome { kEditedValidLeaning, kEditedMalformed, kFallback };
enum class EditKind { kNone, kLiteral, kBinaryOperator, kWrapExpression,
                      kInsertStatement, kDisruptSyntax };

struct Config {
  unsigned valid_percent = 90;
};

struct Result {
  std::string source;
  Outcome outcome = Outcome::kFallback;
  EditKind edit = EditKind::kNone;
};

Result Mutate(const uint8_t* data, size_t size, size_t max_size,
              uint32_t seed, Config config = {});

}  // namespace js_mutator
