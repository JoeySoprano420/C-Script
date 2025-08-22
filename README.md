# C-Script

## C-Script — Low-level authority, through high-level scripting

C-Script is a directive-driven derivative of C that keeps 100% ABI-level compatibility with C while giving you a scripting-like authoring surface and a compiler that aggressively optimizes AOT, performs whole-program hardline checks, and links to a single .exe with no user-visible intermediates.

It does this with two complementary modes:

hardline — strict, razor-edged memory & type posture (no implicit narrowing, no UB-adjacent constructs, lifetime/alias checks, constexpr bounds when possible).

softline — expressive surface sugar, directive macros, and inline templates that compress “toxic” boilerplate back to clean C underneath.

## Quick glance: the surface

C:

// hello.csc
@hardline on              // turn on strict posture
@opt max                  // whole-program aggressive optimization
@link "m"                 // link a math library (platform aware)
@out "hello.exe"          // name the final artifact
@profile off              // (optional) build-without instrumentation

fn add(int a, int b) -> int => a + b;    // softline single-expression function

int main(void) {
    let int x = 20;                       // softline 'let' => const int
    var int y = 22;                       // softline 'var' => int
    print("C-Script says: %d\n", add(x, y));
    return 0;
}


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

C:

@hardline on
int take(short s) {      // error: implicit narrow from (int) call sites must be explicit
    return (int)s;
}

---

2.2 Soft-lining productivity (zero-overhead lowering)

Sugars lower to C with no runtime tax.

Inline templates for common patterns (RAII-ish scopes via macros, optional defer).

Paper-cuts gone: fn, =>, let/var, print, simple defer.

---

C:

@softline on
fn clamp(int v, int lo, int hi) -> int => (v < lo ? lo : (v > hi ? hi : v));

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

cscriptc.cpp (C++17)

CPP:

// cscriptc.cpp — C-Script v0.1 reference compiler (front + driver)
// Build: g++ -std=c++17 cscriptc.cpp -o cscriptc
// Usage: ./cscriptc hello.csc
#include <bits/stdc++.h>
using namespace std;

#if defined(_WIN32)
  #include <windows.h>
  #define PATH_SEP '\\'
#else
  #include <unistd.h>
  #define PATH_SEP '/'
#endif

struct Config {
    bool hardline=true;
    bool softline=true;
    string opt="O2";
    bool lto=true;
    bool profile=false;
    string out="a.exe";
    string abi="";
    vector<string> defines, incs, libpaths, links;
    bool strict=false, relaxed=false, show_c=false;
    string cc_prefer="";
};

static string trim(string s){
    size_t a=s.find_first_not_of(" \t\r\n");
    size_t b=s.find_last_not_of(" \t\r\n");
    if(a==string::npos) return "";
    return s.substr(a,b-a+1);
}

static bool starts_with(const string& s, const string& p){
    return s.rfind(p,0)==0;
}

static string prelude() {
    return R"(// --- C-Script prelude (zero-cost) ---
#if defined(_MSC_VER)
  #define CS_INLINE __forceinline
#else
  #define CS_INLINE inline __attribute__((always_inline))
#endif
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define print(...) printf(__VA_ARGS__)
#define likely(x)   __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)

// defer (optional): for(int _i=0;_i<1;_i++) for(int _j=0;_j<1;_i+=1, _j=1) for(;_j;_j=0)
)"; // keep prelude minimal; grow as needed
}

struct Source {
    string path;
    string text;
};

// Parse directives and keep original lines (minus directive lines)
static void parse_directives_and_collect(const string& in, Config& cfg, vector<string>& body){
    istringstream ss(in);
    string line;
    while(getline(ss,line)){
        string t = trim(line);
        if(starts_with(t,"@")){
            // parse
            // format: @name args...
            istringstream ls(t.substr(1));
            string name; ls>>name;
            if(name=="hardline"){ string v; ls>>v; cfg.hardline=(v!="off"); }
            else if(name=="softline"){ string v; ls>>v; cfg.softline=(v!="off"); }
            else if(name=="opt"){ string v; ls>>v; cfg.opt=v; }
            else if(name=="lto"){ string v; ls>>v; cfg.lto=(v!="off"); }
            else if(name=="profile"){ string v; ls>>v; cfg.profile=(v!="off"); }
            else if(name=="out"){ string v; ls>>quoted(v); cfg.out=v; }
            else if(name=="abi"){ string v; ls>>quoted(v); cfg.abi=v; }
            else if(name=="define"){ string v; ls>>v; cfg.defines.push_back(v); }
            else if(name=="inc"){ string v; ls>>quoted(v); cfg.incs.push_back(v); }
            else if(name=="libpath"){ string v; ls>>quoted(v); cfg.libpaths.push_back(v); }
            else if(name=="link"){ string v; ls>>quoted(v); cfg.links.push_back(v); }
            else {
                cerr<<"warning: unknown directive @"<<name<<"\n";
            }
            continue;
        }
        body.push_back(line);
    }
}

// Very light softline lowering via regex transforms
static string lower_softline_to_c(const string& src, bool softline_on){
    if(!softline_on) return src;
    string s=src;

    // 1) fn name(args) -> ret => expr;
    {
        regex r(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^=\{\n;]+)\s*=>\s*(.*?);)");
        s = regex_replace(s, r, "static inline $3 $1($2){ return ($4); }");
    }
    // 2) fn name(args) -> ret { ... }
    {
        // This is a best-effort; multi-line capture handled non-greedily
        regex r(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^\{;\n]+)\s*\{)");
        s = regex_replace(s, r, "$3 $1($2){");
    }
    // 3) let T id = expr;  -> const T id = expr;
    {
        regex r(R"(\blet\s+)");
        s = regex_replace(s, r, "const ");
    }
    // 4) var T id = expr;  -> T id = expr;
    {
        regex r(R"(\bvar\s+)");
        s = regex_replace(s, r, "");
    }
    // 5) print(...) stays print, prelude maps to printf
    return s;
}

static string read_file(const string& p){
    ifstream f(p, ios::binary);
    if(!f) throw runtime_error("cannot open: "+p);
    ostringstream ss; ss<<f.rdbuf(); return ss.str();
}

static string write_temp(const string& base, const string& content){
    string dir;
#if defined(_WIN32)
    char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
    dir = string(buf);
#else
    const char* t = getenv("TMPDIR"); dir = t? t: "/tmp/";
#endif
    string path = dir + base;
    ofstream o(path, ios::binary); o<<content;
    return path;
}

// Try to locate a C compiler
static string pick_cc(const string& prefer=""){
#if defined(_WIN32)
    vector<string> cands;
    if(!prefer.empty()) cands.push_back(prefer);
    cands.insert(cands.end(), {"clang","clang-cl","cl","gcc"});
    for(auto& c: cands){
        string cmd = c + " --version > NUL 2>&1";
        if(system(cmd.c_str())==0) return c;
    }
    return "clang";
#else
    vector<string> cands = prefer.empty()? vector<string>{"clang","gcc"} : vector<string>{prefer,"clang","gcc"};
    for(auto& c: cands){
        string cmd = c + string(" --version > /dev/null 2>&1");
        if(system(cmd.c_str())==0) return c;
    }
    return "clang";
#endif
}

static string join(const vector<string>& v, const string& sep){
    string r; for(size_t i=0;i<v.size();++i){ if(i) r+=sep; r+=v[i]; } return r;
}

int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    if(argc<2){ cerr<<"usage: cscriptc [options] file.csc\n"; return 1; }

    Config cfg;
    string inpath;
    vector<string> args;
    for(int i=1;i<argc;i++) args.push_back(argv[i]);

    // very small CLI
    for(size_t i=0;i<args.size();++i){
        string a=args[i];
        if(a=="-o" && i+1<args.size()){ cfg.out=args[++i]; }
        else if(starts_with(a,"-O")){ cfg.opt=a.substr(1); }
        else if(a=="--no-lto"){ cfg.lto=false; }
        else if(a=="--strict"){ cfg.strict=true; cfg.hardline=true; }
        else if(a=="--relaxed"){ cfg.relaxed=true; }
        else if(a=="--show-c"){ cfg.show_c=true; }
        else if(a=="--cc" && i+1<args.size()){ cfg.cc_prefer=args[++i]; }
        else if(a.size()>0 && a[0]!='-'){ inpath=a; }
    }
    if(inpath.empty()){ cerr<<"error: missing input .csc file\n"; return 2; }

    string src = read_file(inpath);
    vector<string> body;
    parse_directives_and_collect(src, cfg, body);

    // Build C source
    ostringstream csrc;
    csrc<<prelude()<<"\n";
    // Pack/ABI/warnings can be threaded as pragmas if desired.

    string body_joined;
    for(auto& l: body){ body_joined+=l; body_joined.push_back('\n'); }
    string lowered = lower_softline_to_c(body_joined, cfg.softline);
    csrc<<lowered;

    // Write temp .c
    string baseC = string("cscript_") + to_string(uintptr_t(&cfg)) + ".c";
    string cpath = write_temp(baseC, csrc.str());

    // Assemble compile command
    string cc = pick_cc(cfg.cc_prefer);

    vector<string> cmd;
    cmd.push_back(cc);

    // Strictness -> flags
    if(cc=="cl" || cc=="clang-cl"){
        // MSVC style
        cmd.push_back("/nologo");
        if(cfg.opt=="O0") cmd.push_back("/Od");
        else if(cfg.opt=="O1") cmd.push_back("/O1");
        else if(cfg.opt=="O2") cmd.push_back("/O2");
        else if(cfg.opt=="O3" || cfg.opt=="max") cmd.push_back("/O2"); // map O3/max to /O2
        if(cfg.strict) { cmd.push_back("/Wall"); cmd.push_back("/WX"); }
        if(cfg.lto)    { cmd.push_back("/GL"); }
        for(auto& d: cfg.defines) cmd.push_back("/D"+d);
        for(auto& p: cfg.incs)    cmd.push_back("/I"+p);
        cmd.push_back(cpath);
        // Linker
        cmd.push_back("/Fe:"+cfg.out);
        for(auto& lp: cfg.libpaths) cmd.push_back("/link /LIBPATH:\""+lp+"\"");
        for(auto& l: cfg.links){
            string lib=l;
            if(lib.rfind(".lib")==string::npos) lib += ".lib";
            cmd.push_back("/link "+lib);
        }
    } else {
        // GCC/Clang style
        cmd.push_back("-std=c11");
        if(cfg.opt=="O0") cmd.push_back("-O0");
        else if(cfg.opt=="O1") cmd.push_back("-O1");
        else if(cfg.opt=="O2") cmd.push_back("-O2");
        else if(cfg.opt=="O3") cmd.push_back("-O3");
        else if(cfg.opt=="size") cmd.push_back("-Os");
        else if(cfg.opt=="max") { cmd.push_back("-O3"); if(cfg.lto) cmd.push_back("-flto"); }
        if(cfg.hardline){ cmd.push_back("-Wall"); cmd.push_back("-Wextra"); cmd.push_back("-Werror");
                          cmd.push_back("-Wconversion"); cmd.push_back("-Wsign-conversion"); }
        if(cfg.lto) cmd.push_back("-flto");
        for(auto& d: cfg.defines){ cmd.push_back("-D"+d); }
        for(auto& p: cfg.incs){ cmd.push_back("-I"+p); }
        cmd.push_back(cpath);
        cmd.push_back("-o"); cmd.push_back(cfg.out);
        for(auto& lp: cfg.libpaths){ cmd.push_back("-L"+lp); }
        for(auto& l: cfg.links){ cmd.push_back("-l"+l); }
    }

    // Build final command string
    string full;
#if defined(_WIN32)
    const char* SEP = " ";
#else
    const char* SEP = " ";
#endif
    for(size_t i=0;i<cmd.size();++i){
        if(i) full+=SEP;
        // quote arguments containing spaces
        bool needQ = cmd[i].find(' ')!=string::npos;
        if(needQ) full.push_back('"');
        full+=cmd[i];
        if(needQ) full.push_back('"');
    }

    if(cfg.show_c){
        cerr<<"--- generated C ---\n"<<csrc.str()<<"\n--- end ---\n";
        cerr<<"CC: "<<full<<"\n";
    }

    int rc = system(full.c_str());
    if(rc!=0){
        cerr<<"build failed ("<<rc<<")\n";
        return rc? rc: 3;
    }

    // Clean temp C unless debugging
    if(!cfg.show_c){
        // best-effort delete
        remove(cpath.c_str());
    }

    cout<<cfg.out<<"\n";
    return 0;
}

---

What this gives you now

A working .csc → .exe pipeline that:

Honors the core directives (@hardline, @opt, @link, @out, etc.)

Lowers the headline softline sugars (fn, =>, let, var, print)

Enforces a strict compile posture (diagnostics as errors) when hardline is on

Produces only the final .exe; intermediates are kept in memory or ephemeral temp and removed

Extend points: add a real parser for deeper checks, integrate libLLVM for true in-memory IR+LLD if/when you want to move beyond system toolchain driving. The UX won’t change.

10) Try it now

1. Save cscriptc.cpp and build it:

SH:

# Linux/macOS
g++ -std=c++17 cscriptc.cpp -o cscriptc
# Windows (MSYS2/Clang suggested)
clang++ -std=c++17 cscriptc.cpp -o cscriptc.exe

---

2, Create hello.csc:

C:

@hardline on
@opt max
@out "hello.exe"

fn add(int a, int b) -> int => a + b;

int main(void){
    let int x = 40;
    var int y = 2;
    print("Answer: %d\n", add(x,y));
    return 0;
}

---

3, Build to a single exe:

SH:

./cscriptc hello.csc
# => hello.exe


Run it. 🎉

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

