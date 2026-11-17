# JavaScript mutator — design proposal

**Status:** first implementation in progress.

## Goal and first integration

The project is a standalone C++ library that turns a JavaScript seed program into a related candidate program. A small libFuzzer adapter will call it from `LLVMFuzzerCustomMutator`. The library will have no dependency on the target engine or its harness.

The first milestone is deliberately narrow: generate useful structural edits from real seed programs, keep output within `MaxSize`, reproduce mutations from a fixed seed, and measure how often the target parser accepts the result. Broader ECMAScript coverage follows from measured gaps.

## Data flow

```text
libFuzzer corpus parent
        │
        ▼
libFuzzer adapter: bytes, size, MaxSize, seed
        │
        ▼
mutation core: parse → choose eligible node → edit source span → check candidate
        │
        ▼
JavaScript candidate → target harness → libFuzzer coverage and corpus selection
```

The adapter only translates the callback arguments and copies the returned bytes. It does not contain JavaScript mutation rules. Other fuzzers or test programs can use the same core API.

## Core representation

Use a pinned Tree-sitter runtime and JavaScript grammar to find syntax nodes and byte ranges. Edit only selected spans of the original source, preserving the rest of the seed exactly. This avoids building a JavaScript printer in the first milestone.

Each edit rule declares the node types and parent contexts it accepts, the replacement it can produce, and a maximum output size check. For example, an expression replaces an expression; a statement insertion occurs only at a statement-list boundary. A seeded PRNG chooses among applicable edits within the limits below.

The first language profile is ECMAScript 2026 with the Script parse goal and standard syntax only. It guides rule selection and fixes the profile for target-parser measurements. Tree-sitter can accept module syntax and overlook ECMAScript early errors. A clean tree qualifies an edit as valid-leaning. The target parser determines actual acceptance.

### Per-call limits

Check the parent length before reading or copying it. A parent over 64 KiB goes directly to fallback. Before building a candidate, check that its byte length fits both `MaxSize` and 64 KiB. Skip edits that would exceed either size.

Allow up to four edit attempts. Across the initial parse and all attempts combined, allow 1,024 Tree-sitter progress callbacks and 65,536 node visits by the core. Use deterministic counters across reparses and retries; work exhaustion returns the fallback. The in-process first milestone measures peak memory against an 8 MiB target. It does not claim a hard per-call memory cap: Tree-sitter's process-wide allocator and allocation-failure behavior do not provide that guarantee.

**Project-owner decision (2026-11-17):** the in-process measured-memory approach was approved for this milestone. It replaces the earlier hard 8 MiB cap and host-survival-on-memory-exhaustion requirement.

## Initial mutation rules

Start with a few independently testable rules:

1. Replace a literal with a different literal of the same grammatical category.
2. Replace a binary operator with one valid in that expression context.
3. Wrap an eligible expression with a syntax-preserving construct, with parentheses when required.
4. Insert a small statement at a verified statement-list boundary.

Rules should use parent nodes and spans from the supplied seed. Later rules can address bindings and scopes, functions and arrows, classes and static blocks, templates, modules, and cross-input composition. Each rule gets examples of accepted contexts and rejected contexts before it is enabled in fuzzing.

## Input modes and fallback

- **Valid-leaning mode** reparses each edit and accepts it only if the tree contains no `ERROR` or `MISSING` nodes. Measure acceptance by the target parser separately.
- **Malformed mode** makes one controlled syntax disruption near a real node or token. It reports an edit only when the candidate has more Tree-sitter `ERROR` or `MISSING` nodes than its parent; equal-count cases fall back conservatively.
- **Fallback** applies in either mode, for any parent. Recoverable subtrees remain eligible for edits. If no eligible edit fits, all attempts are rejected, or a work limit prevents completion, return the single ASCII byte `;`. When `MaxSize` is zero, return zero bytes immediately. Count fallbacks separately from edits in either mode, even if the bytes equal the parent.

The initial mode mix defaults to 90% valid-leaning and 10% malformed, and can be configured. Both modes obey `MaxSize` and never rely on a hard-coded vulnerability PoC.

## Coverage behavior

Stock libFuzzer keeps inputs that discover new features and selects them as future mutation parents. That is the first coverage loop: the core repeatedly edits structurally interesting descendants. The standard custom-mutator callback has no coverage or outcome argument, so the core cannot honestly update per-rule success rates from that callback alone.

The design leaves room for an adapter that can report candidate outcomes to the core. Once such a runner exists, rule weights can be adjusted from measured novelty per rule. Until then, rule selection may depend on the structure of the current parent but must not claim direct coverage learning.

## Build boundary and layout

Proposed layout after review:

```text
include/js_mutator/      public mutation API
src/                     parser wrapper, edit rules, mutation engine
adapters/libfuzzer/      LLVMFuzzerCustomMutator bridge
tests/                   core correctness and integration checks
docs/                    requirements, design, benchmark protocol
```

Build the core as a static C++ library with a pinned Tree-sitter dependency. Keep the mutation engine and its parser dependency out of target coverage instrumentation where the build system permits, so coverage reflects the code under test. AddressSanitizer checks for the mutator itself can be configured separately.

## Evidence required before expansion

- Identical input, seed, and configuration produce identical bytes; every result fits `MaxSize`.
- The one-byte parent `;` with `MaxSize = 1` returns `;` as a fallback. With `MaxSize = 0` it returns zero bytes. Verify that an oversized parent bypasses parsing and work exhaustion returns the fallback. Measure peak memory on representative and stress inputs.
- Each initial rule changes representative seeds, preserves unrelated source regions, and rejects incompatible contexts.
- Report output rates and target-parser acceptance separately for valid-leaning and malformed edits. For example, if the target rejects `export const x = 1` under the Script goal, count that rejection even when Tree-sitter accepts the edit.
- The core links unchanged into two different harnesses.
- Under equal time and seed corpora, compare execution rate and target coverage against libFuzzer's default mutation path.
- Every saved candidate can be replayed by the target harness as a single file.

## Review decisions

1. Is Tree-sitter plus conservative source-span edits acceptable for the first milestone, given its ECMAScript approximation? A full ECMAScript parser would increase precision and implementation cost.
2. Is the initial 90/10 valid-leaning versus malformed mix appropriate?
3. Is libFuzzer's indirect coverage loop sufficient for the first milestone, with direct per-rule learning deferred until a runner can provide feedback?

## References

- [libFuzzer callback contract](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/fuzzer/FuzzerInterface.h)
- [libFuzzer corpus and coverage behavior](https://llvm.org/docs/LibFuzzer.html)
- [Tree-sitter C API](https://github.com/tree-sitter/tree-sitter/blob/master/lib/include/tree_sitter/api.h)
- [Tree-sitter JavaScript grammar](https://github.com/tree-sitter/tree-sitter-javascript)
- [ECMAScript early errors](https://tc39.es/ecma262/2026/multipage/error-handling-and-language-extensions.html)
