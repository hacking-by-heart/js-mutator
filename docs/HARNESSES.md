# V8 harnesses

A harness is the small program that hands each fuzzer input to V8. This repository has four, in `harness/`. Each one builds into its own fuzzer.

## How V8 handles a function

V8 turns JavaScript into bytecode before it runs it:

```text
JavaScript text  ->  parse (understand the text)  ->  compile (make bytecode)  ->  run
```

V8 is lazy about functions. Take this script:

```js
function a() { return 1; }
function b() { return 2; }
a();
```

- When the script loads, V8 reads the top level and only skims the bodies of `a` and `b`. It checks that they look valid and saves what it needs for later.
- `a()` is called, so V8 goes back, parses `a` fully and compiles it.
- `b` is never called, so it is never fully parsed.

That full parse is still the parser. It runs `Parser::ParseFunction` and rebuilds the function's outer scopes (`Scope::DeserializeScopeChain`) before it parses the body.

## The four harnesses

| Harness | Fuzzer | What it does |
|---|---|---|
| `parser.cc` | `v8_js_mutator_parser_fuzzer` | Parses the script once, the way V8 does when the script loads. Function bodies are only skimmed. The fastest harness. |
| `compile.cc` | `v8_js_mutator_compile_fuzzer` | Compiles the script without running it, then compiles every function as if it had been called (up to 64). Every function body gets the full parse. |
| `diff.cc` | `v8_js_mutator_diff_fuzzer` | Parses the script twice, lazily and eagerly, and crashes if the two results disagree. |
| `streaming.cc` | `v8_js_mutator_streaming_fuzzer` | Feeds the script to V8 in small chunks and in several text encodings, the way a browser receives a script while it downloads. |

## Why the compile harness is needed

The parser harness stops after the skim, so it never reaches the full parse of a function body. That is a large part of the parser: `Parser::ParseFunction`, rebuilding scopes, reusing the skim's saved data, and reparsing class field initializers. The compile harness forces that step for every function.

- **More of the parser.** In two hours from the same corpus, the compile harness reached 37,432 coverage edges and the parser harness 27,622.
- **It finds bugs the parser harness cannot.** It found a V8 crash while rebuilding a function's scopes. Every frame of that crash is parser code (`Parser::ParseFunction`, `Scope::DeserializeScopeChain`), and the parser harness never runs that code.
- **The cost.** It runs about half as many inputs per second, and it also generates bytecode, which is not parser code. Read a crash's stack to tell: frames in `src/parsing/` and `src/ast/` are the parser.

Run both: the parser harness for speed, the compile harness for the parts of the parser that only run when a function is first called.
