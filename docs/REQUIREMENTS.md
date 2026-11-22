# JavaScript mutator requirements — draft

**Purpose.** Build a standalone JavaScript source mutator that can be embedded in different fuzzing harnesses. libFuzzer is the first integration. This is a requirements draft for review, not an implementation plan.

## Implementation status (2026-11-19)

- **Generation from scratch:** recursive Script grammar with a compiled-in keyword and punctuator vocabulary and lexical rules for every literal kind (see [Design](DESIGN.md)).
- **Seed mutation with scope reuse:** names visible at the edit position (parameters, locals, loop, catch, and class bindings) and the seed's own expressions and functions feed the generator. In the acceptance test, 64% of code generated inside a function used its parameters.
- **New constructs and nesting:** growing `let x = 1 + 2;` produces all 69 tracked constructs. The nested shape `function f() { (a = class { static { this; } }) => {} }` appears both from empty input and through growth.
- **Valid by default:** Tree-sitter check plus an early-error checker. V8 accepted 9,996 of 10,000 valid-leaning samples; the 4 rejects are valid code that JavaScriptCore accepts, apparently a V8 parser bug. The malformed mode is rejected by V8 about 97% of the time.
- **Coverage loop:** retained inputs are the next parents; invalid parents are repaired instead of discarded; custom crossover combines corpus entries.
- **Bounded and reproducible:** deterministic, size- and work-limited, sanitizer-tested.
- **Profile:** ECMAScript Script and Module goals as accepted by V8 15.8 without experimental flags, including `using` declarations. Modules are tagged with a leading `/*M*/`. Decorators and auto-accessors are excluded (see Design).

## 1. Core interface and portability

- The mutation engine accepts source bytes, their length, a maximum output length, a random seed, and a JavaScript language profile. It returns mutated source bytes and their length. The current entry point fixes the profile to the ECMAScript 2026 Script profile described below; a profile selector remains a future interface extension.
- The core has no target engine or libFuzzer API dependency. A thin libFuzzer adapter exposes `LLVMFuzzerCustomMutator`; other harnesses can call the same core API.
- The same input, seed, and configuration must produce the same output. The result must fit within the supplied maximum length.
- Reject parents over 64 KiB before reading or copying them. Check prospective candidate length against both 64 KiB and the caller’s maximum before allocating the candidate.
- Each call may make up to eight edit attempts. The initial parse and all attempts share limits of 4,096 Tree-sitter progress callbacks and 262,144 node visits by the core. Work exhaustion returns the fallback. The first in-process version measures peak memory against an 8 MiB target; it does not promise a hard per-call memory cap. Configuration is loaded at startup, with no file or network access in the mutation hot path.
- The project owner approved the measured-memory approach for this milestone on 2026-11-17. This replaces the earlier hard 8 MiB cap and host-survival-on-memory-exhaustion requirement.

## 2. Seed programs

- The mutator uses the actual input supplied by the fuzzing engine as its mutation base. It must not replace every input with a fixed PoC or template.
- The initial `.js` files are supplied as a libFuzzer corpus directory. New coverage increasing inputs become future mutation bases.
- An empty or whitespace-only parent in valid-leaning mode is generated from the grammar. In malformed mode, empty input falls back. If generation cannot fit the caller's limit or produce a clean program, fallback applies.
- The same fallback applies to every non-empty parent in either mode, including malformed, oversized, and valid parents. If no eligible edit fits, the attempt budget is exhausted without an accepted edit, or a work limit prevents completion, return the single ASCII byte `;`. A maximum output length of zero returns zero bytes immediately. Report fallback separately from seed-derived edits. A diagnostic mode can show which seed and edit produced an output, without logging every normal fuzz iteration.

## 3. JavaScript aware mutation

- The mutator recognizes lexical context and structural context: identifiers and bindings, literals, regular expressions and template strings, expressions, statements, functions and arrows, classes and static blocks, and scripts versus modules. It accounts for precedence, required parentheses, and context sensitive constructs such as `return`, `await`, `yield`, `super`, and private names according to the selected language profile.
- Mutation operators include insertion, deletion, replacement, and composition of compatible structures. They should combine features found in seeds rather than only append unrelated tokens.
- The language profile guides edit selection and target-parser measurements. The first milestone uses ECMAScript 2026 with the Script parse goal and standard syntax only.
- The mutator generates lexical elements from language rules. Identifiers, literals, and context dependent grammar require generation rules beyond a finite list of keywords and punctuation.

## 4. Valid-leaning and intentionally malformed inputs

- Provide a valid-leaning mode and a controlled malformed mode with a configurable mix. A valid-leaning edit must reparse with no Tree-sitter `ERROR` or `MISSING` nodes. The grammar can accept syntax outside the selected profile and overlook ECMAScript early errors.
- Measure actual target-parser acceptance for each mode under the selected profile, counting rejections in the results. Report fallbacks separately. Validation must not silently discard all target parser failures or findings.
- Keep mutation output as source code so each candidate can be saved and replayed as a `.js` file.

## 5. Coverage feedback

- In the first version, libFuzzer supplies coverage guidance by retaining interesting candidates and feeding them back as mutation bases. The custom mutator must exploit that by editing the current input, not by generating the same program every time.
- `LLVMFuzzerCustomMutator` receives bytes, size, maximum size, and random seed; it receives **no direct coverage map or result for its previous edit**. Therefore, direct per-operator learning requires a separate, optional feedback interface from the runner or an offline analysis loop. That interface is a later design decision.
- The core should be able to accept such feedback when an adapter can provide it and adjust mutation strategy based on measured novelty. The first libFuzzer adapter cannot claim this behavior from the standard callback alone.
- Report mutation operator counts, parser acceptance, executions per second, and coverage growth so strategy changes can be judged against evidence.

## 6. Acceptance criteria

- A fixed input, seed, and configuration reproduce identical output; every output stays within the size limit.
- The one-byte parent `;` with `MaxSize = 1` returns `;` as a fallback. With `MaxSize = 0` it returns zero bytes. Verify the same size-dependent fallback for an oversized parent and for forced work exhaustion, including during the first parse. Report peak memory measurements separately.
- A set of representative seed programs produces diverse edits that retain identifiable parts of their parents. Every valid-leaning edit passes the Tree-sitter check. Report target-parser acceptance independently for valid-leaning and malformed edits.
- The same core can be linked to two different harnesses without modifying the core.
- Saved outputs, including crashes, replay as individual input files.

## Decisions before implementation

1. Is the initial default of 90% valid-leaning and 10% deliberately malformed appropriate?
2. Should the first version be a lexer/structure editor or a full AST based editor? This determines the achievable syntax precision and speed.
3. Is indirect libFuzzer coverage guidance sufficient initially, or is direct operator feedback required in the first version?

## Interface references

- [LLVM libFuzzer interface](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/fuzzer/FuzzerInterface.h): custom mutator arguments, size bound, and determinism.
- [LLVM libFuzzer guide](https://llvm.org/docs/LibFuzzer.html): corpus and coverage behavior.
