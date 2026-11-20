#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace js_mutator {

enum class Outcome { kEditedValidLeaning, kEditedMalformed, kFallback,
                     kGeneratedValidLeaning };

enum class EditKind {
  kNone,
  kLiteral,             // replace a literal with a new one of the same kind
  kBinaryOperator,      // replace an operator with a compatible operator
  kWrapExpression,      // wrap a seed expression in a new construct
  kGenerateStatement,   // insert a generated statement using in-scope names
  kGenerateExpression,  // replace an expression with a generated one
  kWrapStatement,       // wrap a seed statement in a new construct
  kDeleteStatement,
  kDuplicateStatement,
  kSwapStatements,
  kReuseSubtree,        // reuse a seed function or class elsewhere
  kRepairSyntax,        // fix one syntax error in an invalid parent
  kCrossOver,           // transplant code from a second input
  kGenerateProgram,     // replace the parent with a generated program
  kDisruptSyntax,       // malformed mode: token-level corruption
  kInjectEarlyError,    // malformed mode: an early (parse-time) error
  kMagicComment,        // rebuild //# sourceURL, debugId, compile-hint comments
};

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

// Combine two inputs: code from `donor` is transplanted into `data`, or
// generated in `data` from the donor's building blocks.
Result CrossOver(const uint8_t* data, size_t size, const uint8_t* donor,
                 size_t donor_size, size_t max_size, uint32_t seed,
                 Config config = {});

}  // namespace js_mutator
