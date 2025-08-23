C-Script: Unified Grammar, Semantics, and Syntax Reference (Monolithic Spec)

This reference consolidates the C-Script surface language and its lowering semantics as implemented by the reference compiler cscriptc.cpp and described in the project README. C-Script is a tiny, zero-overhead “scripting veneer” over C that compiles to ordinary C (or LLVM IR in the embedded mode) with zero runtime tax. Anything not recognized as a C-Script construct is passed through to the C toolchain unchanged.  ￼

⸻

1. Design overview
	•	Goal: keep C’s data model and ABIs while adding lightweight conveniences and compile-time analyzers (exhaustive enums, single-expression functions, pattern matching, @unsafe guard rails, two-pass PGO, etc.).  ￼
	•	Compilation model: “lowering then compile.” The C-Script front end scans a .csc source file, rewrites C-Script forms into pure C, optionally instruments for PGO, and then builds a single native executable (either by invoking a system C compiler/linker or fully in-process via LLVM+LLD).  ￼
	•	Zero-overhead promise: Added constructs become inline C, macros, or attributes; there is no runtime library beyond a tiny prelude header inlined into the generated C.  ￼

⸻

2. Lexical conventions

C-Script follows C11 tokens and whitespace/comments. Additional tokens/keywords appear only in the C-Script forms listed below; all other tokens are passed into the C backend unchanged.

2.1 Character set and encoding
	•	Source is treated as bytes; C compiler handles final character set semantics.
	•	Newlines separate logical lines; \r\n or \n accepted.

2.2 Comments and whitespace
	•	Same as C: /* … */ and // ….
	•	Whitespace is ignored except where it affects tokenization.

2.3 Identifiers
	•	Same as C: [A-Za-z_][A-Za-z0-9_]*.

2.4 Literals and operators
	•	Same as C; C-Script introduces no new literal forms or operators. New syntax is keyword-driven (fn, enum!, match, directives).

⸻

3. Top-level structure

A translation unit consists of any intermix of:
	1.	Directives (lines beginning with @),
	2.	C-Script declarations/expressions (enum!, fn, match, @unsafe blocks), and
	3.	Raw C (headers, types, functions, macros, etc.), passed through.

Formally (EBNF):

translation_unit  ::= { directive | cs_item | c_passthrough } ;

directive         ::= '@' ident { directive_arg } newline ;
directive_arg     ::= ident | string_literal | number ;

cs_item           ::= enum_bang
                    | softline_fn
                    | softline_fn_block
                    | unsafe_block
                    | match_stmt
                    | switch_exhaustive_macro  // appears in passthrough C too
                    ;

c_passthrough     ::= any_tokens_not_captured_by_cs ;


⸻

4. Directives (@…) — compiler configuration in source

Directives configure the build, optimizer, instrumentation, and UI. They may appear anywhere; the reference compiler scans the entire file and applies them before lowering.

Recognized directive names (values are case-sensitive unless noted):

@hardline   on|off       // default on : enables hardline checks (Werror etc.)
@softline   on|off       // default on : enables lowering of 'fn' sugar
@opt        O0|O1|O2|O3|max|size
@lto        on|off       // link-time optimization
@profile    on|off       // 2-pass PGO: instrument then rebuild hot
@out        "path"       // final executable path
@abi        "string"     // ABI tag (passed through; toolchain-specific usage)
@define     NAME[=VALUE] // add -DNAME[=VALUE]
@inc        "path"       // add -Ipath
@libpath    "path"       // add -Lpath (or LIBPATH on MSVC)
@link       "lib"        // add -llib / lib.lib
@guardian   on|off       // confirmation overlays before risky actions
@anim       on|off       // animated CLI spinner during compile/link
@muttrack   on|off       // define CS_TRACK_MUTATIONS for mutation counters

Semantics:
	•	Unknown directives are warned and ignored (non-fatal).
	•	In addition to in-source directives, CLI flags provide equivalents; in-source settings apply to the file being compiled.  ￼

⸻

5. Prelude (zero-cost runtime)

Before lowered code, the compiler prepends a tiny prelude (C macros and helpers):
	•	print(...) alias for printf.
	•	likely(x), unlikely(x) mapped to compiler intrinsics where available.
	•	CS_DEFER(body) single-use defer macro (for RAII-ish cleanup).
	•	CS_UNSAFE_BEGIN/END warning-relaxing pragmas around @unsafe blocks.
	•	CS_SWITCH_EXHAUSTIVE/CS_CASE/CS_SWITCH_END helpers for enum switches.
	•	CS_HOT attribute for hot functions on the PGO second pass.
	•	Optional mutation counters if CS_TRACK_MUTATIONS is defined.  ￼

The prelude is headerless and inlined into the generated C source so no extra include is required.

⸻

6. Enum declarations with exhaustiveness: enum!

6.1 Grammar

enum_bang     ::= 'enum!' ident '{' enumerator_list '}' ;
enumerator_list
               ::= enumerator { ',' enumerator } [ ',' ] ;
enumerator     ::= ident [ '=' constant_expression ] ;

6.2 Semantics / Lowering
	•	Generates a C typedef enum Name { … } Name;.
	•	Also emits two helpers:

static inline int  cs__enum_is_valid_Name(int v) { switch((Name)v){ case A: /*…*/ default: return 0; } }
static inline void cs__enum_assert_Name(int v) { #if CS_HARDLINE … #endif }


	•	The exhaustiveness analyzer (see §8) scans uses of CS_SWITCH_EXHAUSTIVE(Name, expr) … CS_SWITCH_END(Name, expr) and fails the build if any enum! Name member is not covered by a CS_CASE(Member) in that switch. Missing cases are printed with file/line. (This check only runs for enum! enums; ordinary C enums are not checked.)  ￼

⸻

7. Functions (softline sugar): fn

Two forms are accepted and lowered to static inline C functions (with optional hot attributes / instrumentation in PGO mode):

7.1 Single-expression functions

softline_fn      ::= 'fn' ident '(' param_list? ')' '->' type '=>' expression ';' ;
param_list       ::= param { ',' param } ;
param            ::= c_decl_tokens ;   // forwarded to C as-is
type             ::= c_decl_tokens ;   // forwarded to C as-is

Lowering:

static inline [CS_HOT] RetType Name(Params) { [if PGO pass1: cs_prof_hit("Name");] return (Expr); }

7.2 Block-body functions

softline_fn_block ::= 'fn' ident '(' param_list? ')' '->' type '{' block_body '}' ;
block_body        ::= c_tokens_until_matching_brace ;

Lowering:

[CS_HOT] RetType Name(Params) { [if PGO pass1: cs_prof_hit("Name");] /* body */ }

Notes:
	•	Parameter/return types and declarations are pure C; the front-end does not parse C types beyond token boundaries—it preserves them verbatim.
	•	fn is sugar only; linkage is static inline (internal to translation unit) by design.  ￼

⸻

8. Exhaustive switches over enum! types

8.1 Macros (to be written inside C/C-Script code)

CS_SWITCH_EXHAUSTIVE(T, expr) {
    CS_CASE(MemberA): /* … */ break;
    CS_CASE(MemberB): /* … */ break;
    // ...
}
CS_SWITCH_END(T, expr)

8.2 Semantics
	•	At compile time, the front-end statically cross-checks each macro region against the members of enum! T.
	•	If any member is not matched by a CS_CASE, the build fails with a “non-exhaustive” diagnostic.
	•	At runtime (hardline mode), a default guard calls cs__enum_assert_T to abort on impossible values.  ￼

⸻

9. Unsafe regions: @unsafe { … }

9.1 Grammar

unsafe_block ::= '@unsafe' '{' block_body '}' ;

9.2 Semantics / Lowering
	•	Lowered to { CS_UNSAFE_BEGIN; … CS_UNSAFE_END; }, which temporarily relaxes selected compiler warnings (e.g., signed/unsigned conversions) under both MSVC and GCC/Clang pragmas.
	•	Intended for small, well-reviewed code; guarded by Guardian (optional prompt) when building/running.  ￼

⸻

10. Pattern matching (with destructuring): match (…) { … }

Lightweight sugar that lowers to an if/else ladder. Intended for simple scalar matches or tuple destructuring with fields ._0, ._1. (This is a conservative textual lowering—keep case bodies reasonably simple for predictable transformation.)  ￼

10.1 Grammar

match_stmt     ::= 'match' '(' expression ')' '{' case_list '}' ;
case_list      ::= { case_entry } ;
case_entry     ::= pattern '=>' case_body ';' ;
case_body      ::= statement_or_expr ;  // may be a braced single-line block

pattern        ::= '_'
                 | 'default'
                 | scalar_alternatives
                 | tuple_destructure ;

scalar_alternatives ::= scalar_pat { '|' scalar_pat } ;
scalar_pat     ::= ident | number | char_literal | qualified_enum_member ;

tuple_destructure    ::= '(' ident [ ',' ident ] ')' ;

10.2 Semantics / Lowering
	•	The subject expression is captured once into a fresh local __cs_subj.
	•	Scalar alternatives lower to if (__cs_subj==(Alt1) || __cs_subj==(Alt2) || …).
	•	Tuple destructuring assumes the subject has fields ._0, ._1, binding fresh auto locals to them before executing the case body.
	•	A _ or default pattern becomes the trailing else arm; if omitted, a no-op else {} is inserted to keep lowering simple.

⸻

11. Variable sugar: let and var
	•	let → replaced with const (pure textual).
	•	var → removed (becomes a plain C declaration with default mutability).

Grammar:

c_declaration_with_let ::= 'let'  c_decl_rest ';' ;  // becomes 'const' c_decl_rest ';'
c_declaration_with_var ::= 'var'  c_decl_rest ';' ;  // becomes        c_decl_rest ';'

Because this is textual, write declarations in forms that remain valid after the substitution (e.g., let int x = 1;).  ￼

⸻

12. Mutation tracking (optional)

If @muttrack on (or -DCS_TRACK_MUTATIONS), the prelude defines:

static volatile unsigned long long cs__mutations;
#define CS_MUT_NOTE()        do { cs__mutations++; } while(0)
#define CS_MUT_STORE(dst,val) do { (dst)=(val); cs__mutations++; } while(0)
#define CS_MUT_MEMCPY(d,s,n)  do { memcpy((d),(s),(n)); cs__mutations++; } while(0)
static unsigned long long cs_mutation_count(void){ return cs__mutations; }

Use these to annotate important state changes; in the “off” configuration they collapse to ordinary stores/memcpy.  ￼

⸻

13. Guardian confirmation overlays (optional)

If @guardian on (default), the driver may ask for confirmation before risky actions:
	•	Running the instrumented build during PGO pass 1.
	•	Writing to a protected output path (e.g., system bin directories).

Set env CS_GUARDIAN_AUTOYES=1 in CI to bypass prompts.  ￼

⸻

14. Animated CLI glyphs (optional)

If @anim on (default), the driver shows a spinner during compile and link phases (both external toolchain and embedded LLD path). This is purely cosmetic; it has no effect on timing or artifacts.  ￼

⸻

15. Profile-Guided Optimization (PGO), 2-pass
	1.	Pass 1 (instrumented):
	•	All softline fn bodies call cs_prof_hit("fnName") at entry.
	•	A tiny counter table accumulates per-function hit counts.
	•	On exit, counts are flushed to a profile file specified by CS_PROFILE_OUT env.  ￼
	2.	Selection: The driver reads the counts and marks the top-N functions as “hot”.
	3.	Pass 2 (final): Rebuild with CS_HOT attributes applied to those functions; instrumentation removed.

The entire flow yields one final executable, and all temporary files are deleted.  ￼

⸻

16. Embedded toolchain (optional): LLVM + LLD + IR-pass PGO

Build can run entirely in-process:
	•	Define CS_EMBED_LLVM to compile generated C with Clang in-proc to an object buffer and LLD to a final executable (COFF/ELF/Mach-O).
	•	Define both CS_EMBED_LLVM + CS_PGO_EMBED to perform an IR-level profiling pass that injects calls to cs_prof_hit at function entries before codegen, then optimize and link with LLD.  ￼

These flags change the build path, not the language. The surface grammar/semantics are identical.

⸻

17. Command-line interface (CLI)

cscriptc file.csc [--show-c] [--strict] [--relaxed]
                  [-O{0|1|2|3|max|size}] [--no-lto]
                  [--cc <compiler>] [-o out.exe]

	•	--show-c   : print the generated C to stderr (keeps temp file).
	•	--strict   : hardline mode + treat warnings as errors (MSVC /Wall /WX, GCC/Clang -Wall -Wextra -Werror plus conversion warnings).
	•	--relaxed  : opposite of --strict (fewer diagnostics).
	•	-O… / --no-lto / -o / --cc : optimizer/LTO/output/compiler picker.
	•	In-source @… directives can mirror/override many of these.  ￼

⸻

18. Grammar summary (EBNF)

Below is a compact grammar capturing the C-Script surface constructs (everything else is normal C tokens):

translation_unit  ::= { directive | cs_item | c_passthrough } ;

directive         ::= '@' ident { directive_arg } newline ;
directive_arg     ::= ident | string_literal | integer_literal ;

cs_item           ::= enum_bang
                    | softline_fn
                    | softline_fn_block
                    | unsafe_block
                    | match_stmt ;

enum_bang         ::= 'enum!' ident '{' enumerator_list '}' ;
enumerator_list   ::= enumerator { ',' enumerator } [ ',' ] ;
enumerator        ::= ident [ '=' constant_expression ] ;

softline_fn       ::= 'fn' ident '(' param_list? ')' '->' type '=>' expression ';' ;
softline_fn_block ::= 'fn' ident '(' param_list? ')' '->' type '{' block_body '}' ;

param_list        ::= param { ',' param } ;
param             ::= c_decl_tokens ;
type              ::= c_decl_tokens ;
block_body        ::= c_tokens_until_matching_brace ;

unsafe_block      ::= '@unsafe' '{' block_body '}' ;

match_stmt        ::= 'match' '(' expression ')' '{' case_list '}' ;
case_list         ::= { case_entry } ;
case_entry        ::= pattern '=>' case_body ';' ;

pattern           ::= '_'
                    | 'default'
                    | scalar_alternatives
                    | tuple_destructure ;

scalar_alternatives ::= scalar_pat { '|' scalar_pat } ;
scalar_pat        ::= ident | number | char_literal | qualified_enum_member ;
tuple_destructure ::= '(' ident [ ',' ident ] ')' ;

c_passthrough     ::= any_tokens_not_captured_by_cs ;

Notes
	•	c_decl_tokens, block_body, c_tokens_* are “verbatim forward” regions: the front-end does not parse C—those tokens are copied into the generated C unchanged.
	•	The grammar intentionally avoids constraining C expressions/types; your C compiler remains the authority.

⸻

19. Semantic rules & lowering table

Construct	Well-formedness	Lowering (conceptual)
enum! T { … }	At least one enumerator; names follow C rules.	Emits typedef enum T { … } T; + cs__enum_is_valid_T + cs__enum_assert_T.
CS_SWITCH_EXHAUSTIVE(T, e) … CS_SWITCH_END(T, e)	T must be an enum!. All CS_CASE members must be subset of T.	Pure C switch, plus static analyzer that fails if any member missing.
fn name(args) -> R => expr;	args and R must be valid C declarations; expr must be a valid C expression for R.	static inline [CS_HOT] R name(args) { [cs_prof_hit] return (expr); }
fn name(args) -> R { … }	body must be valid C.	R name(args) { [cs_prof_hit] … } with optional CS_HOT.
@unsafe { … }	braces balanced.	{ CS_UNSAFE_BEGIN; … CS_UNSAFE_END; }
match (e) { p => s; … }	each case must end with ;.	If/else chain over a cached __cs_subj. Tuple cases bind auto x = __cs_subj._0; etc.
let T x = v;	textual const must still form valid C.	const T x = v;
var T x = v;	textual removal must still form valid C.	T x = v;
Mutation tracking	—	Macros increment cs__mutations counter if enabled.


⸻

20. Diagnostics & hardline mode
	•	Hardline (@hardline on / --strict): Elevates warnings to errors and enables runtime checks for impossible enum values (via cs__enum_assert_T).
	•	Analyzer errors: Missing cases in exhaustive switches, malformed fn/match/enum!, unknown directive names (warn).
	•	Show generated C: --show-c dumps the exact C fed to the backend for debugging.

⸻

21. Toolchain & build products
	•	External path: Choose CC (clang preferred; gcc, cl, clang-cl also supported); assemble C flags from directives/CLI; run the linker; produce one final executable; delete temps.
	•	Embedded path: With CS_EMBED_LLVM, compile C to object in memory and link with LLD in-proc for ELF/COFF/Mach-O; optional IR-pass profiling with CS_PGO_EMBED.  ￼

⸻

22. Minimal examples

22.1 enum! + exhaustive switch

enum! Color { Red, Green=3, Blue };

int main(void) {
  Color c = Red;
  CS_SWITCH_EXHAUSTIVE(Color, c) {
    CS_CASE(Red):   print("R\n"); break;
    CS_CASE(Green): print("G\n"); break;
    CS_CASE(Blue):  print("B\n"); break;
  } CS_SWITCH_END(Color, c);
}

22.2 Softline functions and matching

fn add(int a, int b) -> int => a + b;

struct Pair { int _0, _1; };

int main(void) {
  struct Pair p = {1, 2};
  match (p) {
    (x, y)   => print("%d\n", x + y);
    default  => print("nope\n");
  };
}

22.3 Unsafe island with mutation notes

@unsafe {
  var int x = 0;
  CS_MUT_STORE(x, 42);
}
print("%llu\n", cs_mutation_count());


⸻

23. Portability and limits
	•	The front-end uses regex-based scanning for C-Script forms; write them in canonical shapes (e.g., keep => function bodies to one expression, terminate match arms with ;).
	•	Pattern matching is intentionally conservative and may not understand deeply nested blocks—use a block arm { … } that fits on one logical line or write explicit if/switch in C when needed.
	•	Exhaustiveness checks only apply to enum! declarations known in the same compilation unit.

⸻

24. Versioning
	•	This spec tracks the reference compiler and README in the repository; consult the repo for the authoritative list of flags and their current defaults.  ￼

⸻

Sources
	•	Project repository overview, feature list, and build notes (authoritative for the constructs and build flow summarized here).  ￼

⸻

Here’s the canonical Hello World in C-Script using its softline fn sugar:

// hello.csc

// optional directive for output file
@out "hello.exe"

// define a softline function
fn main() -> int {
    print("Hello, world!\n");
    return 0;
}

Notes:
	•	fn main() -> int { … } is lowered to a normal C int main(void) with body intact.
	•	print(...) is defined in the C-Script prelude as an alias for printf.
	•	The @out directive tells the compiler to name the output executable hello.exe (optional; without it you’d get a.exe by default).

⸻

✅ To build:

cscriptc hello.csc

✅ To run:

./hello.exe


⸻

Here’s what your hello.csc lowers to in plain C.

Semantically equivalent minimal C (what the final exe effectively is)

// --- generated from hello.csc (prelude elided for brevity) ---
#include <stdio.h>

int main(void) {
    printf("Hello, world!\n");
    return 0;
}

“Faithful” lowering with the tiny bit of prelude it uses

The compiler always prepends a small prelude; for this example only the print alias matters:

// --- generated C (abridged prelude + lowered body) ---
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// prelude alias used by the source
#define print(...) printf(__VA_ARGS__)

// softline header lowering:  fn main() -> int { ... }
int main(void) {
    print("Hello, world!\n");
    return 0;
}

That’s it—the fn … -> … {} header is rewritten to a normal C signature, and print is just printf. Instrumentation/PGO and other prelude helpers are not emitted in the final optimized pass unless you enable them (e.g., @profile on).
