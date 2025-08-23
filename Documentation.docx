# 🧠 C-Script: The Monolithic Guide  
**Low-level authority, through high-level scripting**

---

## 📌 Overview

C-Script is a directive-driven derivative of C that preserves full ABI compatibility while offering a scripting-like surface. It compiles to a single native executable with no visible intermediates (.o, .ll, .asm). It features:

- **Dual modes**:  
  - `@hardline`: strict memory, type, and aliasing posture  
  - `@softline`: expressive syntax sugar with zero runtime cost

- **Single-artifact builds**: One `.exe` output, no intermediates

- **Whole-program AOT + IR optimization**: Constant folding, LICM, SROA, vectorization, and more

- **Self-contained PGO**: Profile → rebuild → hot path optimization

- **Embedded toolchain option**: LLVM + LLD in-process

---

## 📁 File Model

- **Extension**: `.csc`
- **Structure**:  
  - Top-level `@directives`  
  - C-Script constructs (`fn`, `let`, `enum!`, `match`, `@unsafe`)  
  - Raw C (headers, types, macros) passed through unchanged

---

## 🧭 Directives

Placed anywhere (typically top), they configure build, optimization, linking, and semantics.

| Directive       | Values / Example                      | Description |
|----------------|----------------------------------------|-------------|
| `@out`         | `"hello.exe"`                          | Output filename |
| `@opt`         | `O0`, `O1`, `O2`, `O3`, `max`, `size`  | Optimization level |
| `@lto`         | `on`, `off`                            | Link-time optimization |
| `@profile`     | `on`, `off`, `auto`                    | Enables PGO |
| `@hardline`    | `on`, `off`                            | Enables strict diagnostics |
| `@softline`    | `on`, `off`                            | Enables syntax sugar |
| `@define`      | `NAME=VALUE`                           | Preprocessor macro |
| `@inc`         | `"path"`                               | Include path |
| `@libpath`     | `"path"`                               | Library path |
| `@link`        | `"libname"`                            | Link library |
| `@abi`         | `"sysv"`, `"msvc"`                     | ABI convention |
| `@guardian`    | `on`, `off`                            | Confirmation overlays |
| `@anim`        | `on`, `off`                            | Animated CLI spinner |
| `@muttrack`    | `on`, `off`                            | Mutation tracking instrumentation |

---

## 🧃 Softline Syntax (Zero-Cost Sugar)

### 🧩 Functions

```c
fn add(int a, int b) -> int => a + b;
fn clamp(int x, int lo, int hi) -> int {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}
```

Lowers to:

```c
static inline int add(int a, int b) { return (a + b); }
int clamp(int x, int lo, int hi) { ... }
```

### 🧩 Bindings

```c
let int a = 42;  // → const int a = 42;
var int b = 7;   // → int b = 7;
```

### 🧩 Enum + Exhaustive Switch

```c
enum! Color { Red, Green, Blue }

fn paint(Color c) -> void {
  CS_SWITCH_EXHAUSTIVE(Color, c)
    CS_CASE(Red): print("Red\n"); break;
    CS_CASE(Green): print("Green\n"); break;
    CS_CASE(Blue): print("Blue\n"); break;
  CS_SWITCH_END(Color, c)
}
```

Compiler checks for missing cases at compile time.

### 🧩 Match (Pattern Matching)

```c
match (x) {
  1 => print("One\n");
  2 | 3 => print("Two or Three\n");
  _ => print("Other\n");
};
```

Lowers to `if/else` ladder with destructuring support.

### 🧩 Unsafe Blocks

```c
@unsafe {
  int* p = (int*)0xDEADBEEF;
  *p = 42;
}
```

Wraps with `CS_UNSAFE_BEGIN` / `CS_UNSAFE_END` to relax warnings.

---

## 🧱 Prelude Macros

Included automatically:

- `print(...)` → `printf(...)`
- `CS_DEFER(...)` → RAII-style cleanup
- `likely(x)` / `unlikely(x)` → branch hints
- `CS_SWITCH_EXHAUSTIVE`, `CS_CASE`, `CS_SWITCH_END` → enum switch helpers
- `CS_HOT` → hot function attribute for PGO

---

## 🧮 Hardline Mode

Enabled via `@hardline on` or `--strict`. Guarantees:

- No implicit narrowing or signed/unsigned surprises
- Constexpr bounds checking for arrays
- Pointer provenance enforcement
- Exhaustive switch enforcement for `enum!`
- Compile-time UB detection (shift overflow, divide by zero, etc.)
- Lifetime diagnostics (e.g., returning address of local)

---

## 🔬 Mutation Tracking

Enabled via `@muttrack on`:

```c
CS_MUT_STORE(x, 42);
print("%llu\n", cs_mutation_count());
```

Tracks state changes via counter macros.

---

## 🔥 Profile-Guided Optimization

With `@profile on` or `auto`:

1. Instrumented build with `cs_prof_hit("fnName")`
2. Run once, collect hit counts
3. Rebuild with `CS_HOT` on hottest functions

No external tools required.

---

## 🧰 CLI Usage

```bash
cscriptc myprog.csc \
  --out app.exe \
  -O3 --lto \
  --profile=auto \
  --hardline \
  --link z --link pthread \
  --inc deps/include --libpath /opt/mylibs
```

---

## 🧪 Embedded Toolchain (Optional)

Define `CS_EMBED_LLVM` to compile and link in-process using LLVM + LLD. Supports IR-level profiling with `CS_PGO_EMBED`.

---

## 🧬 Grammar Summary (EBNF)

```ebnf
translation_unit ::= { directive | cs_item | c_passthrough } ;
directive ::= '@' ident { directive_arg } newline ;
cs_item ::= enum_bang | softline_fn | softline_fn_block | unsafe_block | match_stmt ;
enum_bang ::= 'enum!' ident '{' enumerator_list '}' ;
softline_fn ::= 'fn' ident '(' param_list? ')' '->' type '=>' expression ';' ;
softline_fn_block ::= 'fn' ident '(' param_list? ')' '->' type '{' block_body '}' ;
unsafe_block ::= '@unsafe' '{' block_body '}' ;
match_stmt ::= 'match' '(' expression ')' '{' case_list '}' ;
```

---

## 🧠 Semantic Lowering

| Construct       | Lowered Form |
|----------------|--------------|
| `fn`           | `static inline` C function |
| `let`          | `const` declaration |
| `var`          | mutable C declaration |
| `enum!`        | `typedef enum` + validity helpers |
| `match`        | `if/else` ladder |
| `@unsafe`      | pragma-wrapped block |
| `print(...)`   | `printf(...)` macro |

---

## 🧪 Examples

### Hello World

```c
@out "hello.exe"
fn main() -> int {
  print("Hello, world!\n");
  return 0;
}
```

### Enum Switch

```c
enum! Mode { Idle, Scan, Fire }

fn step(Mode m) -> void {
  CS_SWITCH_EXHAUSTIVE(Mode, m)
    CS_CASE(Idle): print("Idle\n"); break;
    CS_CASE(Scan): print("Scan\n"); break;
    CS_CASE(Fire): print("Fire\n"); break;
  CS_SWITCH_END(Mode, m)
}
```

---

## 🧩 Interop

- Use system headers: `@include <stdio.h>`
- Link libraries: `@link "m"`
- Call C functions directly
- C can call C-Script functions (same ABI)

---

## 🧱 Build Pipeline Summary

1. Parse directives
2. Lower softline syntax
3. Inject prelude
4. Analyze enums and switches
5. Instrument (if profiling)
6. Compile to C
7. Emit `.exe` via system CC or embedded LLVM

---

## 🧭 Final Notes

C-Script is a compact, expressive, directive-first language that lets you write systems code with the ergonomics of a script and the authority of C. It’s built for single-artifact builds, real compile-time diagnostics, and zero-cost abstractions.

You can build kernels, firmware, servers, games, and tools—all with one `.csc` file and one `.exe`.

---

If you'd like, I can scaffold a full tutorial series next: from writing your first `.csc`, to building capsule overlays and integrating mutation tracking in Crown.
