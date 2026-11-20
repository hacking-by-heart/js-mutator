# JavaScript mutator: design

## Goal

Generate and transform JavaScript so that coverage-guided fuzzing can reach every part of a JavaScript parser. Correct code is the default; deliberately broken code is a separate mode. The core is a C++ library with no engine dependency; `adapters/libfuzzer.cc` connects it to libFuzzer.

## Data flow

```text
libFuzzer parent (corpus entry)          second input (crossover)
        │                                          │
        ▼                                          ▼
 parse (Tree-sitter) ──► context + scope analysis at a chosen position
        │                     │
        │     ┌───────────────┴──────────────────────────────┐
        │     ▼                                              ▼
        │  grammar generator (generator.cc)          seed transforms (mutator.cc)
        │  uses visible names, seed expressions,     wrap, delete, duplicate, swap,
        │  seed functions/classes                    literal/operator, transplant
        ▼     └───────────────┬──────────────────────────────┘
   empty parent → whole       ▼
   program generated   candidate → Tree-sitter check + early-error check
                              ▼
                  libFuzzer runs it, keeps it if coverage grows
```

## Vocabulary and grammar

`generator.h` compiles in the JavaScript keyword and punctuator tables. Identifiers, numbers (decimal, hex, octal, binary, exponents, separators, BigInt, legacy octal in sloppy code), strings (every escape form, line continuations), templates (substitutions, and the invalid escapes that only tagged templates allow), and regular expressions come from lexical rules.

Regular expressions are built in two passes for one mode (legacy, `u` or `v`). The first pass plans groups under a depth and size budget and leaves placeholders for back references; the second resolves them once every capture and name is known, so forward and self references (numbered and `\k<name>`) appear. Group names may repeat only in sibling alternatives. The grammar covers modifier groups such as `(?i-s:...)`, lookaround, quantifiers, Unicode escapes and properties, Annex B escapes and non-quantifier braces in legacy mode only, and the `v`-mode class set grammar: unions, `&&` and `--` never mixed at one level, nested classes, `\q{...}`, and properties of strings, which never appear under negation. Syntax that needs extra V8 flags is not emitted.

The generator is a recursive grammar over the Script goal. Every rule composes with every other where the language allows it:

- **Statements:** all loop forms including `for await` and the Annex B `for (var x = 1 in o)`, labeled statements with `break L`/`continue L`, single-statement and block bodies, dangling `else`, `switch` with case declarations, `try`/`catch` with patterns and optional binding, `with` in sloppy code, directive prologues (`"use strict"`), automatic semicolon insertion, comments including HTML-like comments, and a hashbang.
- **Functions:** declarations and expressions, async and generator variants, arrows with and without parentheses, and parameters with defaults (including functions, classes and arrows as defaults), destructuring and rest. A function or arrow can be nested inside another's default value.
- **Classes:** heritage, constructors with `super(...)`, methods, getters and setters, static and instance fields, static blocks, `#private` fields, methods and accessors, `#x in obj`, and computed keys. Object literals support methods, accessors, computed and shorthand keys, spread and `__proto__`.
- **Direct eval:** about 6% of calls (more in class elements and methods) call `eval`, mostly directly, sometimes through `eval?.()`, `(0, eval)()` or `globalThis.eval()`. Parameter defaults use it alone or inside an arrow, and the statement edit places it in sloppy blocks with function declarations, catch blocks, `with`, derived-class methods, fields and static blocks.
- **Expressions:** every operator, printed with only the parentheses that precedence requires. That covers `??` mixing rules, `**` versus unary operators, optional chains (including `?.()` and private members), tagged templates, `new` with and without arguments, `import()` with options, `new.target`, `super.x`, `await`, `yield`/`yield*`, and destructuring assignment. Left-hand-side chains mix 3-6 member, call and template links (no template after `?.`), and conditional chains have 3-6 arms. A bare `yield` appears before each terminator (`]`, `)`, `}`, `:`, `,`, `;`, `in`).
- **Cover grammar:** one list of elements, each marked as usable in a binding pattern, an expression and/or an assignment target, is finished as an arrow head, a parenthesized expression, a destructuring assignment, an `async(...)` call (with `await`/`yield` as identifiers in sloppy script code) or an async arrow. Parameter defaults also use direct `eval`, `arguments` and classes with static blocks.

A context record (`GenContext`) gates every construct with legality rules: `return`, `await` and `yield` (including the ban in parameter lists), `break`/`continue` and their labels, `super` property access versus `super()` calls, `new.target`, `arguments` (banned in class fields and static blocks), private names, and strict mode. Class bodies and heritage are always strict. A scope stack provides visible names, and fresh names never collide with any name in the seed.

**Names and targets.** About 30% of binding names reuse a name from an enclosing scope, so the new binding shadows it: lexical names never come from the same scope (or, in a catch body, from the catch parameter), and var names only from outside the current function and never across a lexical declaration they would hoist through. Sloppy code also gets Annex B function declarations (in blocks, `if` bodies, labels and `switch` cases, often named like an outer binding), colliding simple parameters `(a, b, a)` in plain functions (which then never get `"use strict"`), and body `var`s that redeclare a non-simple parameter. In sloppy code that is neither async nor a generator, about 5% of binding names are contextually reserved words (`await`, `yield`, `let`, `static`, `implements`, `eval`, `arguments`, ...), and statements use `await:` labels, `if (c) let` + line break, `let` + line break + name as a declaration, `(let)[x] = y`, `using` + line break, `using[x]`, and `({await, yield, let, static})`. Identifiers are escaped at any position (`v\u0031`, `\u{0061}b`). Class members may be named `get`, `set`, `static`, `async` or `accessor`, and fields may end by automatic semicolon insertion. Assignment, update and for-in/of targets may be parenthesized (`(x) = 1`, `((o.p)) += 1`), and in sloppy code may be calls (`(f()) = 1`, `f()++`, `for ((f()) of o)`), a run-time error rather than an early one. Tree-sitter cannot parse parenthesized pattern elements (`[(o.p)] = r`), so they are not generated.

## Seed transforms

The parent is never discarded. For each edit, the mutator picks a position and computes that position's context and its *visible names*: parameters, locals, loop and catch bindings, and class and function names in enclosing scopes. Generated code at that position reuses those names and the seed's own expressions and functions.

| Edit | What it does |
|---|---|
| Generate statement | Insert a grammar-generated statement before, between, or after existing statements |
| Generate expression | Replace a value with a generated expression in the same context |
| Wrap statement | Put a seed statement inside `if`/`else`, `try`, a loop, `switch`, a label, a block, an arrow, function or generator called immediately (IIFE), a function, arrow, object method or class method that is never called (one or two levels deep), a class static block, `with`, or a strict function, but only where that keeps it legal |
| Wrap expression | Put a seed expression inside an arrow, a parameter default, a class field, a generator, a template, a conditional, a spread, and so on |
| Literal / operator | Replace a literal with another of the same kind, or an operator with a compatible one |
| Delete / duplicate / swap | Restructure statement lists while avoiding redeclarations and dead ends |
| Reuse subtree | Copy a seed function or class (self-contained, with no foreign private names) to another position |
| Crossover | Transplant a top-level statement from a second corpus input. Its declarations must not collide, and sloppy-only code never enters strict positions |
| Repair | For an invalid parent, insert a missing token or delete an error span, so invalid corpus entries stay useful |
| Rebind | Reuse a visible name: shadow it in a block (`let`, `const`, `class`, function), redeclare it with `var`, a catch parameter or a for-in head, reuse it as a parameter with a closure over it and a body `var` (as an immediately called function and as a lazily compiled declaration), or, in sloppy code, as an Annex B function or a duplicate parameter. The early-error check filters illegal results |
| Generate program | Occasionally (3%), replace the parent with a fresh program |
| Laziness | Turn a called function expression (`(function(){})()`) into `var g = function(){}; g();`, nest a statement two or three levels deep in functions that are never called, or add or remove the parentheses that make V8 compile a function eagerly |
| Capture | Replace a name with a closure that captures it (`(() => n)()`, a parameter default, a class static block, a shadowing `const`, `eval` or `with` around it, the mapped `arguments` object), or use a `let`/`const`/`class` binding from a closure before its declaration |
| Parameter scope | Move a seed expression or function into a new parameter default, never in a function with `"use strict"`, duplicate parameters, accessors, or after a rest parameter |
| Unwrap | Replace an expression with one of its operands (parenthesized where precedence needs it); functions and classes are never unwrapped |
| Delete range | Delete 2-8 consecutive statements of one list |
| Deepen | Nest one expression 16-800 times in context-neutral wrappers (`(…)`, `[…][0]`, `1 ? … : 0`, `!…`, `void …`) with one text splice, bounded by the remaining bytes |

Valid mode requires no new Tree-sitter error nodes and no new early errors. An internal checker covers redeclarations (including a `var` that hoists through a lexical declaration of an enclosing block, `for` head, `switch` or destructured catch parameter, and labelled functions), parameter and catch conflicts, duplicate parameters (legal only in sloppy plain functions with simple lists), `"use strict"` with non-simple parameters, function declarations as `if`, label and loop bodies, strict reserved words, `await`/`yield` as names where reserved, `eval`/`arguments` bindings and targets in strict code, escaped keywords, assignment targets (parenthesized patterns, optional chains, `new`, tagged templates, and calls in strict code, with logical operators, or in patterns), `break`/`continue`/`return` targets, duplicate labels, undeclared or duplicate private names, and duplicate constructors. Identifier escapes are decoded before names are compared. A parent that is already invalid can only improve. Valid mode never returns the `;` fallback while a regenerated program fits.

**Malformed mode** corrupts tokens (delete, duplicate, replace with a random keyword or punctuator, swap, truncate, open an unterminated string, template, regex or comment) or injects errors that V8 reports at parse time. Some are built from the seed itself: redeclaring one of its `let` bindings, or moving a seed statement that uses `await` or `break` across a function boundary. Others only the full parser reports, placed in bodies V8 preparses: `let z; var z;` or a `let` of a parameter name inside a function that is not parenthesized or called where it is defined, and parameter/body, catch, duplicate-parameter and `"use strict"`-with-defaults conflicts in both called and uncalled functions. Others are inserted at real positions: `return` outside a function, `super()` outside a derived constructor, octal literals in strict code, duplicate `__proto__`, invalid regex flags, cover-grammar errors such as `(...a, b) => 0` and `({a = 1})`, optional-chain errors such as ``a?.b`t` `` and `new a?.b()`, and more. A regexp-targeted edit (`kRegexMalformed`) applies one single-point corruption to a seed regexp literal (or to a fresh one), each tied to one V8 regexp error, or flips the flags into an invalid set.

## Coverage loop and limits

libFuzzer keeps coverage-increasing inputs and hands them back as parents, so complexity accumulates across generations. The callback receives no coverage information, so edit weights are fixed except for size: when the parent fills more than 70% of the limit, deleting and unwrapping weigh 4x and generating half; below 30%, the reverse. Each call makes up to 8 attempts within shared work limits (4,096 parser progress callbacks, 262,144 node visits). Generation from empty input targets 2,048 bytes regardless of `MaxSize`, so every program replays byte for byte at its exact size. The same input, seed and configuration always produce the same output.

## Out of scope

Module code (`import`/`export`, top-level `await`, `import.meta`) is not generated, because the lab harness parses Script code only. Decorators and `using` declarations are newer than the V8 12.5 lab build and are also excluded. JSX is not JavaScript.
