# C-Script

## C-Script — Low-level authority, through high-level scripting

C-Script is a directive-driven derivative of C that keeps 100% ABI-level compatibility with C while giving you a scripting-like authoring surface and a compiler that aggressively optimizes AOT, performs whole-program hardline checks, and links to a single .exe with no user-visible intermediates.

It does this with two complementary modes:

hardline — strict, razor-edged memory & type posture (no implicit narrowing, no UB-adjacent constructs, lifetime/alias checks, constexpr bounds when possible).

softline — expressive surface sugar, directive macros, and inline templates that compress “toxic” boilerplate back to clean C underneath.


## Result: a single optimized hello.exe, no .o, .ll, or .asm lying around.

1) Language model
1.1 Files, extensions, and mapping

Source extension: .csc

Direct mapping: Every C construct is valid in C-Script. You can paste ANSI/ISO C or C11 code and compile it unchanged.

Syntactic sugar (softline):

fn name(args) -> ret { ... } → ret name(args) { ... }

fn name(args) -> ret => expr; → static inline ret name(args){ return (expr); }

let T id = expr; → const T id = expr;

var T id = expr; → T id = expr;

print(...) → printf(...) (prelude provides the alias)

1.2 Directives (declarative build & semantics)

All directives start with @ and can appear anywhere before the first non-comment token (recommended at top):

Directive	Values	Effect
`@hardline on	off`	default: off
`@softline on	off`	default: on
`@opt O0	O1	O2
`@lto on	off`	default: on
`@jitreg on	off`	default: on
`@profile on	off`	default: off
@define NAME=VALUE		Adds a preprocessor definition for the C backend.
@inc "path"		Add an include search path.
@libpath "path"		Add a library search path.
@link "name"		Link a library (-lname on Unix; name.lib on Windows).
@out "name.exe"		Set the output filename.
`@abi "sysv	msvc	arm64sysv"`
`@pack 1	2	4
`@warn aserror	pedantic	relaxed`

AOT before LLVM-IR?
C-Script’s front-end does semantic resolution, constant folding, flow pruning, and directive mapping before we ever emit IR. We then emit LLVM-IR in-memory, run late passes (GVN, SROA, LICM, etc.), and finish to a native .exe via an embedded LLD/driver—without surfacing intermediates.

2) Hard-lining vs Soft-lining
2.1 Hard-lining guarantees (compile-time, zero-cost)

No implicit narrowing or signed/unsigned surprises (promotions must be explicit).

For arrays with static bounds, index expressions are constexpr-checked when resolvable.

For pointer arithmetic, hardline forbids mixing unrelated provenance without an explicit @unsafe block.

Enforces switch exhaustiveness on tagged enums (when using softline enum! macro).

UB-adjacent constructs are turned into errors (e.g., shifting by width or more).

Emits compile-time lifetime misuse diagnostics for obvious escapes in static scope (e.g., returning address of a local).

---

---

2.2 Soft-lining productivity (zero-overhead lowering)

Sugars lower to C with no runtime tax.

Inline templates for common patterns (RAII-ish scopes via macros, optional defer).

Paper-cuts gone: fn, =>, let/var, print, simple defer.

---

---

3) Optimizations (baked-in, zero-cost)

Early (pre-IR) AOT:

Constant folding & short-circuit simplification

Dead branch pruning under @hardline

Inline expansion of fn ... => expr where referenced

Mid (IR):

SROA, GVN, DCE, LICM, loop unroll/peel, vectorization, tail-call opt, inliner with cost model

Late (codegen):

Peephole (mov-elim, lea-fold), frame shrink, branch-probability guided layout, cold partitioning

JIT register memory handling (AOT):

Heuristic or profile-guided adaptive register allocation (spill-avoid hot blocks, remat for cold)

“JIT” here means just-in-time during compilation: allocation tuned by measured/heuristic hotness. No runtime overhead.

4) Errors resolved AOT

C-Script aims to surface errors before codegen:

Type/lifetime violations (hardline)

Provenance misuse (obvious cases)

Constant range/bounds (constexpr stage)

UB-adjacent shifting, division by known zero, out-of-range enum

Link-time failures surfaced as compile diagnostics (inline linker)

5) Tooling & CLI

CSC:

cscriptc [options] file.csc

Options:
  -o <file.exe>       override @out
  -O{0|1|2|3|max|size} override @opt
  --no-lto            disable @lto
  --strict            hardline on + pedantic-as-error
  --relaxed           softer diagnostics
  --cc <clang|gcc|cl> prefer backend toolchain (embedded driver still hides .o)
  --show-c            dump generated C (debug only; not kept by default)

---

6) Reference subset grammar (v0.1)

CSC:

translation-unit  := { directive | external-decl }+
directive         := '@' ident [tokens…] NEWLINE
external-decl     := function-def | decl | typedef | preproc | softline-fn
softline-fn       := 'fn' ident '(' param-list? ')' '->' type ('=>' expr ';' | compound-stmt)

param-list        := param { ',' param }*
param             := type ident
type              := C-type-token-seq…
decl              := ('let'|'var') type ident ('=' initializer)? ';' | C-declaration
expr, stmt, etc.  := as in C (with sugars lowering to standard C)

---

7) “C-Script ⇄ C” mapping highlights

ABI, layout, integer widths: identical to host C (C11 default)

Headers: use your C system headers directly: #include <stdio.h>

Interop: call C and be called by C without shims

enum! (softline macro) expands to a C enum + helper tables for hardline exhaustiveness checks in switch

8) Mini standard prelude (zero-cost)

print(...) → macro alias to printf

defer (optional macro) using for-scope trick

likely()/unlikely() hint macros

enum! helper (compile-time tables for switch checks)

static_assert shims for MSVC/older C

9) Working prototype compiler (single file)

Below is a compact, build-today reference compiler that:

Parses directives

Lowers softline sugars to portable C11

Injects a tiny prelude

Spawns your system C toolchain behind the scenes to produce a single .exe (no emitted .o, .ll, or .asm)

Enforces hardline posture via strict flags

It’s a real starting point you can compile now (C++17). It is intentionally modest to keep it readable; extend passes and checks as you grow the tool.

---


SH:

# Linux/macOS
g++ -std=c++17 cscriptc.cpp -o cscriptc
# Windows (MSYS2/Clang suggested)
clang++ -std=c++17 cscriptc.cpp -o cscriptc.exe

---

2, Create hello.csc:

---

11) Roadmap to “compiler wizardry”

Deep hardline: build a symbol table + simple flow analysis to reject lossy conversions, dangling escapes, narrowings, and unannotated aliasing.

Profile loop: when @profile on, spawn an instrumented ephemeral build, run quickly (possibly with a user-provided input seed), collect counters in memory, re-enter codegen with PGO, then discard the instrumented binary. Still ends with a single final .exe.

Embedded IR & linker: replace external CC calls with libLLVM + in-memory LLD so no intermediates ever hit disk, even ephemerally.

Template compressions: higher-level reusable patterns (safe ring buffers, static arenas, bitsets) that lower to zero-cost C.

Enum exhaustiveness: enum! macro + compile-time tables; hardline forbids non-exhaustive switch.

TL;DR

Paradigm: directive-first, dual-posture (hardline/softline) C with single-artifact builds.

Tagline: Low-level authority, through high-level scripting.

Output: one .exe only.

Optimizations: whole-program AOT + IR + codegen passes, profile-guided when requested.

Mapping: fully C-ABI compatible; drop in your C headers and code.

Tool: a working v0.1 compiler above—extend it into your dream build.

---

Quick demo

demo.csc

@hardline on
@opt max
@profile on          // triggers PGO two-pass
@out "demo.exe"

enum! Color { Red=1, Green, Blue, }

// Exhaustive switch OK:
int score(Color c){
    int s=0;
    CS_SWITCH_EXHAUSTIVE(Color, c)
        CS_CASE(Red):   s=10; break;
        CS_CASE(Green): s=20; break;
        CS_CASE(Blue):  s=30; break;
    CS_SWITCH_END(Color, c)
    return s;
}

// Softline fns (instrumented on first pass; hotness applied on second)
fn inc(int x) -> int => x+1;
fn triple(int x) -> int {
    int r = x*3;
    return r;
}

int main(void){
    let int a = 7;      // lowers to: const int a = 7;
    var int b = 9;      // lowers to: int b = 9;
    @unsafe {
        short s = a + 500000; // narrowed here (allowed only in @unsafe)
        (void)s;
    }
    print("score(Blue)=%d; inc(40)=%d; triple(5)=%d\n", score(Blue), inc(40), triple(5));
    return 0;
}

---

Build & run

SH:

g++ -std=gnu++17 cscriptc.cpp -o cscriptc
./cscriptc demo.csc
./demo.exe

---

If you intentionally omit a CS_CASE(Blue) you’ll get a compile‑time error listing the missing case(s).

With @profile on, you’ll see a quick instrumented build+run happen invisibly; then we rebuild and annotate the hottest softline fns with CS_HOT for the final optimized .exe.

What you’ve got now

Directive-first C with dual posture (hardline/softline), single‑artifact build, and baked‑in optimizations.

Real compile‑time analysis for enum! switch coverage (no runtime hand‑waving).

Self-contained PGO loop that doesn’t leak intermediates and meaningfully steers codegen via hot attributes.

---

# C-Script 1.0 — The Monolith

*Low-level authority, through high-level scripting.*

This is the **complete, unified overview** of the *final* C-Script language and toolchain—what ships at 1.0. It reads as a spec, cookbook, and operator’s manual in one. You can start from here and build kernels, firmware, servers, games, and little tools, all with the same **one-file → one .exe** experience.

---

## 0) North Star

* **Directive paradigm.** Files are “programs + build files” in one. Lines starting with `@` steer compilation, linking, target, and memory semantics.
* **Hard-line / Soft-line duality.**

  * **Hard-line**: enforce strict types, integer/pointer conversions, aliasing, and boundary guarantees; maximize diagnostics; UB-averse lowering.
  * **Soft-line**: expressive syntax sugar (`fn`, `let`, `=>`, `enum!`, etc.), macro-like templates—*zero-cost* lowering to C/LLVM constructs.
* **Exactly one output**: a native **`.exe`** (or platform equivalent). No `.o`, `.ll`, `.asm` left behind. Intermediates live in memory or temp and are destroyed.
* **AOT + IR-aware** front-end: semantic resolution, analysis, and aggressive optimizations **before** emitting LLVM IR.
* **IR-embedded PGO**: profile in process (no external toolchain), rebuild hot paths, and multi-version where useful.
* **JIT register policy**: the produced exe embeds *function multi-versions* using alternative regalloc/alloca and inlining strategies; startup and/or on-device sampling selects the best variant. No runtime JIT of user code; just **adaptive dispatch** among prebuilt variants.
* **Direct C mapping**: every legal C construct has a stable mapping; the C ABI is the default FFI; headers are welcomed.
* **Non-toxic boilerplate**: modern conveniences that compress classic C verbosity **without cost**.

---

## 1) File Model & Tooling

A C-Script source file is a UTF-8 text program with **inline build directives**:

```c
// hello.csc

@out "hello.exe"
@opt O3
@lto on
@hardline on
@profile auto         // off|on|auto; auto instruments, runs once (if runnable), rebuilds hot

@link "m"             // -lm / libm
@inc  "deps/include"  // add header search path
@define FOO=1

fn main(argc:int, argv: **char) -> int {
    print("Hello, world! argc=%d\n", argc);
    return 0;
}
```

### 1.1 Directives (declarative, per-file or scoped)

Directives are processed top-down before lowering. The common set:

* **Build/Output**

  * `@out "prog.exe"` – output file name.
  * `@opt O0|O1|O2|O3|max|size` – optimization level; `max` implies full inliner+vectorizer pipeline.
  * `@lto on|off` – ThinLTO whole-program when appropriate.
  * `@target "x86_64-pc-windows-msvc"` – default is host triple.
  * `@abi "sysv"| "msvc"` – choose calling convention set.
  * `@cc feature(+sse4.2, +crc32)` – CPU feature hints for multiversioning.
* **Linking**

  * `@link "z"` `@link "pthread"` – add libs.
  * `@libpath "/opt/mylibs"` – search paths.
  * `@inc "/path/include"` – header search paths.
* **Preprocessing / Globals**

  * `@define NAME=VALUE` – predefine a macro visible to hardline C as well.
  * `@include <stdio.h>` or `@include "foo.h"` – sugar for passing headers through.
* **Modes**

  * `@hardline on|off` – toggle strict mode (see §4).
  * `@softline on|off` – toggle sugars (§2).
  * `@profile off|on|auto` – IR-embedded profiling pass; `auto` runs once, then rebuilds hot.
  * `@regjit on|off` – emit/disable register-policy multiversioning.
  * `@sanitize address|undefined|thread` – dev-only sanitizers (not default); preserved as build-only.
* **Packaging**

  * `@unit "driver"` – label the current translation unit.
  * `@use "other.csc"` – import additional C-Script files into the build (single-exe link).

**Scope note:** Directives apply from the line they appear until overridden. Use them at top of file for clarity.

---

## 2) Soft-line Language

Soft-line is the pleasant layer. It lowers *exactly* to C/LLVM equivalents with no runtime overhead.

### 2.1 Functions

**Expression-bodied:**

```c
fn add(a:int, b:int) -> int => a + b;

fn tri(n:int) -> int => n*(n+1)/2;
```

**Block form:**

```c
fn clamp(x:int, lo:int, hi:int) -> int {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}
```

**Attributes (compile-time only):**

* `@hot`, `@cold`, `@inline`, `@noinline`, `@flatten`, `@unroll(n)`, `@likely`, `@unlikely`.

```c
@hot @inline
fn dot3(a:*float, b:*float) -> float {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
```

### 2.2 Bindings

```c
let pi = 3.1415926535;   // lowers to `const` where possible
var i  = 0;              // lowers to a normal local (no 'auto' storage penalty)
```

**Type inference** is local and *constant-folded*, but everything lowers to explicit C types before IR.

### 2.3 `enum!` (exhaustive, zero-cost)

```c
enum! Color { Red, Green, Blue }

fn paint(c:Color) -> void {
    switch!(c) {
        case Red:   print("R\n");
        case Green: print("G\n");
        case Blue:  print("B\n");
    }
}
```

* `enum!` generates a `typedef enum` and hidden validity helpers.
* `switch!` is **compile-time exhaustive**; missing cases are a hard error under hard-line.

### 2.4 Slices, Views, and Small Generics

C-Script gives you **zero-cost views** that lower to plain structs (pointer + length) with inliner-friendly helpers.

```c
// Built-ins
type view[T] = struct { T* ptr; size_t len; };

// Sugar constructors
fn view_of[T](p:*T, n:size_t) -> view[T] => (view[T]){ p, n };

fn sum(v:view[int]) -> long {
    var s: long = 0;
    for (var i=0; i<(long)v.len; ++i) s += v.ptr[i];
    return s;
}
```

No hidden bounds checks; if `@hardline on`, analyzers warn on non-dominated bounds guards.

### 2.5 Defer & Scope Guards

```c
fn copy(src: *char, dst: *char, n:size_t) -> void {
    var f = fopen("log.txt", "a");
    defer { if (f) fclose(f); }      // lowers to a no-alloc for/defer pattern

    memcpy(dst, src, n);
}
```

### 2.6 `@unsafe` Aisle

```c
@unsafe {
    var p = (int*)0xDEADBEEF;    // ok inside; flagged if it escapes scope (hard-line check)
    *p = 42;
}
```

`@unsafe` is a *fenced* region: warnings suppressed for a curated set (casts, aliasing, volatile juggling). Outside of it, hard-line rules apply.

---

## 3) Hard-line Semantics (When the gloves go on)

Turned on by `@hardline on` or `--strict`. It is still C—just the nicest, safest corner of it.

* **Conversions**: no silent narrowing (e.g., `long -> int`) without explicit cast; signed/unsigned mixes warn/error.
* **Pointers**:

  * disallow arithmetic that leaves the object or array bounds without a dominating guard;
  * flag casts between unrelated pointer types (except via `uintptr_t` in `@unsafe`).
* **Aliasing**: `@noalias` parameter attribute enforces TBAA-friendly lowering.
* **Init**: all locals must be definitely assigned before use.
* **Enums**: `switch!` must be exhaustive.
* **`fallthrough`** requires `@fallthrough` annotation to avoid accidental slips.
* **Atomics**: only via `@atomic` intrinsics or C11 `<stdatomic.h>`; mixed plain/atomic is a diagnostic.
* **Undefined behavior**: analyzer surfaces common UB shapes (shift overflow, div zero, out-of-range enum, trap reps) as compile errors whenever provable.

---

## 4) Types, Declarations, and Interop

### 4.1 Primitive & Derived

* All C scalar types are present: `char`, `short`, `int`, `long`, `long long`, fixed-width `int32_t` etc., `float`, `double`, `_Bool` as `bool`.
* Pointers `*T`, arrays `T[n]`, function pointers `fnptr` (exact C signatures), structs/unions, enums.

### 4.2 Structs/Unions/Bitfields

C-Script mirrors C layout. Attributes:

```c
@packed struct Header { u16 kind; u32 len; }
@aligned(64) struct Line { float x,y; float m,b; }
```

### 4.3 FFI (C ABI by default)

Include headers and call functions directly:

```c
@include <stdio.h>
@link "m"

extern fn cos(double) -> double;   // optional; usually pulled via @include
```

Or wrap with **extern blocks**:

```c
extern @header("<time.h>") {
    fn clock() -> long;
}
```

---

## 5) Control Flow & Expressions

* `if`, `switch`, `while`, `do/while`, `for` lower to the identical C forms.
* `switch!` is the exhaustive variant; plain `switch` behaves like C.
* `guard` sugar (optional): `guard (cond) {return err;}` lowers to `if(!(cond)) { … }`.

---

## 6) Compile-time Facilities

### 6.1 `static_assert`, `constexpr`, and `meta`

* `static_assert(expr, "msg")` as in C11.
* **Const-eval** expressions fold aggressively.
* **`meta` evaluates at compile time** and splices results (like a hygienic macro) into the lowered C:

```c
meta {
    // computed constants, tables
    let LUT = gen_sine_table(1024);
}
```

(Internally this is a front-end phase—not C++ template metaprogramming—so it’s fast and predictable.)

### 6.2 Templates (Monomorphized Helpers)

C-Script provides **parametric helpers** that become concrete C at emit time:

```c
template swap[T](a:*T, b:*T) -> void {
    let t = *a; *a = *b; *b = t;
}

fn demo() -> void {
    var x=1, y=2;
    swap[int](&x, &y);
}
```

Monomorphization is **AOT**; you pay only for used instantiations.

---

## 7) Concurrency & Atomics

* **Threads**: `spawn` is portable sugar that lowers to pthreads/WinThreads.

```c
fn worker(arg:*void) -> *void { /* ... */ return null; }
var t = spawn worker(null);   // returns a join handle
join t;                       // lowers to pthread_join / WaitForSingleObject
```

* **Channels (opt-in)**: `chan[T]` lowers to a small lock+ring buffer implementation when used; header-only, inlined.

* **Atomics**: `atomic_load`, `atomic_store`, `atomic_fetch_add` map to C11 atomics, with memory orders as enums.

---

## 8) Diagnostics & Errors (AOT)

**All resolvable errors are compile-time.** The front end performs:

* Symbol and type resolution with full range checks and conversion audits.
* Enum exhaustiveness proofs.
* Bound dominance checks in hard-line mode.
* UB shape checks (provable ones).
* Cross-TU link sanity (missing symbol, ABI mismatch) **before emitting IR**.

Errors are printed with **file\:line\:col**, code snippet, and a suggested fix when simple.

---

## 9) Optimizations

**Zero-cost by default**: sugar erases, IR is clean.

* **Constant folding, DCE, SCCP, mem2reg, instcombine.**
* **Loop opts**: unroll heuristic + `@unroll(n)` override; vectorizer when profitable.
* **Tail calls, inlining** driven by profile/hints.
* **Peepholes** from front-end (e.g., immediate forms).
* **Wholetime**: if `@lto on`, cross-unit inlining and de-virt (where applicable).
* **Code layout** guided by PGO.

---

## 10) Profile-Guided Build (fully in-proc)

* `@profile on|auto` instruments **at IR level**: every defined `fn` gets a lightweight `cs_prof_hit("name")`.
* The driver runs the instrumented exe (if `auto` and runnable), collects counts, selects top hot functions, and **rebuilds** with:

  * `@hot` attributes,
  * larger inline budgets,
  * optional **function multi-versioning** (different register pressure/inlining/regalloc tradeoffs),
  * code layout optimized for fall-through.

No external profraw/profdata or compiler subprocess is required.

---

## 11) JIT Register Policy (without a JIT)

The final exe can embed **N variants** of a hot function (e.g., *reg-tight*, *spill-resistant*, *vector-happy*). At program start (or after a brief warm-up, depending on `@regjit` policy), a tiny selector:

1. Detects CPU features (ISA, core counts, L1/L2 sizes).
2. Samples a few calls or consults persisted profile.
3. Patches the dispatch table (an indirect jump or PLT-style thunk) to the best variant.

**No runtime IR/JIT**—just **static variants** plus a 1-hop indirection resolved once.

---

## 12) Memory Semantics

* `@noalias` on function parameters promises independent object lifetimes (lowers to TBAA-friendly attributes).
* `@restrict` (C99 semantics) recognized and preserved.
* `@aligned(N)`, `@assume_aligned(N)`, `@likely`, `@unlikely`.
* Stack allocation is plain C; optional `@stack(nbytes)` hints large frames to static allocation arenas on freestanding targets.
* **Lifetime notes**: The analyzer warns on escaping stack addresses, double-frees (provable), and use-after-free patterns when detectable.

---

## 13) Packaging, Modules, and C/C++ Integration

### 13.1 Multi-file

```c
// driver.csc
@unit "driver"
@use  "math.csc"
@out "app.exe"

fn main(...) -> int { return run(); }
```

```c
// math.csc
@unit "math"
fn run() -> int { return 0; }
```

All `@use`d units compile into the **same .exe**. The front-end guarantees consistent ABI and attribute merging before IR.

### 13.2 Headers

* `@include` lines are forwarded to the C surface as `#include` with managed search paths.
* C++ headers are allowed behind `@cpp on` blocks; you control mangling with `extern "C"` declarations. (C-Script itself is C-ABI by default.)

---

## 14) Standard Prelude (Always Available)

* `print` → `printf`
* `defer` scope guard
* `likely`/`unlikely`
* Small helpers for `view[T]`, `min`, `max`, `swap[T]`
* Exhaustive switch helpers behind `switch!`

Everything is *header-sized* and compiled into your TU; nothing dynamic or hidden.

---

## 15) Examples

### 15.1 Exhaustive control with hard-line

```c
@hardline on

enum! Mode { Idle, Scan, Fire }

fn step(m:Mode) -> void {
    switch!(m) {
        case Idle: print("idle\n");
        case Scan: print("scan\n");
        case Fire: print("fire\n");
    }
}
```

If you forget a case, the compiler errors out before IR emission.

### 15.2 Views and fast loops

```c
fn saxpy(a:float, x:view[float], y:view[float]) -> void {
    // hard-line will warn unless len guards dominate the loop
    if (x.len != y.len) return;

    for (var i=0; i<(long)x.len; ++i) {
        y.ptr[i] = a*x.ptr[i] + y.ptr[i];
    }
}
```

`view[T]` lowers to `{T*, size_t}` and gets vectorized at `-O3`.

### 15.3 Inline linking and feature flags

```c
@link "pthread"
@cc feature(+sse4.2, +popcnt)
@regjit on
@profile auto
@opt max
```

The build produces one optimized `.exe` with hot function multi-versioning and PGO-driven layout.

---

## 16) Command-line UX (mirrors directives)

```
cscriptc myprog.csc \
  --out app.exe \
  -O3 --lto \
  --profile=auto \
  --hardline \
  --link z --link pthread \
  --inc deps/include --libpath /opt/mylibs
```

CLI flags and `@` directives are merged; CLI wins when both are given.

---

## 17) Targets & Runtimes

* **Hosted**: Windows (COFF/MSVC ABI), Linux (ELF/SysV), macOS (Mach-O).
* **Freestanding**: Bare-metal targets can be selected with `@target` + `@abi`, and you provide start files via `@link`/`@libpath`. The driver still emits a single image.

---

## 18) Guarantees

* **Performance**: sugar erases; IR is equivalent (or better) than hand-written C at the same flags.
* **Stability**: Direct C mapping is normative—any C header is welcome; the ABI does not drift.
* **Composability**: one TU or many, still one exe.
* **Diagnostics first**: if the tool can prove a mistake, you hear about it **before** IR exists.

---

## 19) What’s Not in the Language

* No GC, no exceptions, no implicit heap.
* No runtime reflection (compile-time `meta` only).
* No hidden bounds checks or panics; your guards are yours.
* No mandatory runtime; the prelude is tiny and visible.

---

## 20) Quick Reference (Cheatsheet)

* **Sugar**

  * `fn name(args) -> ret => expr;`
  * `fn name(args) -> ret { ... }`
  * `let x = ...;` (const) · `var x = ...;` (mutable)
  * `enum! Name { A, B, C }` with `switch!(value){ case A: ... }`
  * `defer { ... }`
  * `@unsafe { ... }`
* **Directives**

  * Build: `@out`, `@opt`, `@lto`, `@target`, `@abi`
  * Profile: `@profile`, `@regjit`
  * Link: `@link`, `@libpath`, `@inc`, `@include`
  * Modes: `@hardline`, `@softline`, `@sanitize`
  * Units: `@unit`, `@use`
* **Attrs**

  * `@hot`, `@cold`, `@inline`, `@noinline`, `@unroll(n)`, `@packed`, `@aligned(n)`, `@noalias`, `@restrict`

---

## 21) Closing

C-Script 1.0 is a compact language with a **directive brain** and a **C soul**: the fastest way to write serious systems code that still reads like a script. You keep the metal **and** the ergonomics. The compiler does the orchestration—AOT semantics, IR-aware optimization, **in-proc** profiling, and a single clean artifact on disk.

---

# C-Script 1.0 — Unified Language Reference (Grammar • Syntax • Semantics)

This is a single, self-contained specification of the C-Script language and its compiler toolchain as of the “Monolith” design described in the public repo. It consolidates the *language model*, *surface syntax & sugars*, *directives*, *compilation semantics*, and a practical EBNF-style grammar for the subset that extends C. C-Script is a directive-driven derivative of C with two complementary modes:

* **Hard-line:** strict, UB-averse posture and aggressive diagnostics.
* **Soft-line:** expressive sugars that lower to plain C/LLVM with zero runtime cost. ([GitHub][1])

C-Script produces **a single native executable** (no visible `.o`, `.ll`, `.asm` outputs) via an **IR-aware** front-end and late optimization/link in process. All valid C (ANSI/ISO, C11 default) is valid C-Script; you can paste it unchanged. ([GitHub][1])

---

## 0) Language Model & Goals

* **Directive paradigm.** Lines beginning with `@` (typically at the top) control build, target, linking, and semantic modes. They are processed before lowering. ([GitHub][1])
* **Dual posture.** Hard-line enforces strict types, integer/pointer conversions, aliasing, boundary guarantees, and exhaustiveness; soft-line offers sugars like `fn`, `let`, `=>`, `enum!`, etc., all lowering to C without cost. ([GitHub][1])
* **Single-artifact output** with **AOT semantic analysis before IR**, and an **embedded link** step. ([GitHub][1])
* **Self-contained PGO.** When profiling is enabled, the compiler performs an instrumented build+run, then rebuilds and marks hottest soft-line functions as hot for the final exe. ([GitHub][1])
* **Direct C mapping.** ABI/layout/widths match host C; headers are used directly; interop is seamless; C-Script additions expand to ordinary C constructs. ([GitHub][1])

---

## 1) Files, CLI, and Build Model

### 1.1 Source Files

* **Extension:** `.csc`
* **Content:** Directives (`@...`) followed by ordinary C plus soft-line sugar.
* **Mapping:** Any legal C construct is accepted verbatim. Sugars lower to C/LLVM during compile. ([GitHub][1])

### 1.2 Command Line

```
cscriptc [options] file.csc
```

**Notable options** (override in-file directives):
`-o <file.exe>` (override `@out`) · `-O{0|1|2|3|max|size}` (override `@opt`) · `--no-lto` (disable `@lto`) · `--strict` (enable hard-line + treat pedantic warnings as errors) · `--relaxed` (softer diagnostics) · `--cc <clang|gcc|cl>` (prefer backend toolchain even though the driver hides intermediates) · `--show-c` (dump generated C). ([GitHub][1])

---

## 2) Directives (Declarative Build & Semantics)

Directives begin with `@` and are processed top-down prior to lowering. They typically appear before the first non-comment token. **Scope**: from the line they appear until overridden. ([GitHub][1])

### 2.1 Build / Output

* `@out "prog.exe"` — output file name.
* `@opt O0|O1|O2|O3|max|size` — optimization level; `max` enables a full inliner+vectorizer pipeline.
* `@lto on|off` — Whole-program ThinLTO when appropriate.
* `@target "triple"` — target triple; default is host.
* `@abi "sysv"|"msvc"` — calling convention set.
* `@cc feature(+sse4.2, +crc32)` — CPU features for multiversioning. ([GitHub][1])

### 2.2 Linking / Search Paths

* `@link "z"` / `@link "pthread"` — add a library.
* `@libpath "/opt/mylibs"` — add library search directory.
* `@inc "/path/include"` — add header search path.
* `@define NAME=VALUE` — predefine a macro visible to the C backend as well.
* `@include <stdio.h>` / `@include "foo.h"` — convenience header passthrough. ([GitHub][1])

### 2.3 Modes / Multi-file

* `@hardline on|off` (default off); `@softline on|off` (default on).
* `@profile off|on|auto` — `auto`: instrument→run if possible→rebuild hot.
* `@unit "name"` — label the translation unit.
* `@use "other.csc"` — import additional C-Script files into the same single-exe build. ([GitHub][1])

> **Design note:** The README presents hard/soft toggles, optimization/LTO controls, library paths, ABI/target, and a profiling mode that drives hot-path re-emission for the final exe. ([GitHub][1])

---

## 3) Soft-Line Surface (Zero-Cost Sugars)

These are syntax conveniences that lower to ordinary C during compile (no runtime overhead). Unless stated otherwise, the semantics are identical to the expanded C.

### 3.1 Functions

**Single-expression form**:

```
fn Name (param-list) -> RetType => expr ;
```

Lowers to:

```
static inline RetType Name(param-list) { return (expr); }
```

**Block form**:

```
fn Name (param-list) -> RetType { /* body */ }
```

Lowers to:

```
RetType Name(param-list) { /* body */ }
```

These are explicitly stated sugars in the README’s language model. ([GitHub][1])

### 3.2 Variable Declarations

* `let T id = expr;` → `const T id = expr;`
* `var T id = expr;` → `T id = expr;`
  Also valid without initializer when C allows. ([GitHub][1])

### 3.3 Prelude Convenience

* `print(...)` → macro alias to `printf(...)` (no overhead).
* Optional `defer` macro (for-scope trick).
* `likely(x) / unlikely(x)` hint macros. ([GitHub][1])

### 3.4 `enum!` Macro + Exhaustive Switch

```
enum! Color { Red, Green, Blue }
```

Expands to an ordinary C `enum` plus helper data for hard-line **switch exhaustiveness** checking when used with the provided macro pattern; compile-time errors list missing cases if you intentionally omit one. ([GitHub][1])

---

## 4) Hard-Line Semantics (Diagnostics & Guarantees)

When hard-line is enabled (via directive or `--strict`), the compiler aims to surface errors **before** codegen and to reject UB-adjacent constructs. Highlights:

* No implicit narrowing or signed/unsigned surprise conversions; promotions must be explicit.
* Constexpr-checkable array indexes are verified against static bounds.
* Pointer arithmetic forbids mixing unrelated provenance unless wrapped in an explicit unsafe region.
* Switch exhaustiveness is enforced for `enum!`-declared enums.
* UB-adjacent operations (e.g., shift by width or more, divide by known zero, out-of-range enum values) become errors.
* Obvious lifetime escapes in static scope (e.g., returning address of a local) are diagnosed. ([GitHub][1])

> The project’s stated goal is to surface type/lifetime/provenance/range errors at compile time, *prior* to IR emission, and to surface link-time failures as compile diagnostics thanks to the inline linker. ([GitHub][1])

---

## 5) Profiles, Hotness, and Codegen

With profiling enabled, C-Script runs a quick instrumented build+run, collects counts, and **rebuilds** the program marking **hottest soft-line functions** as hot (e.g., via a target-appropriate “hot” attribute), improving final code layout and inlining. This loop is self-contained—no leaking intermediates. ([GitHub][1])

---

## 6) Types, Interop, and Data Layout

* **Primitives:** all C scalars (`char`, `short`, `int`, `long`, `long long`, fixed-width `int32_t`/friends, `float`, `double`, and C’s `_Bool` visible as `bool`).
* **Derived:** pointers `*T`, arrays `T[n]`, function pointers, structs, unions, enums, and bitfields—all matching C’s ABI/layout on the host toolchain.
* **Attributes:** e.g., `@packed struct ...`, `@aligned(64) struct ...` for layout control.
* **Interop:** include and call C directly; C code can call into C-Script with the same ABI. ([GitHub][1])

> The “C-Script ⇄ C mapping highlights” section emphasizes identical ABI/layout/widths, direct header usage, and zero-shim interop. ([GitHub][1])

---

## 7) Mini-Standard Prelude (Zero-Cost)

C-Script ships a tiny prelude providing `print`, `defer`, and `likely/unlikely` hints as simple macros, intentionally avoiding a heavy runtime. ([GitHub][1])

---

## 8) Formal Grammar (EBNF-style Additions Over C)

> **Scope.** This grammar specifies the C-Script extensions and sugars on top of standard C11. Where a non-terminal delegates to C, the C11 grammar applies unchanged. A C-Script **translation unit** is a sequence of directives and ordinary C external declarations, plus the forms listed below. ([GitHub][1])

**Lexical**
C-Script uses C’s lexical structure (identifiers, literals, comments). The `@` prefix introduces **directives**.

```
letter        ::= 'A'..'Z' | 'a'..'z' | '_'
digit         ::= '0'..'9'
ident         ::= letter { letter | digit }*
string        ::= ...       // as in C
integer       ::= ...       // as in C
newline       ::= '\n' | '\r\n'
```

**Top Level** ([GitHub][1])

```
translation-unit
  ::= { directive | external-decl }+ ;

directive
  ::= '@' ident directive-tail? newline ;

directive-tail
  ::= tokens…           // parsed according to §2 (out/opt/lto/etc.)

external-decl
  ::= function-def
   |  declaration
   |  typedef
   |  preprocessor
   |  softline-fn ;
```

**Soft-line Function** ([GitHub][1])

```
softline-fn
  ::= 'fn' ident '(' param-list? ')' '->' type ( '=>' expr ';' | compound-stmt ) ;

param-list
  ::= param { ',' param }* ;

param
  ::= type ident ;

type
  ::= C-type-token-seq… ;   // use C’s type grammar
```

**Variable Declarations (Sugar)**

```
declaration
  ::= 'let' type ident ( '=' initializer )? ';'
   |  'var' type ident ( '=' initializer )? ';'
   |  c-declaration ;                 // any other C declaration
```

**Enum Macro (Soft-line)**

```
enum-macro
  ::= 'enum!' ident '{' enumerator-list '}' ;

enumerator-list
  ::= enumerator { ',' enumerator }* ;

enumerator
  ::= ident [ '=' constant-expr ]? ;   // as in C enum
```

**Exhaustive Switch Helper (Usage Pattern)**
C-Script expects a macro pattern (provided by the prelude) to check exhaustiveness for enums defined via `enum!`. Conceptually:

```
switch-exhaustive
  ::= 'CS_SWITCH_EXHAUSTIVE' '(' EnumType ',' expr ')' '{'
         { 'CS_CASE' '(' enumerator ')' ':' statement-seq }*
      'CS_SWITCH_END' '(' EnumType ',' expr ')' ';' ;
```

If any enumerator is missing, **compile-time error** lists exactly which are missing. ([GitHub][1])

**Directives** (token forms)
The following schematic forms are accepted (see §2 for meaning): ([GitHub][1])

```
'@out' string
'@opt' ('O0'|'O1'|'O2'|'O3'|'max'|'size')
'@lto' ('on'|'off')
'@target' string
'@abi' ('sysv'|'msvc')
'@link' string
'@libpath' string
'@inc' string
'@define' ident ['=' tokens…]
'@include' ('<' hdr '>' | '"' hdr '"')
'@profile' ('off'|'on'|'auto')
'@unit' string
'@use' string
'@hardline' ('on'|'off')
'@softline' ('on'|'off')
```

**Everything else** (expressions, statements, operators, precedence, preprocessing) follows **C11**. Any C code compiles as-is. ([GitHub][1])

---

## 9) Semantics of Lowering

* **`fn` single-expr** → `static inline` function returning `(expr)`.
* **`fn` block** → ordinary C function of the given return type.
* **`let`** → `const` introduction; **`var`** → plain C definition.
* **`enum!`** → expands to a C `enum` plus data enabling hard-line exhaustiveness checks.
* **Prelude** provides `print`, `defer`, `likely/unlikely` as macros.
* **No runtime tax:** lowering targets idiomatic C so the optimizer can erase abstraction cost. ([GitHub][1])

---

## 10) Examples

### 10.1 Hello

```c
@out "hello.exe"
@opt O3
@lto on
@hardline on

fn main(argc:int, argv:**char) -> int {
    print("Hello, world! argc=%d\n", argc);
    return 0;
}
```

A single optimized `hello.exe` is produced; intermediates are kept in memory or temp and removed. ([GitHub][1])

### 10.2 Enum + Exhaustive Switch

```c
enum! Color { Red, Green, Blue }

fn describe(c: Color) -> void {
    CS_SWITCH_EXHAUSTIVE(Color, c) {
        CS_CASE(Red):   print("red\n");
        CS_CASE(Green): print("green\n");
        CS_CASE(Blue):  print("blue\n");
    } CS_SWITCH_END(Color, c);
}
```

Omit a case and the compiler emits a **compile-time error** naming the missing enumerator(s). ([GitHub][1])

### 10.3 Profiling to Steer Codegen

```c
@profile auto

fn hotpath(x:int) -> int => x * x + 3;

fn main() -> int {
    for (int i=0; i<1<<20; ++i) hotpath(i);
    return 0;
}
```

The compiler instruments, runs once, and rebuilds with `hotpath` marked as hot in the final exe. ([GitHub][1])

---

## 11) Conformance, Portability, and Interop

* **ABI/layout/widths:** identical to your host C toolchain (C11 default).
* **Headers:** use your system headers directly.
* **FFI:** calling between C and C-Script needs no shims.
* **Libraries:** `@link`/`@libpath` map to target-appropriate options (e.g., `-lX` vs `X.lib`). ([GitHub][1])

---

## 12) Error Model & Diagnostics (Hard-Line)

* Integer narrowing, signed/unsigned mixups, out-of-range enums, UB-adjacent shifts/divides: **errors**.
* Array bounds (when constexpr-resolvable): **errors** on out-of-bounds.
* Pointer provenance: mixing unrelated provenance: **error** unless in an explicit unsafe region (hard-line).
* Switches over `enum!`: missing cases → **compile-time error** with the missing set.
* Link failures are surfaced as **compile diagnostics** thanks to the inline linker. ([GitHub][1])

---

## 13) “C-Script ⇄ C” Mapping Cheat Sheet

* **Everything C** is valid in C-Script.
* **Sugars** (`fn`, `let`, `var`, `=>`, `enum!`) expand to plain C before IR.
* **Preludes** are macros, *not* runtime helpers.
* **Interop** is immediate—identical ABI, includes, and calling conventions. ([GitHub][1])

---

## 14) Extended Topics (From the Monolith Overview)

The Monolith README outlines additional forward-looking conveniences (e.g., slices/views, simple monomorphized templates) that still lower to plain C structs and functions, paying only for used instantiations. These patterns are described as zero-cost helpers and remain compatible with the overarching goals above. ([GitHub][1])

---

### Appendix A — Minimal Extended Grammar Block

This summarizes §8 in compact form:

```
translation-unit := { directive | external-decl }+ ;
directive        := '@' ident [tokens…] NEWLINE ;

softline-fn := 'fn' ident '(' param-list? ')' '->' type
               ( '=>' expr ';' | compound-stmt ) ;

declaration := ('let'|'var') type ident ('=' initializer)? ';'
             | c-declaration ;

enum-macro  := 'enum!' ident '{' enumerator-list '}' ;
```

All other expressions/statements/types/preprocessing are per C11.

---

### Appendix B — Canonical Examples of Directive Usage

```
@out "prog.exe"
@opt O3
@lto on
@abi "sysv"
@inc "/opt/include"
@libpath "/opt/lib"
@link "m"
@hardline on
@profile auto
```

These correspond one-to-one to the CLI’s override flags. ([GitHub][1])

---

## 15) Final Notes

C-Script is “directive-first C with dual posture, single-artifact build, and baked-in optimizations,” featuring real compile-time analysis for `enum!` switch coverage and a self-contained PGO loop to steer codegen. That’s the intended 1.0 experience per the Monolith README. ([GitHub][1])

---

### Sources

The above is synthesized directly from the project README and its “Monolith” sections (language model, grammar, directives, mapping, diagnostics, CLI, and examples). Where behavior is compiler/driver-specific (profiling rebuilds, inline linker), we followed the repo’s descriptions. ([GitHub][1])


⚠️ Common Setup Issues
Here are the most frequent hiccups when setting up C-Script:

🔧 LLVM/Clang Linking Problems
Missing LLVM dev packages: You need full LLVM + Clang + LLD development libraries installed.

Incorrect llvm-config path: If you have multiple LLVM versions, make sure you're using the one that matches your linked libraries.

Static vs dynamic linking: Some systems default to static builds, which can cause bloated binaries or linker errors.

🧱 Windows-Specific Quirks
MSVC regex quirks require patching or using Clang/MSYS2 instead.

Path separators (\\ vs /) can break temp file generation.

🧠 Compiler Flags
If you forget -DCS_EMBED_LLVM=1, it’ll try to use a system compiler instead of in-process LLVM.

Some flags like --capsule or --profile require matching directives in the .csc file.
