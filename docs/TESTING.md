# Verification record

Standalone project checks ran on Linux x64 on 2026-11-17, before the V8 integration smoke test.

| Check | Result |
| --- | --- |
| GCC 14 debug build and CTest | 2/2 tests passed |
| GCC 14 AddressSanitizer and UndefinedBehaviorSanitizer build | 2/2 tests passed |
| Clang 19 libFuzzer linkage smoke test | Custom mutator detected; 200 corpus runs completed; new inputs retained |
| Debug test process peak RSS | 11,664 to 11,832 KiB across runs |

The RSS number is for the whole process, including its baseline runtime and the large work-budget test. It is **not** a per-call live-allocation measurement, so the 8 MiB working-storage target remains unverified.

The standalone tests check deterministic output, `MaxSize` handling, oversized-parent rejection before reading, the four initial valid-leaning operators, malformed editing, a comment-only-edit regression, a work-limit fallback, and the libFuzzer adapter buffer boundary.

## V8 parser smoke test

Later on 2026-11-17, the mutator was linked into a V8 parser harness. GN generated the target, Ninja linked it, and a 100-run smoke test from one JavaScript seed exited successfully. libFuzzer reported `found LLVMFuzzerCustomMutator`, `Custom-` mutations, and 48 new units. Retained inputs included seed-derived expression edits and statement insertions. The run used `-max_len=2048` to match the harness size cap.

This confirms integration and mutation, not a parser-acceptance rate or an equal-time coverage improvement. ASan reported an invalid external-symbolizer path warning; stack symbolization was not verified.
