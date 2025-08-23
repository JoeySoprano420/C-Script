// cscriptc.cpp — C-Script v0.4 reference compiler (front + analyzer + PGO + driver)
// Build: g++ -std=gnu++17 cscriptc.cpp -o cscriptc      (Linux/macOS, Clang/GCC)
//     or clang++ -std=c++17 cscriptc.cpp -o cscriptc
//     or (MSYS/Clang on Windows recommended). MSVC works but needs regex fixes below.
// Usage:  ./cscriptc file.csc [--show-c] [--strict] [-O{0|1|2|3|max|size}] [-o out.exe]
// Notes:
//  - Produces ONE final .exe. Any temps for build/PGO are deleted.
//  - PGO: when @profile on, we instrument softline `fn` entries, run once, then rebuild with hot attrs.
//  - Exhaustiveness: compile-time error if a CS_SWITCH_EXHAUSTIVE for an enum! misses any cases.
//  - NEW in v0.4: directives @sanitize, @debug, @time, @import, plus @checked blocks and timing.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <iomanip>   // std::quoted
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

using std::string;
using std::vector;
using std::set;
using std::map;
using std::pair;

// ================= Timing Utility =================
using Clock = std::chrono::high_resolution_clock;
static void log_time(const char* tag, Clock::time_point t0, bool on) {
    if (!on) return;
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::cerr << "[time] " << tag << ": " << dt << " ms\n";
}

// ================= Diagnostics ====================
static void warn(const string& msg) {
#if defined(_WIN32)
    std::cerr << "[warn] " << msg << "\n";
#else
    std::cerr << "\033[33m[warn]\033[0m " << msg << "\n";
#endif
}
static void error(const string& msg) {
#if defined(_WIN32)
    std::cerr << "[error] " << msg << "\n";
#else
    std::cerr << "\033[31m[error]\033[0m " << msg << "\n";
#endif
}

// ============================================================================
// Regex wrapper for MSVC iterator + match_results quirks (portable).
// ============================================================================
namespace cs_regex_wrap {
    using cmatch = std::match_results<std::string::const_iterator>;

    inline bool search_iter(std::string::const_iterator begin,
        std::string::const_iterator end,
        cmatch& m,
        const std::regex& re)
    {
        return std::regex_search(begin, end, m, re);
    }

    inline bool search_from(const std::string& s,
        size_t& pos,
        cmatch& m,
        const std::regex& re)
    {
        if (pos > s.size()) return false;
        auto b = s.begin() + static_cast<std::ptrdiff_t>(pos);
        auto e = s.end();
        if (!std::regex_search(b, e, m, re)) return false;
        pos = static_cast<size_t>(m.suffix().first - s.begin());
        return true;
    }

    inline void append_prefix(std::string& out,
        const std::string& src,
        size_t start,
        const cmatch& m)
    {
        const size_t endAbs = static_cast<size_t>(m.prefix().second - src.begin());
        if (endAbs >= start && endAbs <= src.size())
            out.append(src, start, endAbs - start);
    }
}

//============================= Config =============================
struct Config {
    bool hardline = true;
    bool softline = true;
    string opt = "O2";
    bool lto = true;
    bool profile = false; // PGO two-pass
    string out = "a.exe";
    string abi = "";
    vector<string> defines, incs, libpaths, links;
    bool strict = false, relaxed = false, show_c = false;
    string cc_prefer = "";

    // NEW:
    bool ui_anim = true;           // animated CLI glyphs during compilation
    bool guardian = true;          // guardian confirmation overlays
    bool track_mutations = false;  // enable mutation tracking in generated C

    // NEW: no-dangling-pointer mode (enables ASan and helper macros)
    bool no_dangling = false;
};

static bool starts_with(const string& s, const string& p) { return s.rfind(p, 0) == 0; }
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string read_file(const string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + p);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static string write_temp(const string& base, const string& content) {
    string dir;
#if defined(_WIN32)
    char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
    dir = string(buf);
#else
    const char* t = getenv("TMPDIR"); dir = t ? t : "/tmp/";
#endif
    string path = dir + base;
    std::ofstream o(path, std::ios::binary); o << content;
    return path;
}
static void rm_file(const string& p) { std::remove(p.c_str()); }

// For error lines
static pair<int, int> line_col_at(const string& s, size_t pos) {
    int line = 1, col = 1;
    for (size_t i = 0; i < pos && i < s.size(); ++i) {
        if (s[i] == '\n') { line++; col = 1; }
        else col++;
    }
    return { line,col };
}

//============================= Prelude =============================
static string prelude(bool hardline) {
    std::ostringstream o;
    o << R"(// --- C-Script prelude (zero-cost) ---
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define print(...) printf(__VA_ARGS__)
#if defined(__GNUC__) || defined(__clang__)
  #define likely(x)   __builtin_expect(!!(x),1)
  #define unlikely(x) __builtin_expect(!!(x),0)
#else
  #define likely(x)   (x)
  #define unlikely(x) (x)
#endif

// ---- Tiny 'defer' macro ----
#define CS_CONCAT2(a,b) a##b
#define CS_CONCAT(a,b)  CS_CONCAT2(a,b)
#define CS_DEFER(body) for (int CS_CONCAT(_cs_defer_, __COUNTER__) = 0; \
                             CS_CONCAT(_cs_defer_, __COUNTER__) == 0; \
                             (void)(body), CS_CONCAT(_cs_defer_, __COUNTER__)=1)

// ---- Exhaustive switch helpers (enum-specific assert is emitted by compiler) ----
#define CS_SWITCH_EXHAUSTIVE(T, expr) do { int __cs_hit=0; T __cs_v=(expr); switch(__cs_v){
#define CS_CASE(x) case x: __cs_hit=1
#define CS_SWITCH_END(T, expr) default: break; } if(!__cs_hit) cs__enum_assert_##T(__cs_v); } while(0)

// ---- @unsafe pragmas ----
#if defined(_MSC_VER)
  #define CS_PRAGMA_PUSH __pragma(warning(push))
  #define CS_PRAGMA_POP  __pragma(warning(pop))
  #define CS_PRAGMA_RELAX __pragma(warning(disable:4244 4267 4018 4389))
#else
  #define CS_PRAGMA_PUSH _Pragma("GCC diagnostic push")
  #define CS_PRAGMA_POP  _Pragma("GCC diagnostic pop")
  #define CS_PRAGMA_RELAX _Pragma("GCC diagnostic ignored \"-Wconversion\"") \
                          _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"") \
                          _Pragma("GCC diagnostic ignored \"-Wenum-conversion\"")
#endif
#define CS_UNSAFE_BEGIN do { CS_PRAGMA_PUSH; CS_PRAGMA_RELAX; } while(0)
#define CS_UNSAFE_END   do { CS_PRAGMA_POP; } while(0)

// ---- CS_HOT for 2nd-pass PGO ----
#if defined(_MSC_VER)
  #define CS_HOT
#else
  #define CS_HOT __attribute__((hot))
#endif
)";

    if (hardline) o << "#define CS_HARDLINE 1\n";

    // Profiler (only compiled into the instrumented pass)
    o << R"(
#ifdef CS_PROFILE_BUILD
typedef struct { const char* name; unsigned long long count; } _cs_prof_ent;
static _cs_prof_ent* _cs_prof_tbl = 0;
static size_t _cs_prof_cap = 0, _cs_prof_len = 0;
static FILE* _cs_prof_f = NULL;

static void _cs_prof_flush(void){
    if(!_cs_prof_f){
        const char* path = getenv("CS_PROFILE_OUT");
        if(!path) return;
        _cs_prof_f = fopen(path, "wb");
        if(!_cs_prof_f) return;
    }
    for(size_t i=0;i<_cs_prof_len;i++){
        if(_cs_prof_tbl[i].name){
            fprintf(_cs_prof_f, "%s %llu\n", _cs_prof_tbl[i].name, (unsigned long long)_cs_prof_tbl[i].count);
        }
    }
    fclose(_cs_prof_f); _cs_prof_f=NULL;
}

static void _cs_prof_init(void){
    atexit(_cs_prof_flush);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void _cs_prof_ctor(void){ _cs_prof_init(); }

static void cs_prof_hit(const char* name){
    // linear probe (tiny)
    for(size_t i=0;i<_cs_prof_len;i++){
        if(_cs_prof_tbl[i].name && strcmp(_cs_prof_tbl[i].name,name)==0){ _cs_prof_tbl[i].count++; return; }
    }
    if(_cs_prof_len==_cs_prof_cap){
        size_t ncap = _cs_prof_cap? _cs_prof_cap*2 : 32;
        _cs_prof_tbl = (_cs_prof_ent*)realloc(_cs_prof_tbl, ncap*sizeof(_cs_prof_ent));
        for(size_t i=_cs_prof_cap;i<ncap;i++){ _cs_prof_tbl[i].name=NULL; _cs_prof_tbl[i].count=0; }
        _cs_prof_cap = ncap;
    }
    _cs_prof_tbl[_cs_prof_len].name = name;
    _cs_prof_tbl[_cs_prof_len].count = 1;
    _cs_prof_len++;
}
#else
static void cs_prof_hit(const char*){ /* no-op in optimized pass */ }
#endif
)";

    // --- NEW: Mutation tracking hooks (no-op unless CS_TRACK_MUTATIONS is defined)
    o << R"(
#ifdef CS_TRACK_MUTATIONS
static volatile unsigned long long cs__mutations = 0ULL;
#define CS_MUT_NOTE()   do { cs__mutations++; } while(0)
#define CS_MUT_STORE(dst, val) do { (dst) = (val); cs__mutations++; } while(0)
#define CS_MUT_MEMCPY(d,s,n)  do { memcpy((d),(s),(n)); cs__mutations++; } while(0)
static unsigned long long cs_mutation_count(void){ return cs__mutations; }
#else
#define CS_MUT_NOTE()        do{}while(0)
#define CS_MUT_STORE(dst,val) ((dst)=(val))
#define CS_MUT_MEMCPY(d,s,n)  memcpy((d),(s),(n))
static unsigned long long cs_mutation_count(void){ return 0ULL; }
#endif
)";

    return o.str();
}

//============================= enum! parsing + emission =============================
struct EnumInfo { set<string> members; };

static string lower_enum_bang_and_collect(const string& in, map<string, EnumInfo>& enums) {
    using namespace cs_regex_wrap;

    string s = in;
    std::regex re(R"(enum!\s+([A-Za-z_]\w*)\s*\{([^}]*)\})");
    cmatch m;

    string out; out.reserve(s.size() * 12 / 10);
    size_t pos = 0;

    auto split_enums = [](string body)->vector<string> {
        vector<string> names;
        string token;
        auto flush = [&]() {
            string t = trim(token);
            if (!t.empty()) {
                size_t eq = t.find('=');
                string ident = trim(eq == string::npos ? t : t.substr(0, eq));
                if (!ident.empty()) names.push_back(ident);
            }
            token.clear();
            };
        for (char c : body) {
            if (c == ',') flush();
            else token.push_back(c);
        }
        flush();
        return names;
        };

    while (search_from(s, pos, m, re)) {
        append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m); // safe: we append up to match prefix

        string name = m[1].str();
        string body = m[2].str();
        vector<string> items = split_enums(body);
        EnumInfo info;
        for (auto& id : items) info.members.insert(id);
        enums[name] = info;

        // Emit real C typedef enum + validators
        out += "typedef enum " + name + " { " + body + " } " + name + ";\n";
        out += "static inline int cs__enum_is_valid_" + name + "(int v){ switch((" + name + ")v){ ";
        for (auto& e : info.members) out += "case " + e + ": ";
        out += "return 1; default: return 0; } }\n";
        out += "static inline void cs__enum_assert_" + name + "(int v){\n"
            "#if defined(CS_HARDLINE)\n"
            "  if(!cs__enum_is_valid_" + name + "(v)){\n"
            "    fprintf(stderr,\"[C-Script hardline] Non-exhaustive switch for enum " + name + " (value %d)\\n\", v);\n"
            "    abort();\n"
            "  }\n"
            "#else\n"
            "  (void)v;\n"
            "#endif\n"
            "}\n";
        // pos has already advanced by search_from to suffix start
    }
    // Append tail
    out.append(s, pos, string::npos);
    return out;
}

//============================= Compile-time switch exhaustiveness =============================
struct SwitchSite { string type; set<string> cases; size_t startPos = 0; };

static void check_exhaustiveness_or_die(const string& src,
    const map<string, EnumInfo>& enums) {
    using namespace cs_regex_wrap;

    size_t i = 0, n = src.size();
    while (true) {
        size_t a = src.find("CS_SWITCH_EXHAUSTIVE(", i);
        if (a == string::npos) break;
        size_t tstart = a + strlen("CS_SWITCH_EXHAUSTIVE(");
        // parse Type until ',' (skip spaces)
        size_t p = tstart;
        while (p < n && isspace((unsigned char)src[p])) ++p;
        size_t q = p;
        while (q < n && (isalnum((unsigned char)src[q]) || src[q] == '_')) ++q;
        string Type = src.substr(p, q - p);
        if (Type.empty()) { i = a + 1; continue; }

        // Find the matching CS_SWITCH_END(Type
        string endKey = string("CS_SWITCH_END(") + Type;
        size_t b = src.find(endKey, q);
        if (b == string::npos) {
            auto lc = line_col_at(src, a);
            std::cerr << "error: unmatched CS_SWITCH_EXHAUSTIVE for '" << Type
                << "' at " << lc.first << ":" << lc.second << "\n";
            exit(1);
        }
        // Extract region between a..b for cases
        string region = src.substr(a, b - a);

        // Gather CS_CASE(IDENT)
        std::regex caseRe(R"(CS_CASE\s*\(\s*([A-Za-z_]\w*)\s*\))");
        cmatch m; set<string> seen;
        size_t pos2 = 0;
        while (search_from(region, pos2, m, caseRe)) {
            seen.insert(m[1].str());
        }

        // Compare with enum set if exists
        auto itE = enums.find(Type);
        if (itE != enums.end()) {
            const auto& universe = itE->second.members;
            vector<string> missing;
            for (const auto& e : universe) if (!seen.count(e)) missing.push_back(e);
            if (!missing.empty()) {
                auto lc = line_col_at(src, a);
                std::cerr << "error: non-exhaustive switch for enum '" << Type
                    << "' at " << lc.first << ":" << lc.second << ". Missing:";
                for (auto& mname : missing) std::cerr << " " << mname;
                std::cerr << "\n";
                exit(1);
            }
        }
        i = b + 1;
    }
}

//============================= @unsafe blocks =============================
static string lower_unsafe_blocks(const string& in) {
    string s = in, out; out.reserve(s.size() * 11 / 10);
    size_t i = 0, n = s.size();
    auto skip_ws = [&](size_t j) { while (j < n && isspace((unsigned char)s[j])) ++j; return j; };

    while (i < n) {
        if (s[i] == '@' && s.compare(i, 7, "@unsafe") == 0) {
            size_t j = i + 7; j = skip_ws(j);
            if (j < n && s[j] == '{') {
                out += "{ CS_UNSAFE_BEGIN; ";
                int depth = 0; size_t k = j; depth++; k++;
                while (k < n && depth>0) {
                    char c = s[k];
                    if (c == '{') depth++;
                    else if (c == '}') { depth--; if (depth == 0) { out += " CS_UNSAFE_END; "; } }
                    out.push_back(c); ++k;
                }
                i = k; continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

// ============================= Pattern Matching Lowering =====================
// NOTE: This is a conservative regex-based lowering supporting two forms:
//  1) Scalar match with single-line bodies ending in ';'
//  2) Tuple destructuring into fresh locals for fields ._0 and ._1
// It is intentionally simple; for complex bodies keep them on one line or
// expand later into a block-savvy parser.

static std::string lower_match_patterns(const std::string& src) {
    using namespace cs_regex_wrap;

    std::string s = src, out;
    out.reserve(s.size());

    // FIX: no std::regex::dotall in standard; use [\s\S]*? to span newlines under ECMAScript
    std::regex matchBlock(R"(match\s*\(\s*([\s\S]*?)\s*\)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
    cmatch M;
    size_t pos = 0;

    auto trim2 = [](std::string t)->std::string {
        size_t a = t.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
        size_t b = t.find_last_not_of(" \t\r\n"); return t.substr(a, b - a + 1);
    };

    while (search_from(s, pos, M, matchBlock)) {
        append_prefix(out, s, pos - (M.length(0) ? M.length(0) : 0), M);

        std::string subject = trim2(M[1].str());
        std::string bodyAll = M[2].str();

        std::vector<std::pair<std::string, std::string>> cases;
        {
            std::istringstream ss(bodyAll);
            std::string line;
            while (std::getline(ss, line, ';')) {
                std::string t = trim2(line);
                if (t.empty()) continue;
                size_t arrow = t.find("=>");
                if (arrow == std::string::npos) continue;
                std::string pat = trim2(t.substr(0, arrow));
                std::string code = trim2(t.substr(arrow + 2));
                if (!code.empty() && code.front() == '{' && code.back() == '}')
                    code = trim2(code.substr(1, code.size() - 2));
                cases.push_back({ pat, code });
            }
        }

        std::ostringstream L;
        L << "{ auto __cs_subj = (" << subject << "); ";
        bool first = true, hasDefault = false;

        for (auto& kv : cases) {
            const std::string& pat = kv.first;
            const std::string& code = kv.second;

            if (pat == "_" || pat == "default") {
                hasDefault = true; L << "else { " << code << " } "; continue;
            }

            if (pat.size() >= 3 && pat.front() == '(' && pat.back() == ')') {
                std::string inside = trim2(pat.substr(1, pat.size() - 2));
                std::string a, b;
                size_t comma = inside.find(',');
                if (comma != std::string::npos) { a = trim2(inside.substr(0, comma)); b = trim2(inside.substr(comma + 1)); }
                else { a = inside; }
                L << (first ? "if" : "else if") << " (1) { ";
                if (!a.empty()) L << "auto " << a << " = __cs_subj._0; ";
                if (!b.empty()) L << "auto " << b << " = __cs_subj._1; ";
                L << code << " } ";
                first = false; continue;
            }

            std::ostringstream cond;
            {
                std::istringstream alts(pat);
                std::string tok; bool firstAlt = true;
                while (std::getline(alts, tok, '|')) {
                    tok = trim2(tok);
                    if (tok.empty()) continue;
                    cond << (firstAlt ? "" : " || ") << "(__cs_subj==(" << tok << "))";
                    firstAlt = false;
                }
            }
            L << (first ? "if" : "else if") << " (" << cond.str() << ") { " << code << " } ";
            first = false;
        }
        if (!hasDefault) L << "else { /* no-op */ } ";
        L << "}";
        out += L.str();
    }
    out.append(s, pos, std::string::npos);
    return out;
}

//============================= Softline lowering (with optional PGO hot set & inst) =============================
static string softline_lower(const string& src,
    bool softline_on,
    const set<string>& hotFns, // may be empty
    bool instrument // first PGO pass: inject cs_prof_hit
) {
    using namespace cs_regex_wrap;
    if (!softline_on) return src;

    string s = src;

    // 1) single-expression fn:  fn name(args) -> ret => expr;
    {
        std::regex r(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^=\{\n;]+)\s*=>\s*(.*?);)");
        cmatch m;
        size_t pos = 0;
        string rebuilt;
        while (search_from(s, pos, m, r)) {
            append_prefix(rebuilt, s, pos - (m.length(0) ? m.length(0) : 0), m);
            string name = trim(m[1].str());
            string args = m[2].str();
            string retty = trim(m[3].str());
            string expr = m[4].str();

            bool hot = hotFns.count(name) > 0;
            std::ostringstream fn;
            fn << (hot ? "static CS_HOT inline " : "static inline ")
                << retty << " " << name << "(" << args << "){ ";
            if (instrument) fn << "cs_prof_hit(\"" << name << "\"); ";
            fn << "return (" << expr << "); }";
            rebuilt += fn.str();
            // pos already advanced
        }
        rebuilt.append(s, pos, string::npos);
        s.swap(rebuilt);
    }

    // 2) block fn header: fn name(args) -> ret {
    {
        std::regex r(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^\{;\n]+)\s*\{)");
        cmatch m;
        size_t pos = 0;
        string rebuilt;
        while (search_from(s, pos, m, r)) {
            append_prefix(rebuilt, s, pos - (m.length(0) ? m.length(0) : 0), m);
            string name = trim(m[1].str());
            string args = m[2].str();
            string retty = trim(m[3].str());
            bool hot = hotFns.count(name) > 0;
            std::ostringstream hdr;
            hdr << (hot ? "CS_HOT " : "") << retty << " " << name << "(" << args << ")" << "{ ";
            if (instrument) hdr << "cs_prof_hit(\"" << name << "\"); ";
            rebuilt += hdr.str();
        }
        rebuilt.append(s, pos, string::npos);
        s.swap(rebuilt);
    }

    // 3) let -> const ; var -> (erase)
    {
        std::regex r1(R"(\blet\s+)"); 
        std::regex r2(R"(\bvar\s+)");
        s = std::regex_replace(s, r1, "const ");
        s = std::regex_replace(s, r2, "");
    }

    return s;
}

//============================= Spinner (Animated CLI glyphs) ==========================
struct Spinner {
    std::atomic<bool> on{ false };
    std::thread th;
    std::string label;

    void start(const std::string& what, bool enabled = true) {
        if (!enabled) return;
        label = what;
        on = true;
        th = std::thread([this] {
            static const char* frames = "|/-\\";
            size_t i = 0;
            using namespace std::chrono_literals;
            std::cerr << label << " ";
            while (on.load(std::memory_order_relaxed)) {
                std::cerr << '\r' << label << " " << frames[i++ & 3] << std::flush;
                std::this_thread::sleep_for(90ms);
            }
            // Fix C4566 (code page 1252): replace Unicode checkmark with ASCII
            std::cerr << '\r' << label << " [OK]" << std::string(6, ' ') << "\n";
            });
    }
    void stop() {
        if (!on) return;
        on = false;
        if (th.joinable()) th.join();
    }
    ~Spinner() { stop(); }
};

//============================= Guardian Overlays =============================
// Safe getenv wrapper for Windows (fix C4996) and POSIX
static bool cs_env_autoyes() {
#if defined(_WIN32)
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "CS_GUARDIAN_AUTOYES") == 0 && val) {
        std::string v(val);
        free(val);
        return v == "1";
    }
    return false;
#else
    const char* v = std::getenv("CS_GUARDIAN_AUTOYES");
    return v && std::string(v) == "1";
#endif
}

static bool is_protected_path(const std::string& p) {
#if defined(_WIN32)
    // crude heuristic: system drive + Program Files / Windows
    std::string P = p; for (auto& c : P) c = (char)tolower((unsigned char)c);
    return P.find("\\windows\\") != std::string::npos || P.find("\\program files") != std::string::npos;
#else
    return p == "/usr/bin" || p == "/usr/local/bin" || p.rfind("/bin", p.size() - 4) != std::string::npos;
#endif
}

static bool guardian_confirm(const Config& cfg, const std::string& src_for_scan, const std::string& action) {
    if (!cfg.guardian) return true;

    // CI and scripted builds can override interaction:
    if (cs_env_autoyes()) return true;

    bool hasUnsafe = (src_for_scan.find("@unsafe") != std::string::npos);
    std::cerr << "\n[Guardian] About to " << action
        << (hasUnsafe ? " (source contains @unsafe)" : "")
        << ". Proceed? [y/N] ";
    std::string ans; std::getline(std::cin, ans);
    if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) return true;
    std::cerr << "[Guardian] Aborted by user.\n";
    return false;
}

//============================= CC picker & runner =============================
static string pick_cc(const string& prefer = "") {
#if defined(_WIN32)
    vector<string> cands;
    if (!prefer.empty()) cands.push_back(prefer);
    // Try clang first on Windows
    cands.insert(cands.end(), { "clang","clang-cl","cl","gcc" });
    for (auto& c : cands) {
        string cmd = c + string(" --version > NUL 2>&1");
        if (system(cmd.c_str()) == 0) return c;
    }
    return "clang";
#else
    vector<string> cands = prefer.empty() ? vector<string>{"clang", "gcc"} : vector<string>{ prefer,"clang","gcc" };
    for (auto& c : cands) {
        string cmd = c + string(" --version > /dev/null 2>&1");
        if (system(cmd.c_str()) == 0) return c;
    }
    return "clang";
#endif
}

static int run_exe_with_env(const string& exe, const string& key, const string& val) {
#if defined(_WIN32)
    SetEnvironmentVariableA(key.c_str(), val.c_str());
    string cmd = "\"" + exe + "\"";
    int rc = system(cmd.c_str());
    SetEnvironmentVariableA(key.c_str(), NULL); // unset
    return rc;
#else
    string cmd = key + "=" + val + " \"" + exe + "\"";
    return system(cmd.c_str());
#endif
}

//============================= Build driver =============================
struct BuildOut {
    int rc = 0;
    string exe;
};

static string build_cmd(const Config& cfg, const string& cc, const string& cpath, const string& out,
    bool defineProfile = false) {
    vector<string> cmd; cmd.push_back(cc);
    bool msvc = (cc == "cl" || cc == "clang-cl");

    if (msvc) {
        cmd.push_back("/nologo");
        if (cfg.opt == "O0") cmd.push_back("/Od");
        else if (cfg.opt == "O1") cmd.push_back("/O1");
        else if (cfg.opt == "O2") cmd.push_back("/O2");
        else if (cfg.opt == "O3" || cfg.opt == "max") cmd.push_back("/O2");
        if (cfg.hardline || cfg.strict) { cmd.push_back("/Wall"); cmd.push_back("/WX"); }
        if (cfg.lto) cmd.push_back("/GL");
        if (cfg.hardline) cmd.push_back("/DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("/DCS_PROFILE_BUILD=1");
        for (auto& d : cfg.defines) cmd.push_back("/D" + d);
        for (auto& p : cfg.incs)    cmd.push_back("/I" + p);
        cmd.push_back(cpath);
        cmd.push_back("/Fe:" + out);
        for (auto& lp : cfg.libpaths) cmd.push_back("/link /LIBPATH:\"" + lp + "\"");
        for (auto& l : cfg.links) {
            string lib = l; if (lib.rfind(".lib") == string::npos) lib += ".lib";
            cmd.push_back("/link " + lib);
        }
    }
    else {
        cmd.push_back("-std=c11");
        if (cfg.opt == "O0") cmd.push_back("-O0");
        else if (cfg.opt == "O1") cmd.push_back("-O1");
        else if (cfg.opt == "O2") cmd.push_back("-O2");
        else if (cfg.opt == "O3") cmd.push_back("-O3");
        else if (cfg.opt == "size") cmd.push_back("-Os");
        else if (cfg.opt == "max") { cmd.push_back("-O3"); if (cfg.lto) cmd.push_back("-flto"); }
        if (cfg.hardline) {
            cmd.push_back("-Wall"); cmd.push_back("-Wextra"); cmd.push_back("-Werror");
            cmd.push_back("-Wconversion"); cmd.push_back("-Wsign-conversion");
        }
        if (cfg.lto) cmd.push_back("-flto");
        if (cfg.hardline) cmd.push_back("-DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("-DCS_PROFILE_BUILD=1");
        for (auto& d : cfg.defines) { cmd.push_back("-D" + d); }
        for (auto& p : cfg.incs) { cmd.push_back("-I" + p); }
        cmd.push_back(cpath);
        cmd.push_back("-o"); cmd.push_back(out);
        for (auto& lp : cfg.libpaths) { cmd.push_back("-L" + lp); }
        for (auto& l : cfg.links) { cmd.push_back("-l" + l); }
    }

    // Join
    string full;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i )full += ' ';
        bool needQ = cmd[i].find(' ') != string::npos;
        if (needQ) full.push_back('"');
        full += cmd[i];
        if (needQ) full.push_back('"');
    }
    return full;
}

static int run_cmd(const string& cmd, bool echo = false) {
    if (echo) std::cerr << "CC: " << cmd << "\n";
    return system(cmd.c_str());
}

//============================= PGO helper =============================
static map<string, unsigned long long> read_profile_counts(const string& path) {
    map<string, unsigned long long> m;
    std::ifstream f(path);
    string name; unsigned long long cnt = 0ULL;
    while (f >> name >> cnt) { m[name] += cnt; }
    return m;
}
static set<string> select_hot_functions(const map<string, unsigned long long>& m, size_t topN = 16) {
    vector<pair<string, unsigned long long>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    set<string> hot;
    for (size_t i = 0; i < v.size() && i < topN; ++i) {
        if (v[i].second > 0) hot.insert(v[i].first);
    }
    return hot;
}

//============================= MAIN =============================
int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc < 2) { std::cerr << "usage: cscriptc [options] file.csc\n"; return 1; }

    Config cfg;
    string inpath;
    vector<string> args; args.reserve(argc);
    for (int i = 1; i < argc; i++ ) args.push_back(argv[i]);
    for (size_t i = 0; i < args.size(); ++i) {
        string a = args[i];
        if (a == "-o" && i + 1 < args.size()) { cfg.out = args[++i]; }
        else if (starts_with(a, "-O")) { cfg.opt = a.substr(1); }
        else if (a == "--no-lto") { cfg.lto = false; }
        else if (a == "--strict") { cfg.strict = true; cfg.hardline = true; }
        else if (a == "--relaxed") { cfg.relaxed = true; }
        else if (a == "--show-c") { cfg.show_c = true; }
        else if (a == "--cc" && i + 1 < args.size()) { cfg.cc_prefer = args[++i]; }
        else if (!a.empty() && a[0] != '-') { inpath = a; }
    }
    if (inpath.empty()) { std::cerr << "error: missing input .csc file\n"; return 2; }

    // Read & split into directives + body
    string srcAll = read_file(inpath);
    vector<string> bodyLines;
   
    string body;
    body.reserve(srcAll.size());
    for (auto& l : bodyLines) { body += l; body.push_back('\n'); }

    // 1) Analyze enum! and emit typedefs + helpers; collect enum members
    map<string, EnumInfo> enums;
    string enumLowered = lower_enum_bang_and_collect(body, enums);

    // 2) Compile-time switch exhaustiveness checks against enum!
    check_exhaustiveness_or_die(body, enums); // analyze original macros in 'body'

    // 3) Lower @unsafe blocks
    string unsafeLowered = lower_unsafe_blocks(enumLowered);

    // 4) PGO two-pass (optional)
    set<string> hotFns; // selected after pass 1
    string cc = pick_cc(cfg.cc_prefer);

    // Guardian confirmation before instrumented run (if requested)
    if (cfg.profile) {
        if (!guardian_confirm(cfg, unsafeLowered, "build & run instrumented binary")) return 5;
    }

    auto build_once = [&](const string& c_src, const string& out, bool profileBuild)->int {
        string cpath = write_temp(string("cscript_") + std::to_string(uintptr_t(&cfg)) + ".c", c_src);
        string cmd = build_cmd(cfg, cc, cpath, out, profileBuild);
        if (cfg.show_c) { std::cerr << "--- generated C ---\n" << c_src << "\n--- end ---\n"; }

        Spinner sp;
        sp.start(profileBuild ? "Compiling (instrumented)" : "Compiling", cfg.ui_anim);
        int rc = run_cmd(cmd, cfg.show_c);
        sp.stop();

        if (!cfg.show_c) rm_file(cpath);
        return rc;
        };

    if (cfg.profile) {
        // First pass: instrument softline fns and build temp exe
        string s1 = prelude(cfg.hardline);
        string inst = softline_lower(unsafeLowered, cfg.softline, /*hot*/{}, /*instrument*/true);
        s1 += "\n"; s1 += inst;

        string tempExeProfile;
#if defined(_WIN32)
        tempExeProfile = write_temp("cscript_prof.exe", "");
        rm_file(tempExeProfile); // unique path; build will recreate
#else
        tempExeProfile = write_temp("cscript_prof.out", "");
        rm_file(tempExeProfile);
#endif
        if (build_once(s1, tempExeProfile, /*defineProfile*/true) != 0) {
            std::cerr << "build failed (instrumented pass)\n"; return 3;
        }

        // Run once with profile output file
        string profPath = write_temp("cscript_profile.txt", "");
        rm_file(profPath);
        int rcRun = run_exe_with_env(tempExeProfile, "CS_PROFILE_OUT", profPath);
        if (rcRun != 0) {
            std::cerr << "warning: instrumented run returned " << rcRun << "; proceeding\n";
        }
        // Read profile and select hot functions
        auto counts = read_profile_counts(profPath);
        hotFns = select_hot_functions(counts, 16);
        rm_file(profPath);
        rm_file(tempExeProfile);
    }

    // Guardian overlay for protected output path
    if (is_protected_path(cfg.out)) {
        if (!guardian_confirm(cfg, unsafeLowered, std::string("write output to protected path: ") + cfg.out)) return 6;
    }

    // If mutation tracking requested, define the toggle for the C translation unit
    if (cfg.track_mutations) cfg.defines.push_back("CS_TRACK_MUTATIONS=1");

    // 5) Final lowering with hot attributes, no instrumentation
    string csrc = prelude(cfg.hardline);

    // NEW: lower match-patterns (including destructuring) first, then softline:
    string matchLowered = lower_match_patterns(unsafeLowered);
    string lowered = softline_lower(matchLowered, cfg.softline, hotFns, /*instrument*/false);

    csrc += "\n"; csrc += lowered;

    // 6) Final build to single exe
    if (build_once(csrc, cfg.out, /*defineProfile*/false) != 0) {
        std::cerr << "build failed\n"; return 4;
    }

    std::cout << cfg.out << "\n";
    return 0;
}

// ============================================================================
// ==============  EMBEDDED LLVM + LLD (in-process), no system CC  ============
// ============================================================================
// To enable: define CS_EMBED_LLVM at compile time and link LLVM/Clang/LLD libs.
// Example:
//   clang++ -std=c++17 cscriptc.cpp -o cscriptc \
//     -DCS_EMBED_LLVM=1 \
//     `llvm-config --cxxflags --ldflags --system-libs --libs all` \
//     -lclangFrontend -lclangDriver -lclangCodeGen -lclangParse -lclangSema \
//     -lclangSerialization -lclangAST -lclangLex -lclangBasic \
//     -lclangToolingCore -lclangRewrite -lclangARCMigrate \
//     -llldELF -llldCOFF -llldMachO -llldCommon

#if defined(CS_EMBED_LLVM)

#include <memory>
#include <chrono>

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Serialization/PCHContainerOperations.h"

#include "lld/Common/Driver.h"
#include "lld/Common/Version.h"
#include "lld/Common/Memory.h"
#include "lld/COFF/Driver.h"
#include "lld/ELF/Driver.h"
#include "lld/MachO/Driver.h"

// ---- Utility: ephemeral file (used only when a linker flavor demands a path)
static std::string cs_write_temp_file(const std::string& base, llvm::StringRef data) {
    std::string dir;
#if defined(_WIN32)
    char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
    dir = std::string(buf);
#else
    const char* t = getenv("TMPDIR"); dir = t ? t : "/tmp/";
#endif
    std::string path = dir + base;
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_None);
    if (ec) throw std::runtime_error("cannot create temp file: " + path);
    os << data;
    os.close();
    return path;
}

static void cs_rm(const std::string& p) { std::remove(p.c_str()); }

// ---- Map @opt to Clang codegen levels
static void cs_apply_codegen_opts(clang::CodeGenOptions& CGO, const Config& cfg) {
    if (cfg.opt == "O0") CGO.OptimizationLevel = 0;
    else if (cfg.opt == "O1") CGO.OptimizationLevel = 1;
    else if (cfg.opt == "O2") CGO.OptimizationLevel = 2;
    else /*O3/max/size*/   CGO.OptimizationLevel = 3;
}

// ---- Build an in-memory Clang CompilerInstance that emits an object buffer
static std::unique_ptr<llvm::MemoryBuffer>
cs_compile_c_to_obj_inproc(const std::string& c_source,
    const Config& cfg,
    const std::vector<std::string>& incs,
    const std::vector<std::string>& defines) {
    using namespace clang;
    using namespace llvm;

    // Initialize LLVM targets (once).
    static bool inited = false;
    if (!inited) {
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmParsers();
        InitializeAllAsmPrinters();
        inited = true;
    }

    auto DiagOpts = std::make_shared<DiagnosticOptions>();
    auto DiagPrinter = std::make_unique<TextDiagnosticPrinter>(llvm::errs(), DiagOpts.get());
    IntrusiveRefCntPtr<DiagnosticsEngine> Diags(
        new DiagnosticsEngine(new DiagnosticIDs(), &*DiagOpts, DiagPrinter.release(), false));

    CompilerInstance CI;
    CI.createDiagnostics(DiagPrinter.release(), false);

    auto Inv = std::make_shared<CompilerInvocation>();
    // Language: C11
    LangOptions& LO = Inv->getLangOpts();
    LO.C11 = 1;
    LO.C99 = 1;
    LO.GNUMode = 1;

    // Target triple: host
    std::shared_ptr<clang::TargetOptions> TO = std::make_shared<clang::TargetOptions>();
    TO->Triple = llvm::sys::getDefaultTargetTriple();
    Inv->setTargetOpts(*TO);

    // Header search / preprocessor
    PreprocessorOptions& PP = Inv->getPreprocessorOpts();
    for (auto& d : defines) PP.addMacroDef(d);
    HeaderSearchOptions& HS = Inv->getHeaderSearchOpts();
    for (auto& p : incs) HS.AddPath(p, frontend::Angled, false, false);

    // CodeGen / Frontend
    CodeGenOptions& CGO = Inv->getCodeGenOpts();
    cs_apply_codegen_opts(CGO, cfg);
    if (cfg.lto) {
        CGO.EmitLLVMUsableTypeMetadata = 1;
    }
    Inv->getFrontendOpts().Inputs.clear();
    Inv->getFrontendOpts().ProgramAction = frontend::EmitObj;
    Inv->getFrontendOpts().Inputs.emplace_back("input.c", clang::Language::C);

    // Filesystem: put the C source into an in-memory file "input.c"
    auto InMemFS = llvm::vfs::InMemoryFileSystem::create();
    auto MB = llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(c_source), "input.c");
    InMemFS->addFile("input.c", /*modtime*/ 0, std::move(MB));

    auto OverlayFS = std::make_shared<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    OverlayFS->pushOverlay(std::move(InMemFS));

    CI.setInvocation(std::move(Inv));
    CI.createFileManager(OverlayFS);
    CI.createSourceManager(CI.getFileManager());

    const clang::FileEntry* FE = CI.getFileManager().getFile("input.c");
    if (!FE) throw std::runtime_error("failed to map in-memory source as FileEntry");
    CI.getSourceManager().setMainFileID(CI.getSourceManager().createFileID(FE, clang::SourceLocation(), clang::SrcMgr::C_User));

    CI.createPreprocessor(clang::TU_Complete);
    CI.createASTContext();

    clang::EmitObjAction Act;
    if (!CI.ExecuteAction(Act))
        throw std::runtime_error("clang in-proc codegen failed");

    // Grab the produced object as a MemoryBuffer
#if CLANG_VERSION_MAJOR >= 15
    std::unique_ptr<llvm::MemoryBuffer> Obj = Act.takeGeneratedObject();
#else
    std::unique_ptr<llvm::MemoryBuffer> Obj = Act.takeOutputFile(); // older
#endif
    if (!Obj) throw std::runtime_error("no object produced by clang action");

    return Obj;
}

// ---- LLD link (ELF/COFF/Mach-O) in-process to a final .exe
static int cs_link_with_lld(const Config& cfg,
    llvm::MemoryBufferRef objRef,
    const std::string& outPath) {
    using namespace llvm;

    std::string tmpObj = cs_write_temp_file("cscript_lld_obj.o", objRef.getBuffer());
    int rc = 1;

#if defined(_WIN32)
    // COFF
    std::vector<const char*> args;
    std::string outOpt = std::string("/OUT:") + outPath;
    args.push_back("lld-link");
    args.push_back(outOpt.c_str());
    args.push_back(tmpObj.c_str());
    args.push_back("/SUBSYSTEM:CONSOLE");
    args.push_back("/ENTRY:mainCRTStartup");
    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { hold.push_back(std::string("/LIBPATH:") + lp); args.push_back(hold.back().c_str()); }
    for (auto& l : cfg.links) {
        std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib";
        hold.push_back(lib); args.push_back(hold.back().c_str());
    }
    hold.push_back("/defaultlib:msvcrt");
    args.push_back(hold.back().c_str());
    if (lld::coff::link(args, /*canExitEarly*/ false, llvm::outs(), llvm::errs()))
        rc = 0;

#elif defined(__APPLE__)
    // Mach-O
    std::vector<const char*> args;
    args.push_back("ld64.lld");
    args.push_back("-o"); args.push_back(outPath.c_str());
    args.push_back(tmpObj.c_str());
    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { hold.push_back(std::string("-L") + lp); args.push_back(hold.back().c_str()); }
    for (auto& l : cfg.links) { hold.push_back(std::string("-l") + l);  args.push_back(hold.back().c_str()); }
    args.push_back("-lSystem");
    if (lld::macho::link(args, /*canExitEarly*/ false, llvm::outs(), llvm::errs()))
        rc = 0;

#else
    // ELF
    std::vector<const char*> args;
    args.push_back("ld.lld");
    args.push_back("-o"); args.push_back(outPath.c_str());
    args.push_back(tmpObj.c_str());

    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { hold.push_back(std::string("-L") + lp); args.push_back(hold.back().c_str()); }
    for (auto& l : cfg.links) { hold.push_back(std::string("-l") + l);  args.push_back(hold.back().c_str()); }

    const char* dl_candidates[] = {
      "/lib64/ld-linux-x86-64.so.2",
      "/lib/ld-linux-x86-64.so.2",
      "/lib64/ld-linux-aarch64.so.1",
      "/lib/ld-linux-aarch64.so.1"
    };
    for (auto* d : dl_candidates) {
        if (llvm::sys::fs::exists(d)) {
            args.push_back("--dynamic-linker");
            args.push_back(d);
            break;
        }
    }
    hold.push_back("-lc"); args.push_back(hold.back().c_str());

    if (lld::elf::link(args, /*canExitEarly*/ false, llvm::outs(), llvm::errs()))
        rc = 0;
#endif

    cs_rm(tmpObj);
    if (rc != 0) std::remove(outPath.c_str()); // ensure no half-baked output
    return rc;
}

// ---- Public entry: Build once *entirely in-process* (replaces system CC path)
static int build_once_llvm_inproc(const Config& cfg,
    const std::string& c_src,
    const std::string& outPath) {
    std::vector<std::string> incs = cfg.incs;
    std::vector<std::string> defs = cfg.defines;
    if (cfg.hardline) defs.push_back("CS_HARDLINE=1");

    auto ObjBuf = cs_compile_c_to_obj_inproc(c_src, cfg, incs, defs);

    Spinner sp;
    sp.start("Linking (LLD, in-proc)", true);
    int rc = cs_link_with_lld(cfg, ObjBuf->getMemBufferRef(), outPath);
    sp.stop();

    if (rc != 0) {
        llvm::WithColor::error(llvm::errs(), "lld") << "link failed for " << outPath << "\n";
        return 1;
    }
    return 0;
}

// Helper that mirrors the earlier signature & behavior
static int cs_build_once_embed(const Config& cfg,
    const std::string& c_src,
    const std::string& outPath,
    bool /*profileDefineUnusedHere*/) {
    try {
        return build_once_llvm_inproc(cfg, c_src, outPath);
    }
    catch (const std::exception& e) {
        std::cerr << "embed-LLVM build error: " << e.what() << "\n";
        return 1;
    }
}

#ifdef CS_PGO_EMBED
#define CS_BUILD_ONCE_PROFILE(cfg, c_src, outTmp) cs_build_once_embed(cfg, c_src, outTmp, true)
#else
#define CS_BUILD_ONCE_PROFILE(cfg, c_src, outTmp) build_once(c_src, outTmp, /*defineProfile*/true)
#endif

#define CS_BUILD_ONCE_FINAL(cfg, c_src, outPath) cs_build_once_embed(cfg, c_src, outPath, false)

#endif // CS_EMBED_LLVM

// ============================================================================
// ======  FULL IR-LEVEL COUNTER PASS (100% in-proc profiling, no toolchain) ==
// ============================================================================
// Enable together with embedded LLVM:
//   -DCS_EMBED_LLVM=1 -DCS_PGO_EMBED=1
// and link against LLVM/Clang/LLD as shown above.

#if defined(CS_EMBED_LLVM) && defined(CS_PGO_EMBED)

#include <memory>
#include <utility>

#include "llvm/ADT/Triple.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Serialization/PCHContainerOperations.h"
#include "Source.h"

// ---- Compile C text to LLVM Module in-process (instead of straight object)
static std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>>
cs_compile_c_to_module_inproc(const std::string& c_source,
    const Config& cfg,
    const std::vector<std::string>& incs,
    const std::vector<std::string>& defines) {
    using namespace clang;
    using namespace llvm;

    static bool inited = false;
    if (!inited) {
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmParsers();
        InitializeAllAsmPrinters();
        inited = true;
    }

    auto DiagOpts = std::make_shared<DiagnosticOptions>();
    auto DiagPrinter = std::make_unique<TextDiagnosticPrinter>(llvm::errs(), DiagOpts.get());
    IntrusiveRefCntPtr<DiagnosticsEngine> Diags(
        new DiagnosticsEngine(new DiagnosticIDs(), &*DiagOpts, DiagPrinter.release(), false));

    auto Ctx = std::make_unique<LLVMContext>();

    CompilerInstance CI;
    CI.createDiagnostics();

    auto Inv = std::make_shared<CompilerInvocation>();
    LangOptions& LO = Inv->getLangOpts();
    LO.C11 = 1; LO.C99 = 1; LO.GNUMode = 1;

    auto TO = std::make_shared<TargetOptions>();
    TO->Triple = sys::getDefaultTargetTriple();
    Inv->setTargetOpts(*TO);

    PreprocessorOptions& PP = Inv->getPreprocessorOpts();
    for (auto& d : defines) PP.addMacroDef(d);
    HeaderSearchOptions& HS = Inv->getHeaderSearchOpts();
    for (auto& p : incs) HS.AddPath(p, frontend::Angled, false, false);

    Inv->getFrontendOpts().Inputs.clear();
    Inv->getFrontendOpts().ProgramAction = frontend::EmitLLVMOnly;
    Inv->getFrontendOpts().Inputs.emplace_back("input.c", clang::Language::C);

    // Map source in-memory
    auto InMemFS = llvm::vfs::InMemoryFileSystem::create();
    auto MB = llvm::MemoryBuffer::getMemBufferCopy(StringRef(c_source), "input.c");
    InMemFS->addFile("input.c", 0, std::move(MB));
    auto OverlayFS = std::make_shared<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    OverlayFS->pushOverlay(std::move(InMemFS));

    CI.setInvocation(std::move(Inv));
    CI.createFileManager(OverlayFS);
    CI.createSourceManager(CI.getFileManager());

    const clang::FileEntry* FE = CI.getFileManager().getFile("input.c");
    if (!FE) throw std::runtime_error("failed to map in-memory source as FileEntry");
    CI.getSourceManager().setMainFileID(CI.getSourceManager().createFileID(FE, clang::SourceLocation(), clang::SrcMgr::C_User));
    CI.createPreprocessor(clang::TU_Complete);
    CI.createASTContext();

    clang::EmitLLVMOnlyAction Act(Ctx.get());
    if (!CI.ExecuteAction(Act))
        throw std::runtime_error("clang IR generation failed");

#if CLANG_VERSION_MAJOR >= 15
    std::unique_ptr<llvm::Module> M = Act.takeModule();
#else
    std::unique_ptr<llvm::Module> M = Act.takeLLVMModule();
#endif
    if (!M) throw std::runtime_error("no LLVM module produced");

    return { std::move(M), std::move(Ctx) };
}

// ---- IR-level profiling pass (new pass manager)
struct CSProfilePass : llvm::PassInfoMixin<CSProfilePass> {
    llvm::PreservedAnalyses run(llvm::Module& M, llvm::ModuleAnalysisManager&) {
        using namespace llvm;
        LLVMContext& C = M.getContext();

        // Declare (or get) void cs_prof_hit(i8*)
        FunctionCallee Hit = M.getOrInsertFunction(
            "cs_prof_hit",
            FunctionType::get(Type::getVoidTy(C), { Type::getInt8PtrTy(C) }, false));

        for (Function& F : M) {
            if (F.isDeclaration()) continue;
            if (F.getName().startswith("llvm.")) continue;

            BasicBlock& Entry = F.getEntryBlock();
            IRBuilder<> B(&*Entry.getFirstInsertionPt());

            Value* Name = B.CreateGlobalStringPtr(F.getName(), "cs_fn_name");
            B.CreateCall(Hit, { Name });
        }

        return PreservedAnalyses::none();
    }
};

// ---- Utility: run our pass + a standard O-level pipeline before codegen
static void cs_run_ir_pipeline(llvm::Module& M, int optLevel) {
    using namespace llvm;

    PassBuilder PB;
    LoopAnalysisManager     LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager    CGAM;
    ModuleAnalysisManager   MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    ModulePassManager MPM;
    MPM.addPass(CSProfilePass());

    OptimizationLevel O = OptimizationLevel::O2;
    switch (optLevel) {
    case 0: O = OptimizationLevel::O0; break;
    case 1: O = OptimizationLevel::O1; break;
    case 2: O = OptimizationLevel::O2; break;
    case 3: default: O = OptimizationLevel::O3; break;
    }
    MPM.addPass(PB.buildPerModuleDefaultPipeline(O));

    MPM.run(M, MAM);
}

//---- Public entry: Build once (instrumented profiling pass) entirely in-proc
static int cs_build_once_embed_profile_irpass(const Config& cfg,
    const std::string& c_src,
    const std::string& outTmpExecutable) {
    try {
        std::vector<std::string> incs = cfg.incs;
        std::vector<std::string> defs = cfg.defines;
        defs.push_back("CS_PROFILE_BUILD=1");
        if (cfg.hardline) defs.push_back("CS_HARDLINE=1");

        auto pairMC = cs_compile_c_to_module_inproc(c_src, cfg, incs, defs);
        std::unique_ptr<llvm::Module>& Mod = pairMC.first;

        int oLvl = 2;
        if (cfg.opt == "O0") oLvl = 0;
        else if (cfg.opt == "O1") oLvl = 1;
        else if (cfg.opt == "O2") oLvl = 2;
        else                    oLvl = 3;

        cs_run_ir_pipeline(*Mod, oLvl);

        auto ObjBuf = cs_emit_obj_from_module(*Mod, cfg);


        int rc = cs_link_with_lld(cfg, ObjBuf->getMemBufferRef(), outTmpExecutable);
        return rc;
    }
    catch (const std::exception& e) {
        std::cerr << "IR-pass profiling build error: " << e.what() << "\n";
        return 1;
    }
}

// Override profiling builder to use IR-pass path
#undef CS_BUILD_ONCE_PROFILE
#define CS_BUILD_ONCE_PROFILE(cfg, c_src, outTmp) \
  cs_build_once_embed_profile_irpass(cfg, c_src, outTmp)

// Final pass already uses embedded path via CS_BUILD_ONCE_FINAL defined earlier

#endif // CS_EMBED_LLVM && CS_PGO_EMBED

//============================= Directives & body =============================
static void parse_directives_and_collect(const string& in, Config& cfg, vector<string>& body) {
    std::istringstream ss(in);
    string line;
    while (std::getline(ss, line)) {
        string t = trim(line);
        if (starts_with(t, "@")) {
            std::istringstream ls(t.substr(1));
            string name; ls >> name;
            if (name == "hardline") { string v; ls >> v; cfg.hardline = (v != "off"); }
            else if (name == "softline") { string v; ls >> v; cfg.softline = (v != "off"); }
            else if (name == "opt") { string v; ls >> v; cfg.opt = v; }
            else if (name == "lto") { string v; ls >> v; cfg.lto = (v != "off"); }
            else if (name == "profile") { string v; ls >> v; cfg.profile = (v != "off"); }
            else if (name == "out") { string v; ls >> std::quoted(v); cfg.out = v; }
            else if (name == "abi") { string v; ls >> std::quoted(v); cfg.abi = v; }
            else if (name == "define") { string v; ls >> v; cfg.defines.push_back(v); }
            else if (name == "inc") { string v; ls >> std::quoted(v); cfg.incs.push_back(v); }
            else if (name == "libpath") { string v; ls >> std::quoted(v); cfg.libpaths.push_back(v); }
            else if (name == "link") { string v; ls >> std::quoted(v); cfg.links.push_back(v); }
            // NEW directives
            else if (name == "guardian") { string v; ls >> v; cfg.guardian = (v != "off"); }
            else if (name == "anim") { string v; ls >> v; cfg.ui_anim = (v != "off"); }
            else if (name == "muttrack") { string v; ls >> v; cfg.track_mutations = (v != "off"); }
            else { std::cerr << "warning: unknown directive @" << name << "\n"; }
            continue;
        }
        body.push_back(line);
    }
}

// Add near other helpers (cross-platform getenv that returns std::string)
static std::string cs_getenv_string(const char* key) {
#if defined(_WIN32)
    char* val = nullptr; size_t len = 0;
    if (_dupenv_s(&val, &len, key) == 0 && val) {
        std::string s(val); free(val); return s;
    }
    return {};
#else
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
#endif
}

// cscriptc.cpp — C-Script v0.4 reference compiler (front + analyzer + PGO + driver)
// Build: g++ -std=gnu++17 cscriptc.cpp -o cscriptc      (Linux/macOS, Clang/GCC)
//     or clang++ -std=c++17 cscriptc.cpp -o cscriptc
//     or (MSYS/Clang on Windows recommended). MSVC works but needs regex fixes below.
// Usage:  ./cscriptc file.csc [--show-c] [--strict] [-O{0|1|2|3|max|size}] [-o out.exe]
// Notes:
//  - Produces ONE final .exe. Any temps for build/PGO are deleted.
//  - PGO: when @profile on, we instrument softline `fn` entries, run once, then rebuild with hot attrs.
//  - Exhaustiveness: compile-time error if a CS_SWITCH_EXHAUSTIVE for an enum! misses any cases.
//  - NEW in v0.4: directives @sanitize, @debug, @time, @import, plus @checked blocks and timing.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <iomanip>   // std::quoted
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

using std::string;
using std::vector;
using std::set;
using std::map;
using std::pair;

// ================= Timing Utility =================
using Clock = std::chrono::high_resolution_clock;
static void log_time(const char* tag, Clock::time_point t0, bool on) {
    if (!on) return;
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::cerr << "[time] " << tag << ": " << dt << " ms\n";
}

// ================= Diagnostics ====================
static void warn(const string& msg) {
#if defined(_WIN32)
    std::cerr << "[warn] " << msg << "\n";
#else
    std::cerr << "\033[33m[warn]\033[0m " << msg << "\n";
#endif
}
static void error(const string& msg) {
#if defined(_WIN32)
    std::cerr << "[error] " << msg << "\n";
#else
    std::cerr << "\033[31m[error]\033[0m " << msg << "\n";
#endif
}

// ============================================================================
// Regex wrapper for MSVC iterator + match_results quirks (portable).
// ============================================================================
namespace cs_regex_wrap {
    using cmatch = std::match_results<std::string::const_iterator>;

    inline bool search_iter(std::string::const_iterator begin,
        std::string::const_iterator end,
        cmatch& m,
        const std::regex& re)
    {
        return std::regex_search(begin, end, m, re);
    }

    inline bool search_from(const std::string& s,
        size_t& pos,
        cmatch& m,
        const std::regex& re)
    {
        if (pos > s.size()) return false;
        auto b = s.begin() + static_cast<std::ptrdiff_t>(pos);
        auto e = s.end();
        if (!std::regex_search(b, e, m, re)) return false;
        pos = static_cast<size_t>(m.suffix().first - s.begin());
        return true;
    }

    inline void append_prefix(std::string& out,
        const std::string& src,
        size_t start,
        const cmatch& m)
    {
        const size_t endAbs = static_cast<size_t>(m.prefix().second - src.begin());
        if (endAbs >= start && endAbs <= src.size())
            out.append(src, start, endAbs - start);
    }
}

//============================= Config =============================
struct Config {
    bool hardline = true;
    bool softline = true;
    string opt = "O2";
    bool lto = true;
    bool profile = false;
    bool sanitize = false;      // NEW
    bool debug = false;         // NEW
    bool time = false;          // NEW
    string out = "a.exe";
    string abi = "";
    vector<string> defines, incs, libpaths, links;
    bool strict = false, relaxed = false, show_c = false;
    string cc_prefer = "";
};

// ================== helpers ====================
static bool starts_with(const string& s, const string& p) { return s.rfind(p, 0) == 0; }
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static string read_file(const string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + p);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static string write_temp(const string& base, const string& content) {
    string dir;
#if defined(_WIN32)
    char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
    dir = string(buf);
#else
    const char* t = getenv("TMPDIR"); dir = t ? t : "/tmp/";
#endif
    string path = dir + base;
    std::ofstream o(path, std::ios::binary); o << content;
    return path;
}
static void rm_file(const string& p) { std::remove(p.c_str()); }

// For error lines
static pair<int, int> line_col_at(const string& s, size_t pos) {
    int line = 1, col = 1;
    for (size_t i = 0; i < pos && i < s.size(); ++i) {
        if (s[i] == '\n') { line++; col = 1; }
        else col++;
    }
    return { line,col };
}

//============================= Directives & body =============================
static void parse_directives_and_collect(const string& in, Config& cfg, vector<string>& body) {
    std::istringstream ss(in);
    string line;
    while (std::getline(ss, line)) {
        string t = trim(line);
        if (starts_with(t, "@")) {
            std::istringstream ls(t.substr(1));
            string name; ls >> name;
            if (name == "hardline") { string v; ls >> v; cfg.hardline = (v != "off"); }
            else if (name == "softline") { string v; ls >> v; cfg.softline = (v != "off"); }
            else if (name == "opt") { string v; ls >> v; cfg.opt = v; }
            else if (name == "lto") { string v; ls >> v; cfg.lto = (v != "off"); }
            else if (name == "profile") { string v; ls >> v; cfg.profile = (v != "off"); }
            else if (name == "out") { string v; ls >> std::quoted(v); cfg.out = v; }
            else if (name == "abi") { string v; ls >> std::quoted(v); cfg.abi = v; }
            else if (name == "define") { string v; ls >> v; cfg.defines.push_back(v); }
            else if (name == "inc") { string v; ls >> std::quoted(v); cfg.incs.push_back(v); }
            else if (name == "libpath") { string v; ls >> std::quoted(v); cfg.libpaths.push_back(v); }
            else if (name == "link") { string v; ls >> std::quoted(v); cfg.links.push_back(v); }
            else if (name == "sanitize") { string v; ls >> v; cfg.sanitize = (v != "off"); }
            else if (name == "debug") { string v; ls >> v; cfg.debug = (v != "off"); }
            else if (name == "time") { string v; ls >> v; cfg.time = (v != "off"); }
            else if (name == "import") {
                string path; ls >> std::quoted(path);
                try {
                    string sub = read_file(path);
                    vector<string> bl; parse_directives_and_collect(sub, cfg, bl);
                    body.insert(body.end(), bl.begin(), bl.end());
                }
                catch (...) { warn("failed to import " + path); }
            }
            else { warn("unknown directive @" + name); }
            continue;
        }
        body.push_back(line);
    }
}

//============================= Checked blocks =============================
static string lower_checked_blocks(const string& in) {
    string s = in, out; size_t i = 0, n = s.size(); out.clear(); out.reserve(n * 11 / 10);
    while (i < n) {
        if (s.compare(i, 8, "@checked") == 0 && i + 8 < n && s[i + 8] == '{') {
            out += "{ assert("; i += 8; continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

// ==================== ENUM/UNSAFE/SOFTLINE LOWERING ====================
// (KEEP existing implementations from v0.3 here unchanged; omitted for brevity in this snippet)
// They are identical to your last full version, but now call lower_checked_blocks after unsafeLowered.

// … (enum! lowering, CS_SWITCH_EXHAUSTIVE check, @unsafe blocks, softline lowering, etc.) …


//============================= MAIN =============================
int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc < 2) { error("usage: cscriptc [options] file.csc"); return 1; }

    Config cfg;
    string inpath;
    vector<string> args; args.reserve(argc);
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    for (size_t i = 0; i < args.size(); ++i) {
        string a = args[i];
        if (a == "-o" && i + 1 < args.size()) { cfg.out = args[++i]; }
        else if (starts_with(a, "-O")) { cfg.opt = a.substr(1); }
        else if (a == "--no-lto") { cfg.lto = false; }
        else if (a == "--strict") { cfg.strict = true; cfg.hardline = true; }
        else if (a == "--relaxed") { cfg.relaxed = true; }
        else if (a == "--show-c") { cfg.show_c = true; }
        else if (a == "--cc" && i + 1 < args.size()) { cfg.cc_prefer = args[++i]; }
        else if (a == "--sanitize") { cfg.sanitize = true; }
        else if (a == "--debug") { cfg.debug = true; }
        else if (a == "--time") { cfg.time = true; }
        else if (!a.empty() && a[0] != '-') { inpath = a; }
    }
    if (inpath.empty()) { error("missing input .csc file"); return 2; }

    auto t0 = Clock::now();
    string srcAll = read_file(inpath);
    vector<string> bodyLines;
    parse_directives_and_collect(srcAll, cfg, bodyLines);
    string body;
    for (auto& l : bodyLines) { body += l; body.push_back('\n'); }
    log_time("parse directives", t0, cfg.time);

    // ============== pipeline ============
    // Step 1: enum lowering
    map<string, EnumInfo> enums;
    string enumLowered = lower_enum_bang_and_collect(body, enums);

    // Step 2: check switch exhaustiveness
    check_exhaustiveness_or_die(body, enums);

    // Step 3: unsafe blocks
    string unsafeLowered = lower_unsafe_blocks(enumLowered);

    // Step 4: checked blocks
    string checkedLowered = lower_checked_blocks(unsafeLowered);

    // Step 5: PGO/softline lowering (unchanged from v0.3, now applied to checkedLowered)
    // … build_once, profile pass, etc …

    return 0;
}

#ifdef CS_EMBED_LLVM
//============================= Embedded LLVM/Clang/LLD =============================
// (KEEP existing implementations from v0.3 here unchanged; omitted for brevity in this snippet)
// They are identical to your last full version, but now call cs_build_once_embed_profile_irpass
// for the profiling build when both CS_EMBED_LLVM and CS_PGO_EMBED are defined.

// ... (all the embedded LLVM/Clang/LLD code from above) ...
#endif // CS_EMBED_LLVM
//============================= Directives & body =============================

static void parse_directives_and_collect(const string& in, Config& cfg, vector<string>& body) {
    std::istringstream ss(in);
    string line;
    while (std::getline(ss, line)) {
        string t = trim(line);
        if (starts_with(t, "@")) {
            std::istringstream ls(t.substr(1));
            string name; ls >> name;
            if (name == "hardline") { string v; ls >> v; cfg.hardline = (v != "off"); }
            else if (name == "softline") { string v; ls >> v; cfg.softline = (v != "off"); }
            else if (name == "opt") { string v; ls >> v; cfg.opt = v; }
            else if (name == "lto") { string v; ls >> v; cfg.lto = (v != "off"); }
            else if (name == "profile") { string v; ls >> v; cfg.profile = (v != "off"); }
            else if (name == "out") { string v; ls >> std::quoted(v); cfg.out = v; }
            else if (name == "abi") { string v; ls >> std::quoted(v); cfg.abi = v; }
            else if (name == "define") { string v; ls >> v; cfg.defines.push_back(v); }
            else if (name == "inc") { string v; ls >> std::quoted(v); cfg.incs.push_back(v); }
            else if (name == "libpath") { string v; ls >> std::quoted(v); cfg.libpaths.push_back(v); }
            else if (name == "link") { string v; ls >> std::quoted(v); cfg.links.push_back(v); }
            // NEW directives
            else if (name == "guardian") { string v; ls >> v; cfg.guardian = (v != "off"); }
            else if (name == "anim") { string v; ls >> v; cfg.ui_anim = (v != "off"); }
            else if (name == "muttrack") { string v; ls >> v; cfg.track_mutations = (v != "off"); }
            else { std::cerr << "warning: unknown directive @" << name << "\n"; }
            continue;
        }
		body
			.push_back(line);
	}
}
// Add near other helpers (cross-platform getenv that returns std::string)
static std::string cs_getenv_string(const char* key) {
    #if defined(_WIN32)
    char* val = nullptr; size_t len = 0;
    if (_dupenv_s(&val, &len, key) == 0 && val) {
        std::string s(val); free(val); return s;
    }
	return {};
    #else
    const char* v = std::getenv(key);
    return v ? std::string(v) : std::string();
#endif
}
// cscriptc.cpp — C-Script v0.4 reference compiler (front + analyzer + PGO + driver)
// Build: g++ -std=gnu++17 cscriptc.cpp -o cscriptc      (Linux/macOS, Clang/GCC)
//     or clang++ -std=c++17 cscriptc.cpp -o cscriptc
//     or (MSYS/Clang on Windows recommended). MSVC works but needs regex fixes below.
// Usage:  ./cscriptc file.csc [--show-c] [--strict] [-O{0|1|2|3|max|size}] [-o out.exe]
// Notes:
//  - Produces ONE final .exe. Any temps for build/PGO are deleted.
//  - PGO: when @profile on, we instrument softline `fn` entries, run once, then rebuild with hot attrs.
//  - Exhaustiveness: compile-time error if a CS_SWITCH_EXHAUSTIVE for an enum! misses any cases.
//  - NEW in v0.4: directives @sanitize, @debug, @time, @import, plus @checked blocks and timing.

// Replace the first struct Config definition near the top
struct Config {
    bool hardline = true;
    bool softline = true;
    string opt = "O2";
    bool lto = true;
    bool profile = false; // PGO two-pass
    string out = "a.exe";
    string abi = "";
    vector<string> defines, incs, libpaths, links;
    bool strict = false, relaxed = false, show_c = false;
    string cc_prefer = "";

    // NEW:
    bool ui_anim = true;           // animated CLI glyphs during compilation
    bool guardian = true;          // guardian confirmation overlays
    bool track_mutations = false;  // enable mutation tracking in generated C

    // NEW: no-dangling-pointer mode (enables ASan and helper macros)
    bool no_dangling = false;
};

//============================= MAIN =============================

// In prelude(bool hardline), just before `return o.str();`, add CS_SAFE_FREE:
    // --- Safe free helper (nulls pointer after free) ---
o << R"(
#ifndef CS_SAFE_FREE
#define CS_SAFE_FREE(p) do { if ((p)!=NULL) { free(p); (p)=NULL; } } while(0)
#endif
)";

return o.str();

// Replace build_cmd(...) to inject ASan flags when cfg.no_dangling is on
static string build_cmd(const Config& cfg, const string& cc, const string& cpath, const string& out,
    bool defineProfile = false) {
    vector<string> cmd; cmd.push_back(cc);
    bool msvc = (cc == "cl" || cc == "clang-cl");

    // When ASan is on, LTO is commonly incompatible; force it off at link time.
    const bool asan = cfg.no_dangling || false; // keep separate from cfg.sanitize in this branch

    if (msvc) {
        cmd.push_back("/nologo");
        if (cfg.opt == "O0") cmd.push_back("/Od");
        else if (cfg.opt == "O1") cmd.push_back("/O1");
        else if (cfg.opt == "O2" || cfg.opt == "O3" || cfg.opt == "max") cmd.push_back("/O2");

        if (cfg.hardline || cfg.strict) { cmd.push_back("/Wall"); cmd.push_back("/WX"); }
        if (cfg.lto && !asan) cmd.push_back("/GL");
        if (cfg.hardline) cmd.push_back("/DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("/DCS_PROFILE_BUILD=1");
        if (asan) {
            // VS 2022 x64 supports AddressSanitizer
            cmd.push_back("/fsanitize=address");
            cmd.push_back("/Zi"); // better diagnostics
        }

        for (auto& d : cfg.defines) cmd.push_back("/D" + d);
        if (asan) cmd.push_back("/DCS_NO_DANGLING=1");
        for (auto& p : cfg.incs)    cmd.push_back("/I" + p);
        cmd.push_back(cpath);
        cmd.push_back("/Fe:" + out);

        // Linker options
        for (auto& lp : cfg.libpaths) cmd.push_back("/link /LIBPATH:\"" + lp + "\"");
        for (auto& l : cfg.links) {
            string lib = l; if (lib.rfind(".lib") == string::npos) lib += ".lib";
            cmd.push_back("/link " + lib);
        }
    }
    else {
        cmd.push_back("-std=c11");
        if (cfg.opt == "O0") cmd.push_back("-O0");
        else if (cfg.opt == "O1") cmd.push_back("-O1");
        else if (cfg.opt == "O2") cmd.push_back("-O2");
        else if (cfg.opt == "O3") cmd.push_back("-O3");
        else if (cfg.opt == "size") cmd.push_back("-Os");

        if (cfg.hardline) {
            cmd.push_back("-Wall"); cmd.push_back("-Wextra"); cmd.push_back("-Werror");
            cmd.push_back("-Wconversion"); cmd.push_back("-Wsign-conversion");
        }

        if (cfg.lto && !asan) cmd.push_back("-flto");
        if (cfg.hardline) cmd.push_back("-DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("-DCS_PROFILE_BUILD=1");
        if (asan) {
            cmd.push_back("-fsanitize=address");
#if defined(__APPLE__)
            // macOS needs this to get clean backtraces
            cmd.push_back("-fno-omit-frame-pointer");
#endif
        }

        for (auto& d : cfg.defines) cmd.push_back("-D" + d);
        if (asan) cmd.push_back("-DCS_NO_DANGLING=1");

        for (auto& p : cfg.incs) { cmd.push_back("-I" + p); }
        cmd.push_back(cpath);
        cmd.push_back("-o"); cmd.push_back(out);
        for (auto& lp : cfg.libpaths) { cmd.push_back("-L" + lp); }
        for (auto& l : cfg.links) { cmd.push_back("-l" + l); }
    }

    // Join
    string full;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) full += ' ';
        bool needQ = cmd[i].find(' ') != string::npos;
        if (needQ) full.push_back('"');
        full += cmd[i];
        if (needQ) full.push_back('"');
    }
    return full;
}

// In main(...) CLI parsing, add the flag (insert alongside other flags)
        else if (a == "--no-dangling") { cfg.no_dangling = true; }

        // In parse_directives_and_collect(...) (the v0.3 one near the bottom), add @nodangling
        static void parse_directives_and_collect(const string& in, Config& cfg, vector<string>& body) {
            std::istringstream ss(in);
            string line;
            while (std::getline(ss, line)) {
                string t = trim(line);
                if (starts_with(t, "@")) {
                    std::istringstream ls(t.substr(1));
                    string name; ls >> name;
                    if (name == "hardline") { string v; ls >> v; cfg.hardline = (v != "off"); }
                    else if (name == "softline") { string v; ls >> v; cfg.softline = (v != "off"); }
                    else if (name == "opt") { string v; ls >> v; cfg.opt = v; }
                    else if (name == "lto") { string v; ls >> v; cfg.lto = (v != "off"); }
                    else if (name == "profile") { string v; ls >> v; cfg.profile = (v != "off"); }
                    else if (name == "out") { string v; ls >> std::quoted(v); cfg.out = v; }
                    else if (name == "abi") { string v; ls >> std::quoted(v); cfg.abi = v; }
                    else if (name == "define") { string v; ls >> v; cfg.defines.push_back(v); }
                    else if (name == "inc") { string v; ls >> std::quoted(v); cfg.incs.push_back(v); }
                    else if (name == "libpath") { string v; ls >> std::quoted(v); cfg.libpaths.push_back(v); }
                    else if (name == "link") { string v; ls >> std::quoted(v); cfg.links.push_back(v); }
                    else if (name == "guardian") { string v; ls >> v; cfg.guardian = (v != "off"); }
                    else if (name == "anim") { string v; ls >> v; cfg.ui_anim = (v != "off"); }
                    else if (name == "muttrack") { string v; ls >> v; cfg.track_mutations = (v != "off"); }
                    else if (name == "nodangling") { string v; ls >> v; cfg.no_dangling = (v != "off"); }
                    else { std::cerr << "warning: unknown directive @" << name << "\n"; }
                    continue;
                }
                body.push_back(line);
            }
        }

        // ============================ Extra Lowerings (append-only) ============================
        // Conservative regex-based transforms that can be wired into the pipeline without
        // touching existing passes. They operate before softline_lower.

        namespace cs_extra_lowerings {

            // (expr) |> func(args)  ==>  func((expr), args)
            // Notes:
            //   - Requires the subject expression to be parenthesized on the left.
            //   - Allows empty args: (expr) |> func()  ==> func((expr))
            //   - Spans across newlines conservatively.
            static std::string lower_pipeline_op(const std::string& src) {
                using namespace cs_regex_wrap;

                std::string s = src, out;
                out.reserve(s.size());

                // Pattern: ( <subject> ) |> <ident> ( <args> )
                // Use [\s\S]*? to span newlines under ECMAScript.
                std::regex re(R"(\(\s*([\s\S]*?)\s*\)\s*\|\>\s*([A-Za-z_]\w*)\s*\(\s*([\s\S]*?)\s*\))",
                    std::regex::ECMAScript);
                cmatch m;
                size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string subject = m[1].str();
                    const std::string fname = m[2].str();
                    const std::string args = m[3].str();
                    if (args.empty() || trim(args).empty()) {
                        out += fname + "((" + subject + "))";
                    }
                    else {
                        out += fname + "((" + subject + ")," + args + ")";
                    }
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // (a) ?? (b)  ==>  ((a) ? (a) : (b))
            // Notes:
            //   - Requires parentheses around both operands to keep the regex simple and safe.
            //   - Works for pointers and scalars (non-zero considered truthy).
            static std::string lower_null_coalescing(const std::string& src) {
                using namespace cs_regex_wrap;

                std::string s = src, out;
                out.reserve(s.size());

                std::regex re(R"(\(\s*([\s\S]*?)\s*\)\s*\?\?\s*\(\s*([\s\S]*?)\s*\))",
                    std::regex::ECMAScript);
                cmatch m;
                size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string a = m[1].str();
                    const std::string b = m[2].str();
                    out += "((" + a + ") ? (" + a + ") : (" + b + "))";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // Apply all extra lowerings in a stable order.
            // Call this just before softline_lower in your pipeline(s).
            static std::string apply_all(const std::string& src) {
                std::string t = lower_pipeline_op(src);
                t = lower_null_coalescing(t);
                return t;
            }

        } // namespace cs_extra_lowerings

// ============================ C23 Support Pack (append-only) ============================
// Enables compiling generated C in C23 mode and smooths minor portability gaps.
// Features:
//   - Directive:   @c23 on|off
//   - CLI flags:   --c23 / --no-c23
//   - GCC/Clang:   -std=c23 (fallback -std=c2x)
//   - clang-cl:    /clang:-std=c23 (fallback /clang:-std=c2x)
//   - cl (MSVC):   best effort (recommend using clang/clang-cl on Windows)
// Notes:
//   - This pack provides a replacement build_cmd_aware_of_c23(), and a tiny
//     prelude extension. Wire-up is minimal: search for build_cmd(...) call sites
//     and use build_cmd_c23(...) instead, or replace the existing build_cmd
//     implementation with this one.
//
// 1) Extend Config at runtime without editing its struct by tracking a side-channel flag.
        namespace cs_c23_support {
            static bool g_c23_on = false;

            // Loose scan for @c23 directive in the source body (on by default if found).
            static bool scan_c23_directive(const std::string& text, bool deflt) {
                std::istringstream ss(text);
                std::string line;
                bool seen = deflt;
                while (std::getline(ss, line)) {
                    std::string t = line;
                    auto trim = [](const std::string& s)->std::string {
                        size_t a = s.find_first_not_of(" \t\r\n");
                        if (a == std::string::npos) return "";
                        size_t b = s.find_last_not_of(" \t\r\n");
                        return s.substr(a, b - a + 1);
                        };
                    t = trim(t);
                    if (t.rfind("@c23", 0) == 0) {
                        std::istringstream ls(t.substr(4));
                        std::string v; ls >> v;
                        if (v == "off") seen = false;
                        else seen = true;
                    }
                }
                return seen;
            }

            // Prelude addendum to smooth some C23 spelling into older C (only used if needed).
            static std::string c23_prelude_addendum() {
                return R"(
/* --- C23 compatibility sugar (safe no-ops on real C23) --- */
#ifndef __STDC_VERSION__
  #define __STDC_VERSION__ 0
#endif
#if __STDC_VERSION__ < 202311L
  /* alignas/alignof spellings */
  #ifndef alignas
    #define alignas _Alignas
  #endif
  #ifndef alignof
    #define alignof _Alignof
  #endif
  /* nullptr spelling */
  #ifndef nullptr
    #define nullptr ((void*)0)
  #endif
  /* [[maybe_unused]] etc. — ignore if not supported by compiler */
  #if !defined(__has_c_attribute)
    #define __has_c_attribute(x) 0
  #endif
#endif
)";
            }

            // Compose a command with a C23 standard flag tailored to the tool.
            static void add_c23_std_flag(const std::string& cc, std::vector<std::string>& cmd, bool msvc) {
                // Try std=c23 first, then std=c2x as fallback.
                if (msvc) {
                    // clang-cl accepts /clang:-std=c23 or /clang:-std=c2x; cl doesn't support a C std switch.
                    if (cc == "clang-cl") {
                        cmd.push_back("/clang:-std=c23");
                    }
                    else if (cc == "cl") {
                        // No dedicated switch; encourage the user in logs if echo enabled.
                        // We still proceed without a /std flag (MSVC doesn't expose one for C).
                    }
                }
                else {
                    // clang/gcc
                    cmd.push_back("-std=c23");
                }
            }
        } // namespace cs_c23_support

        // 2) Replacement build_cmd that honors C23 when enabled via flag or directive.
        //    To activate globally, replace existing build_cmd with this one (same signature), or
        //    call build_cmd_c23(...) at call sites.
        static std::string build_cmd_c23(const Config& cfg, const std::string& cc, const std::string& cpath, const std::string& out,
            bool defineProfile = false,
            const std::string& src_for_scan = std::string()) {
            using namespace cs_c23_support;

            // Decide whether C23 should be on:
            // Priority: CLI/env side-channel > @c23 directive > off
            bool c23_on = g_c23_on || scan_c23_directive(src_for_scan, /*deflt*/ false);

            std::vector<std::string> cmd; cmd.push_back(cc);
            bool msvc = (cc == "cl" || cc == "clang-cl");

            if (msvc) {
                cmd.push_back("/nologo");
                if (cfg.opt == "O0") cmd.push_back("/Od");
                else if (cfg.opt == "O1") cmd.push_back("/O1");
                else cmd.push_back("/O2");

                if (cfg.hardline || cfg.strict) { cmd.push_back("/Wall"); cmd.push_back("/WX"); }
                if (cfg.lto) cmd.push_back("/GL");
                if (cfg.hardline) cmd.push_back("/DCS_HARDLINE=1");
                if (defineProfile) cmd.push_back("/DCS_PROFILE_BUILD=1");
                for (auto& d : cfg.defines) cmd.push_back("/D" + d);
                for (auto& p : cfg.incs)    cmd.push_back("/I" + p);

                // C23 std flag (if enabled and tool supports it)
                if (c23_on) add_c23_std_flag(cc, cmd, /*msvc*/true);

                cmd.push_back(cpath);
                cmd.push_back("/Fe:" + out);
                for (auto& lp : cfg.libpaths) cmd.push_back("/link /LIBPATH:\"" + lp + "\"");
                for (auto& l : cfg.links) {
                    std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib";
                    cmd.push_back("/link " + lib);
                }
            }
            else {
                // Prefer full C23 if requested; otherwise keep legacy selection.
                if (c23_on) {
                    add_c23_std_flag(cc, cmd, /*msvc*/false);
                }
                else {
                    // legacy mode
                    cmd.push_back("-std=c11");
                }

                if (cfg.opt == "O0") cmd.push_back("-O0");
                else if (cfg.opt == "O1") cmd.push_back("-O1");
                else if (cfg.opt == "O2") cmd.push_back("-O2");
                else if (cfg.opt == "O3") cmd.push_back("-O3");
                else if (cfg.opt == "size") cmd.push_back("-Os");
                else if (cfg.opt == "max") { cmd.push_back("-O3"); if (cfg.lto) cmd.push_back("-flto"); }

                if (cfg.hardline) {
                    cmd.push_back("-Wall"); cmd.push_back("-Wextra"); cmd.push_back("-Werror");
                    cmd.push_back("-Wconversion"); cmd.push_back("-Wsign-conversion");
                }
                if (cfg.lto) cmd.push_back("-flto");
                if (cfg.hardline) cmd.push_back("-DCS_HARDLINE=1");
                if (defineProfile) cmd.push_back("-DCS_PROFILE_BUILD=1");
                for (auto& d : cfg.defines) { cmd.push_back("-D" + d); }
                for (auto& p : cfg.incs) { cmd.push_back("-I" + p); }
                cmd.push_back(cpath);
                cmd.push_back("-o"); cmd.push_back(out);
                for (auto& lp : cfg.libpaths) { cmd.push_back("-L" + lp); }
                for (auto& l : cfg.links) { cmd.push_back("-l" + l); }
            }

            std::string full;
            for (size_t i = 0; i < cmd.size(); ++i) {
                if (i) full += ' ';
                bool needQ = cmd[i].find(' ') != std::string::npos;
                if (needQ) full.push_back('"');
                full += cmd[i];
                if (needQ) full.push_back('"');
            }
            return full;
        }

        // 3) Tiny helpers to toggle C23 from CLI. Call these early in main() if desired.
        //    If you prefer not to touch main(), you can still use build_cmd_c23 wherever you build.
        static void cs_enable_c23() { cs_c23_support::g_c23_on = true; }
        static void cs_disable_c23() { cs_c23_support::g_c23_on = false; }

        // 4) Optional: prelude addendum injection. When you build the final C source string,
        //    just append this once right after prelude() when C23 is requested.
        static std::string prelude_c23_addendum() {
            return cs_c23_support::c23_prelude_addendum();
        }

        // 5) Minimal wiring guide (non-invasive):
        //    - To turn on C23 via CLI: detect flags and call cs_enable_c23().
        //    - Use build_cmd_c23 instead of build_cmd where you build (pass the lowered C text in src_for_scan
        //      so @c23 on in the .csc file also works):
        //          std::string cmd = build_cmd_c23(cfg, cc, cpath, out, profileBuild, /*src_for_scan*/ c_src);
        //
        //    - If you want a guard macro in the generated translation unit, add at emission time:
        //          std::string csrc = prelude(cfg.hardline);
        //          if (cs_c23_support::g_c23_on) csrc += prelude_c23_addendum();
        //          csrc += "\n"; csrc += lowered;
        //
        //    - To allow @c23 on in the .csc body without CLI, keep passing the body/c_src to build_cmd_c23.

// ============================ C-Script Full Support Pack (append-only) ============================
// Conservative, order-stable source lowerings to broaden C-Script coverage without
// touching existing code. Apply before softline_lower in the pipeline.
//
// Features added:
//  - struct! Name { ... }           -> typedef struct Name { ... } Name;
//  - type Name = Existing;          -> typedef Existing Name;
//  - new Type{ ... }                -> (Type){ ... }            (C compound literal)
//  - Keywords: ret/and/or/not/null  -> return/&&/||/!/NULL
// Notes:
//  - These are regex-based and assume normal code (not inside strings/comments).
//  - Designed to run after enum! lowering and @unsafe lowering, before softline_lower.

        namespace cs_fullscript {

            using namespace cs_regex_wrap;

            // struct! Name { fields } -> typedef struct Name { fields } Name;
            static std::string lower_struct_bang(const std::string& src) {
                std::string s = src, out; out.reserve(s.size() + s.size() / 16);
                std::regex re(R"(struct!\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string name = m[1].str();
                    const std::string body = m[2].str();
                    out += "typedef struct " + name + " { " + body + " } " + name + ";";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // type Name = Existing; -> typedef Existing Name;
            static std::string lower_type_alias(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex re(R"(\btype\s+([A-Za-z_]\w*)\s*=\s*([^;]+);)");
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string name = trim(m[1].str());
                    const std::string rhs = trim(m[2].str());
                    out += "typedef " + rhs + " " + name + ";";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // new Type{ ... }  -> (Type){ ... }
            // Also supports nested spaces: new   Vec3 {1,2,3}
            static std::string lower_new_compound(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex re(R"(\bnew\s+([A-Za-z_]\w*)\s*\{\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // The above would match 'new T{...})' (common when used in an argument).
                // Add a gentler variant that does not require trailing ')'.
                std::regex re2(R"(\bnew\s+([A-Za-z_]\w*)\s*\{\s*([\s\S]*?)\s*\})", std::regex::ECMAScript);

                cmatch m; size_t pos = 0; bool any = false;
                while (search_from(s, pos, m, re)) {
                    any = true;
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "((" + m[1].str() + "){" + m[2].str() + "})";
                }
                if (!any) { // fallback simple form
                    pos = 0; out.clear(); out.reserve(s.size());
                    while (search_from(s, pos, m, re2)) {
                        append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                        out += "((" + m[1].str() + "){" + m[2].str() + "})";
                    }
                    out.append(s, pos, std::string::npos);
                    return out;
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // Keyword sugar:
            //  ret  -> return
            //  and  -> &&
            //  or   -> ||
            //  not  -> !
            //  null -> NULL
            static std::string lower_keywords(const std::string& src) {
                std::string s = src;
                s = std::regex_replace(s, std::regex(R"(\bret\b)"), "return");
                s = std::regex_replace(s, std::regex(R"(\band\b)"), "&&");
                s = std::regex_replace(s, std::regex(R"(\bor\b)"), "||");
                s = std::regex_replace(s, std::regex(R"(\bnot\b)"), "!");
                s = std::regex_replace(s, std::regex(R"(\bnull\b)"), "NULL");
                return s;
            }

            // Apply in a stable order
            static std::string apply_all(const std::string& src) {
                std::string t = lower_keywords(src);
                t = lower_type_alias(t);
                t = lower_struct_bang(t);
                t = lower_new_compound(t);
                return t;
            }

            // Optional: add a tiny prelude addendum for bool/null portability.
            static std::string prelude_addendum() {
                return R"(
#ifndef CS_PRELUDE_CSCRIPT_EXTRAS
#define CS_PRELUDE_CSCRIPT_EXTRAS 1
#include <stdbool.h>
#ifndef null
#define null NULL
#endif
#endif
)";
            }
        } // namespace cs_fullscript

// ============================ Architecture & Bitness Support Pack (append-only) ============================
// Full-bitwidth (8/16/32/64) and arch support (x86, x86-64, and common microcontrollers) for generated C.
// Features:
//   - Directives in .csc:  @arch x86|x64|x86-64|avr|msp430|armv7|aarch64
//                          @bits 8|16|32|64
//                          @target "triple"   (e.g., "avr", "x86_64-w64-windows-gnu", "armv7-none-eabi")
//                          @mcpu "cpu"        (e.g., "cortex-m3", "znver4")
//                          @mcu  "name"       (e.g., "atmega328p")
//                          @endian little|big
//   - CLI helpers (opt-in in your main parser): cs_set_arch(...), cs_set_bits(...), cs_set_target(...), cs_set_mcpu(...),
//     cs_set_mcu(...), cs_set_endian(...).
//   - Drop-in build: use build_cmd_arch(...) instead of build_cmd(...).
//   - Adds portable macros into the TU: CS_BITS, CS_WORD_BITS, CS_PTR_BITS, CS_ENDIAN_{LITTLE|BIG}, CS_ARCH_*.
//
// Notes:
//   - This is conservative and portable: for GCC/Clang we prefer --target and -m32/-m64/-m16; for clang-cl we pass
//     /clang:-m32 etc. For baremetal MCUs (AVR/MSP430), --target and -mmcu are used when available.
//   - If a directive is not provided, we do not force a target; we still emit CS_* auto-detection in the prelude addendum.

        namespace cs_arch_support {

            struct ArchSpec {
                std::string arch;    // x86 | x64 | x86-64 | avr | msp430 | armv7 | aarch64
                int         bits = 0; // 8 | 16 | 32 | 64 (0 means auto)
                std::string target; // clang/gcc --target triple
                std::string mcpu;   // -mcpu=<cpu>
                std::string mcu;    // -mmcu=<mcu> (AVR etc)
                std::string endian; // "little" | "big" | "" (auto)
            };

            static ArchSpec g_spec;

            static std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");
                return s.substr(a, b - a + 1);
            }

            // Scan .csc body for @arch/@bits/@target/@mcpu/@mcu/@endian directives.
            static ArchSpec scan_arch_directives(const std::string& text) {
                ArchSpec s = g_spec; // user CLI can pre-fill globals; directives override
                std::istringstream ss(text);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = trim(line);
                    if (t.empty() || t[0] != '@') continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;

                    auto readQuoted = [&](std::string& out) {
                        if (!ls.good()) return;
                        if (ls.peek() == '"') {
                            ls >> std::quoted(out);
                        }
                        else {
                            ls >> out;
                        }
                        };

                    if (name == "arch") { readQuoted(s.arch); }
                    else if (name == "bits") {
                        std::string v; readQuoted(v);
                        if (!v.empty()) s.bits = std::atoi(v.c_str());
                    }
                    else if (name == "target") { readQuoted(s.target); }
                    else if (name == "mcpu") { readQuoted(s.mcpu); }
                    else if (name == "mcu") { readQuoted(s.mcu); }
                    else if (name == "endian") { readQuoted(s.endian); }
                }
                return s;
            }

            // Public CLI helpers (call from your argument parser if desired)
            static void cs_set_arch(const std::string& arch) { g_spec.arch = arch; }
            static void cs_set_bits(int bits) { g_spec.bits = bits; }
            static void cs_set_target(const std::string& triple) { g_spec.target = triple; }
            static void cs_set_mcpu(const std::string& cpu) { g_spec.mcpu = cpu; }
            static void cs_set_mcu(const std::string& mcu) { g_spec.mcu = mcu; }
            static void cs_set_endian(const std::string& e) { g_spec.endian = e; }

            // Normalize arch aliases
            static std::string norm_arch(std::string a) {
                for (auto& c : a) c = (char)tolower((unsigned char)c);
                if (a == "x86-64") a = "x64";
                if (a == "x86_64") a = "x64";
                if (a == "i386" || a == "i486" || a == "i586" || a == "i686") a = "x86";
                return a;
            }

            // Compose macros to advertise arch/bits to the TU.
            static void add_define(std::vector<std::string>& cmd, bool msvc, const std::string& name, const std::string& val = "1") {
                if (msvc) cmd.push_back("/D" + name + "=" + val);
                else      cmd.push_back("-D" + name + "=" + val);
            }

            // Add flags (-m32/-m64/--target/-mcpu/-mmcu/-march) + CS_* defines
            static void add_arch_flags_and_defines(const ArchSpec& s, const std::string& cc, bool msvc, std::vector<std::string>& cmd) {
                const std::string A = norm_arch(s.arch);

                // Inject --target (clang/clang-cl) when provided
                auto push_target = [&](const std::string& t) {
                    if (t.empty()) return;
                    if (msvc) cmd.push_back("/clang:--target=" + t);
                    else      cmd.push_back("--target=" + t);
                    };

                auto push_mcpu = [&](const std::string& cpu) {
                    if (cpu.empty()) return;
                    if (msvc) cmd.push_back("/clang:-mcpu=" + cpu);
                    else      cmd.push_back("-mcpu=" + cpu);
                    };

                auto push_mmcu = [&](const std::string& mcu) {
                    if (mcu.empty()) return;
                    if (msvc) cmd.push_back("/clang:-mmcu=" + mcu);
                    else      cmd.push_back("-mmcu=" + mcu);
                    };

                auto push_march = [&](const std::string& march) {
                    if (march.empty()) return;
                    if (msvc) cmd.push_back("/clang:-march=" + march);
                    else      cmd.push_back("-march=" + march);
                    };

                // -m32/-m64/-m16 where applicable
                auto push_mode = [&](int bits) {
                    if (bits <= 0) return;
                    if (msvc) {
                        if (bits == 32) cmd.push_back("/clang:-m32");
                        else if (bits == 64) cmd.push_back("/clang:-m64");
                        else if (bits == 16) cmd.push_back("/clang:-m16"); // clang-cl supports passing through
                    }
                    else {
                        if (bits == 32) cmd.push_back("-m32");
                        else if (bits == 64) cmd.push_back("-m64");
                        else if (bits == 16) cmd.push_back("-m16"); // limited toolchain support; best-effort
                    }
                    };

                // Target-specific handling
                if (!s.target.empty()) push_target(s.target);

                if (A == "x86") {
                    add_define(cmd, msvc, "CS_ARCH_X86");
                    if (s.bits) push_mode(s.bits); else push_mode(32);
                }
                else if (A == "x64") {
                    add_define(cmd, msvc, "CS_ARCH_X64");
                    if (s.bits) push_mode(s.bits); else push_mode(64);
                }
                else if (A == "avr") {
                    add_define(cmd, msvc, "CS_ARCH_AVR");
                    if (s.target.empty()) push_target("avr");
                    push_mmcu(s.mcu);
                    if (s.bits == 0) add_define(cmd, msvc, "CS_BITS", "8");
                }
                else if (A == "msp430") {
                    add_define(cmd, msvc, "CS_ARCH_MSP430");
                    if (s.target.empty()) push_target("msp430");
                    if (s.bits == 0) add_define(cmd, msvc, "CS_BITS", "16");
                }
                else if (A == "armv7") {
                    add_define(cmd, msvc, "CS_ARCH_ARMV7");
                    push_march("armv7");
                    push_mcpu(s.mcpu);
                    if (s.bits == 0) add_define(cmd, msvc, "CS_BITS", "32");
                }
                else if (A == "aarch64") {
                    add_define(cmd, msvc, "CS_ARCH_AARCH64");
                    if (s.bits == 0) add_define(cmd, msvc, "CS_BITS", "64");
                    if (!s.target.empty()) {
                        // already pushed
                    }
                    else {
                        // leave to host default or explicit --target by user
                    }
                    push_mcpu(s.mcpu);
                }
                else if (!A.empty()) {
                    // Unknown user arch tag – still advertise it
                    add_define(cmd, msvc, "CS_ARCH_CUSTOM");
                    add_define(cmd, msvc, "CS_ARCH_NAME", "\"" + A + "\"");
                }

                // Bits preference
                if (s.bits > 0) add_define(cmd, msvc, "CS_BITS", std::to_string(s.bits));

                // Endianness preference
                if (!s.endian.empty()) {
                    std::string e = s.endian;
                    for (auto& c : e) c = (char)tolower((unsigned char)c);
                    if (e == "little") add_define(cmd, msvc, "CS_ENDIAN_LITTLE");
                    else if (e == "big") add_define(cmd, msvc, "CS_ENDIAN_BIG");
                }

                // Universal derived defines (if CS_BITS given)
                // CS_WORD_BITS == CS_BITS by default; pointers assumed CS_BITS unless overridden by target
                // Provide CS_PTR_BITS as alias (some users prefer explicit pointer width)
                // Note: if not set here, the prelude addendum will compute from sizeof(void*)
                if (s.bits > 0) {
                    add_define(cmd, msvc, "CS_WORD_BITS", std::to_string(s.bits));
                    add_define(cmd, msvc, "CS_PTR_BITS", std::to_string(s.bits));
                }
            }

            // Replacement build_cmd that honors arch/bitness directives or CLI side-channel (g_spec).
            static std::string build_cmd_arch(const Config& cfg,
                const std::string& cc,
                const std::string& cpath,
                const std::string& out,
                bool defineProfile = false,
                const std::string& src_for_scan = std::string()) {
                std::vector<std::string> cmd; cmd.push_back(cc);
                bool msvc = (cc == "cl" || cc == "clang-cl");

                // Copy of baseline behavior, with -std and opts preserved, then augment with arch flags.
                auto push = [&](const std::string& s) { cmd.push_back(s); };

                if (msvc) {
                    push("/nologo");
                    if (cfg.opt == "O0") push("/Od");
                    else if (cfg.opt == "O1") push("/O1");
                    else push("/O2");
                    if (cfg.hardline || cfg.strict) { push("/Wall"); push("/WX"); }
                    if (cfg.lto) push("/GL");
                    if (cfg.hardline) push("/DCS_HARDLINE=1");
                    if (defineProfile) push("/DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) push("/D" + d);
                    for (auto& p : cfg.incs)    push("/I" + p);

                    // Arch flags/defines
                    ArchSpec spec = src_for_scan.empty() ? g_spec : scan_arch_directives(src_for_scan);
                    add_arch_flags_and_defines(spec, cc, /*msvc*/true, cmd);

                    push(cpath);
                    push("/Fe:" + out);
                    for (auto& lp : cfg.libpaths) push("/link /LIBPATH:\"" + lp + "\"");
                    for (auto& l : cfg.links) {
                        std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib";
                        push("/link " + lib);
                    }
                }
                else {
                    // Standard selection: keep legacy C mode (caller may have swapped in C23 pack)
                    push("-std=c11");
                    if (cfg.opt == "O0") push("-O0");
                    else if (cfg.opt == "O1") push("-O1");
                    else if (cfg.opt == "O2") push("-O2");
                    else if (cfg.opt == "O3") push("-O3");
                    else if (cfg.opt == "size") push("-Os");
                    else if (cfg.opt == "max") { push("-O3"); if (cfg.lto) push("-flto"); }

                    if (cfg.hardline) {
                        push("-Wall"); push("-Wextra"); push("-Werror");
                        push("-Wconversion"); push("-Wsign-conversion");
                    }
                    if (cfg.lto) push("-flto");
                    if (cfg.hardline) push("-DCS_HARDLINE=1");
                    if (defineProfile) push("-DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) push("-D" + d);
                    for (auto& p : cfg.incs)    push("-I" + p);

                    // Arch flags/defines
                    ArchSpec spec = src_for_scan.empty() ? g_spec : scan_arch_directives(src_for_scan);
                    add_arch_flags_and_defines(spec, cc, /*msvc*/false, cmd);

                    push(cpath);
                    push("-o"); push(out);
                    for (auto& lp : cfg.libpaths) push("-L" + lp);
                    for (auto& l : cfg.links)      push("-l" + l);
                }

                std::string full;
                for (size_t i = 0; i < cmd.size(); ++i) {
                    if (i) full += ' ';
                    bool needQ = cmd[i].find(' ') != std::string::npos;
                    if (needQ) full.push_back('"');
                    full += cmd[i];
                    if (needQ) full.push_back('"');
                }
                return full;
            }

            // Prelude addendum: derives/validates CS_BITS, endianness, and exposes CS_PTR_BITS/CS_WORD_BITS consistently.
            static std::string prelude_arch_addendum() {
                return R"(
/* --- Arch/Bitness portability addendum --- */
#ifndef CS_BITS
  #define CS_BITS ((int)(sizeof(void*)*8))
#endif
#ifndef CS_PTR_BITS
  #define CS_PTR_BITS CS_BITS
#endif
#ifndef CS_WORD_BITS
  #define CS_WORD_BITS CS_BITS
#endif

/* Endianness auto-detect (overridden if CS_ENDIAN_LITTLE/CS_ENDIAN_BIG defined) */
#if !defined(CS_ENDIAN_LITTLE) && !defined(CS_ENDIAN_BIG)
  #if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
    #if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
      #define CS_ENDIAN_LITTLE 1
    #else
      #define CS_ENDIAN_BIG 1
    #endif
  #elif defined(_WIN32)
    #define CS_ENDIAN_LITTLE 1
  #else
    /* Fallback runtime check (constant-folded by most compilers) */
    static inline int __cs_is_le(void){ union{ unsigned int i; unsigned char b[4]; } u = {1u}; return u.b[0]==1; }
    #define CS_ENDIAN_LITTLE (__cs_is_le())
  #endif
#endif

/* Static sanity checks (C11 _Static_assert) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  _Static_assert(CS_PTR_BITS==8*sizeof(void*), "CS_PTR_BITS must match sizeof(void*)");
  _Static_assert(CS_WORD_BITS == CS_BITS, "CS_WORD_BITS defaults to CS_BITS unless overridden");
#endif
      )";
            }

        } // namespace cs_arch_support

// ============================ Vectorization Support Pack (append-only) ============================
// Complete, portable vectorization wiring for auto-SIMD and explicit vector types.
//
// .csc directives:
//   @vectorize on|off         // enable/disable auto-vectorizer and SLP
//   @vecwidth 128|256|512     // preferred SIMD width (maps to -mavx2/-mavx512 etc. when possible)
//   @fastmath on|off          // enables fast-math flags (dangerous: UB on NaNs/Inf/rounding)
//   @features "+avx2,+fma"    // ISA feature string or "native"
//   @unroll N                 // unroll hint for the next loop
//
// Optional CLI helpers you can call in main(): cs_vec_enable/disable, cs_vec_set_width(...), cs_vec_fastmath(...), cs_vec_set_features(...)
//
// Wire-up (minimal):
//   - Use build_cmd_vec(...) instead of build_cmd(...) at both build call sites (pass the generated C for directive scan).
//   - Apply vector-loop hints before softline_lower:
//       auto hinted = cs_vectorization::lower_vector_hints(matchLowered);
//       string lowered = softline_lower(hinted, cfg.softline, hotFns, false);
//   - Optionally append the prelude addendum once:
//       csrc += cs_vectorization::prelude_vec_addendum(cs_vectorization::effective_width_from_src(unsafeLowered));
//
        namespace cs_vectorization {

            struct VecSpec {
                bool on = true;
                int  width = 0;            // 0=auto, 128/256/512
                bool fastmath = false;
                std::string features;      // "+avx2,+fma" or "native" or ""
            };
            static VecSpec g_vec;

            static std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");  return s.substr(a, b - a + 1);
            }

            // Parse @vectorize/@vecwidth/@fastmath/@features/@unroll
            static VecSpec scan_vec_directives(const std::string& text) {
                VecSpec v = g_vec;
                std::istringstream ss(text); std::string line;
                while (std::getline(ss, line)) {
                    std::string t = trim(line);
                    if (t.empty() || t[0] != '@') continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    if (name == "vectorize") { std::string val; ls >> val; v.on = (val != "off"); }
                    else if (name == "vecwidth") { std::string w; ls >> w; v.width = std::atoi(w.c_str()); }
                    else if (name == "fastmath") { std::string val; ls >> val; v.fastmath = (val != "off"); }
                    else if (name == "features") { if (ls.peek() == '"') ls >> std::quoted(v.features); else ls >> v.features; }
                }
                return v;
            }

            // CLI helpers (optional)
            static void cs_vec_enable(bool on = true) { g_vec.on = on; }
            static void cs_vec_disable() { g_vec.on = false; }
            static void cs_vec_set_width(int w) { g_vec.width = w; }
            static void cs_vec_fastmath(bool on = true) { g_vec.fastmath = on; }
            static void cs_vec_set_features(const std::string& f) { g_vec.features = f; }

            // Compose defines for TU visibility
            static void add_define(std::vector<std::string>& cmd, bool msvc, const std::string& name, const std::string& val = "1") {
                if (msvc) cmd.push_back("/D" + name + "=" + val);
                else      cmd.push_back("-D" + name + "=" + val);
            }

            // Effective width from text (public utility)
            static int effective_width_from_src(const std::string& src) {
                VecSpec v = scan_vec_directives(src);
                return v.width ? v.width : 0;
            }

            // Flag injection for compilers
            static void add_vec_flags(const VecSpec& v, const std::string& cc, bool msvc, std::vector<std::string>& cmd) {
                // Common toggles
                if (msvc) {
                    // MSVC/clang-cl: vectorizer on with /O2 by default; steer ISA width via /arch or /clang:-m*
                    if (cc == "clang-cl") {
                        if (v.width == 512) cmd.push_back("/clang:-mavx512f");
                        else if (v.width == 256) cmd.push_back("/clang:-mavx2");
                        else if (v.width == 128) cmd.push_back("/clang:-msse2");
                        if (!v.features.empty()) cmd.push_back("/clang:-march=" + v.features); // "native" or cpu/feature string
                        if (!v.on) cmd.push_back("/clang:-fno-vectorize"), cmd.push_back("/clang:-fno-slp-vectorize");
                        if (v.on)  cmd.push_back("/clang:-fvectorize"), cmd.push_back("/clang:-fslp-vectorize");
                        if (v.fastmath) cmd.push_back("/clang:-ffast-math");
                    }
                    else { // cl
                        // Best-effort ISA
                        if (v.width == 512) cmd.push_back("/arch:AVX512");
                        else if (v.width == 256) cmd.push_back("/arch:AVX2");
                        else if (v.width == 128) cmd.push_back("/arch:SSE2");
                        // Fast-math
                        if (v.fastmath) cmd.push_back("/fp:fast");
                        // No direct f(vectorize) toggles beyond /O2; leave as-is for cl.
                    }
                }
                else {
                    // Clang/GCC
                    // Enable/disable vectorizers
                    if (v.on) {
                        cmd.push_back("-fvectorize");
                        cmd.push_back("-fslp-vectorize");
                        // GCC uses -ftree-vectorize (on at -O3, but force on):
                        cmd.push_back("-ftree-vectorize");
                    }
                    else {
                        cmd.push_back("-fno-vectorize");
                        cmd.push_back("-fno-slp-vectorize");
                        cmd.push_back("-fno-tree-vectorize");
                    }
                    // Width/ISA
                    if (v.width == 512) cmd.push_back("-mavx512f");
                    else if (v.width == 256) cmd.push_back("-mavx2");
                    else if (v.width == 128) cmd.push_back("-msse2");
                    // Features / cpu
                    if (!v.features.empty()) {
                        if (v.features == "native") cmd.push_back("-march=native");
                        else cmd.push_back("-march=" + v.features);
                    }
                    // Fast-math (opt-in: may change FP semantics)
                    if (v.fastmath) {
                        cmd.push_back("-ffast-math");
                        cmd.push_back("-fno-trapping-math");
                    }
                }

                // Defines visible to generated C
                add_define(cmd, msvc, "CS_VEC", v.on ? "1" : "0");
                if (v.width) add_define(cmd, msvc, "CS_VEC_WIDTH", std::to_string(v.width));
                if (v.fastmath) add_define(cmd, msvc, "CS_VEC_FASTMATH", "1");
            }

            // Replacement build command with vectorization enabled (drop-in for build_cmd)
            static std::string build_cmd_vec(const Config& cfg,
                const std::string& cc,
                const std::string& cpath,
                const std::string& out,
                bool defineProfile = false,
                const std::string& src_for_scan = std::string()) {
                std::vector<std::string> cmd; cmd.push_back(cc);
                auto push = [&](const std::string& s) { cmd.push_back(s); };
                bool msvc = (cc == "cl" || cc == "clang-cl");

                // Base (copy of current behavior)
                if (msvc) {
                    push("/nologo");
                    if (cfg.opt == "O0") push("/Od");
                    else if (cfg.opt == "O1") push("/O1");
                    else push("/O2");
                    if (cfg.hardline || cfg.strict) { push("/Wall"); push("/WX"); }
                    if (cfg.lto) push("/GL");
                    if (cfg.hardline) push("/DCS_HARDLINE=1");
                    if (defineProfile) push("/DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) push("/D" + d);
                    for (auto& p : cfg.incs)    push("/I" + p);

                    // Vector flags
                    add_vec_flags(scan_vec_directives(src_for_scan), cc, /*msvc*/true, cmd);

                    push(cpath);
                    push("/Fe:" + out);
                    for (auto& lp : cfg.libpaths) push("/link /LIBPATH:\"" + lp + "\"");
                    for (auto& l : cfg.links) { std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib"; push("/link " + lib); }
                }
                else {
                    // Standard C mode as in current code (caller may have C23 pack)
                    push("-std=c11");
                    if (cfg.opt == "O0") push("-O0");
                    else if (cfg.opt == "O1") push("-O1");
                    else if (cfg.opt == "O2") push("-O2");
                    else if (cfg.opt == "O3") push("-O3");
                    else if (cfg.opt == "size") push("-Os");
                    else if (cfg.opt == "max") { push("-O3"); if (cfg.lto) push("-flto"); }
                    if (cfg.hardline) { push("-Wall"); push("-Wextra"); push("-Werror"); push("-Wconversion"); push("-Wsign-conversion"); }
                    if (cfg.lto) push("-flto");
                    if (cfg.hardline) push("-DCS_HARDLINE=1");
                    if (defineProfile) push("-DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) push("-D" + d);
                    for (auto& p : cfg.incs)    push("-I" + p);

                    // Vector flags
                    add_vec_flags(scan_vec_directives(src_for_scan), cc, /*msvc*/false, cmd);

                    push(cpath);
                    push("-o"); push(out);
                    for (auto& lp : cfg.libpaths) push("-L" + lp);
                    for (auto& l : cfg.links)      push("-l" + l);
                }

                std::string full;
                for (size_t i = 0; i < cmd.size(); ++i) {
                    if (i) full += ' ';
                    bool needQ = cmd[i].find(' ') != std::string::npos;
                    if (needQ) full.push_back('"');
                    full += cmd[i];
                    if (needQ) full.push_back('"');
                }
                return full;
            }

            // Lowering: insert loop-vectorization pragmas from lightweight annotations:
            //  - @vectorize on         -> injects pragmas that enable vectorization for the immediately following loop
            //  - @unroll N             -> injects unroll pragmas for the immediately following loop
            static std::string lower_vector_hints(const std::string& src) {
                std::string s = src;
                // Replace "@vectorize on" with a pair of pragmas commonly respected by Clang/GCC/MSVC
                {
                    std::regex re(R"(@vectorize\s+on\b)");
                    s = std::regex_replace(s, re,
                        "#pragma clang loop vectorize(enable)\n#pragma GCC ivdep\n#pragma loop(ivdep)\n");
                }
                {
                    std::regex re(R"(@vectorize\s+off\b)");
                    s = std::regex_replace(s, re,
                        "#pragma clang loop vectorize(disable)\n#pragma GCC novector\n#pragma loop(no_vector)\n");
                }
                // @unroll N
                {
                    std::regex re(R"(@unroll\s+([0-9]+))");
                    s = std::regex_replace(s, re,
                        "#pragma clang loop unroll_count(\\1)\n#pragma GCC unroll \\1\n#pragma loop(unroll(\\1))\n");
                }
                return s;
            }

            // Prelude addendum: portable vector typedefs for 128/256/512 lanes and helpers.
            static std::string prelude_vec_addendum(int preferredWidth) {
                // choose max width to expose. If 0, default to 128.
                int W = (preferredWidth == 256 || preferredWidth == 512) ? preferredWidth : 128;
                std::ostringstream o;
                o << "/* --- Vectorization addendum (portable typedefs) --- */\n";
                o << "#if defined(__clang__) || defined(__GNUC__)\n";
                // 128-bit
                o << "typedef unsigned char  v16u8  __attribute__((vector_size(16)));\n";
                o << "typedef unsigned short v8u16  __attribute__((vector_size(16)));\n";
                o << "typedef unsigned int   v4u32  __attribute__((vector_size(16)));\n";
                o << "typedef unsigned long long v2u64 __attribute__((vector_size(16)));\n";
                o << "typedef float          v4f32  __attribute__((vector_size(16)));\n";
                o << "typedef double         v2f64  __attribute__((vector_size(16)));\n";
                if (W >= 256) {
                    o << "typedef unsigned char  v32u8  __attribute__((vector_size(32)));\n";
                    o << "typedef unsigned short v16u16 __attribute__((vector_size(32)));\n";
                    o << "typedef unsigned int   v8u32  __attribute__((vector_size(32)));\n";
                    o << "typedef unsigned long long v4u64 __attribute__((vector_size(32)));\n";
                    o << "typedef float          v8f32  __attribute__((vector_size(32)));\n";
                    o << "typedef double         v4f64  __attribute__((vector_size(32)));\n";
                }
                if (W >= 512) {
                    o << "typedef unsigned char  v64u8  __attribute__((vector_size(64)));\n";
                    o << "typedef unsigned short v32u16 __attribute__((vector_size(64)));\n";
                    o << "typedef unsigned int   v16u32 __attribute__((vector_size(64)));\n";
                    o << "typedef unsigned long long v8u64 __attribute__((vector_size(64)));\n";
                    o << "typedef float          v16f32 __attribute__((vector_size(64)));\n";
                    o << "typedef double         v8f64  __attribute__((vector_size(64)));\n";
                }
                o << "#define VEC_ADD(a,b) ((a)+(b))\n";
                o << "#define VEC_SUB(a,b) ((a)-(b))\n";
                o << "#define VEC_MUL(a,b) ((a)*(b))\n";
                o << "#define VEC_AND(a,b) ((a)&(b))\n";
                o << "#define VEC_OR(a,b)  ((a)|(b))\n";
                o << "#define VEC_XOR(a,b) ((a)^(b))\n";
                o << "#define VEC_MIN(a,b) __builtin_elementwise_min((a),(b))\n";
                o << "#define VEC_MAX(a,b) __builtin_elementwise_max((a),(b))\n";
                o << "#elif defined(_MSC_VER)\n";
                o << "#  include <immintrin.h>\n";
                // Minimal typedef exposure for MSVC (use intrinsics directly)
                o << "typedef __m128  v4f32; typedef __m128d v2f64; typedef __m128i v16u8;\n";
                if (W >= 256) o << "typedef __m256  v8f32; typedef __m256d v4f64; typedef __m256i v32u8;\n";
                if (W >= 512) o << "typedef __m512  v16f32; typedef __m512d v8f64; typedef __m512i v64u8;\n";
                o << "#define VEC_ADD(a,b) /* use intrinsics: _mm_add_ps/_mm256_add_ps */ (a)\n";
                o << "#define VEC_SUB(a,b) (a)\n";
                o << "#define VEC_MUL(a,b) (a)\n";
                o << "#define VEC_AND(a,b) (a)\n";
                o << "#define VEC_OR(a,b)  (a)\n";
                o << "#define VEC_XOR(a,b) (a)\n";
                o << "#define VEC_MIN(a,b) (a)\n";
                o << "#define VEC_MAX(a,b) (a)\n";
                o << "#else\n";
                o << "/* No vector extension available; typedefs omitted. */\n";
                o << "#endif\n";
                return o.str();
            }

        } // namespace cs_vectorization

		// ============================ End of Vectorization Support Pack ============================

// ============================ Supreme Shader Support Pack (append-only) ============================
// Parse, compile, and embed shaders from .csc sources as binary arrays (SPIR-V/DXIL).
// Non-invasive: call extract/compile and append the emitted arrays into the C TU before building.
//
// Supported blocks:
//   1) shader! Name(stage=frag, lang=glsl, entry=main) { ... }
//   2) @shader Name stage=frag lang=glsl entry=main
//        ... shader text ...
//      @endshader
//
// Attributes:
//   - stage: vert|frag|comp|geom|tesc|tese
//   - lang:  glsl|hlsl|wgsl
//   - entry: entry-point symbol (default: "main")
//   - profile: e.g., ps_6_0 / vs_6_0 for HLSL
//   - features: ISA string, forwarded to tool if applicable
//
// External tools (auto-detected):
//   - glslangValidator, glslc, dxc, tint (best-effort). If not found, shader is emitted as UTF-8 source bytes.

        namespace cs_supreme {

            struct ShaderSpec {
                std::string name;
                std::string stage;   // vert/frag/comp/geom/tesc/tese
                std::string lang;    // glsl/hlsl/wgsl
                std::string entry = "main";
                std::string profile; // ps_6_0, vs_6_0...
                std::string features;
                std::string source;  // raw shader text
            };

            struct CompiledShader {
                ShaderSpec spec;
                std::string format;  // "spirv" | "dxil" | "raw"
                std::vector<unsigned char> bytes;
            };

            // Trim helpers
            static inline std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");
                return s.substr(a, b - a + 1);
            }
            static inline std::string lcase(std::string s) {
                for (auto& c : s) c = (char)tolower((unsigned char)c);
                return s;
            }

            // Parse "k=v" attributes separated by commas/spaces
            static std::map<std::string, std::string> parse_attrs(const std::string& attrs) {
                std::map<std::string, std::string> m;
                std::string tok, key, val;
                auto flush = [&]() {
                    std::string t = trim(tok); tok.clear();
                    if (t.empty()) return;
                    size_t eq = t.find('=');
                    if (eq == std::string::npos) { m[lcase(t)] = "1"; return; }
                    key = lcase(trim(t.substr(0, eq)));
                    val = trim(t.substr(eq + 1));
                    if (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
                        char q = val.front(); if (val.back() == q && val.size() >= 2) val = val.substr(1, val.size() - 2);
                    }
                    m[key] = val;
                    };
                for (char c : attrs) {
                    if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') { flush(); }
                    else tok.push_back(c);
                }
                flush();
                return m;
            }

            // Extract shaders from source (both forms).
            static std::vector<ShaderSpec> extract_shaders(const std::string& src) {
                using namespace cs_regex_wrap;
                std::vector<ShaderSpec> out;

                // Form 1: shader! Name(attrs) { body }
                {
                    std::regex re(R"(shader!\s+([A-Za-z_]\w*)\s*(?:\(([^)]*)\))?\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                    cmatch m; size_t pos = 0;
                    while (search_from(src, pos, m, re)) {
                        ShaderSpec s;
                        s.name = trim(m[1].str());
                        auto attrs = parse_attrs(m[2].str());
                        s.stage = lcase(attrs.count("stage") ? attrs["stage"] : "");
                        s.lang = lcase(attrs.count("lang") ? attrs["lang"] : "");
                        if (attrs.count("entry"))   s.entry = attrs["entry"];
                        if (attrs.count("profile")) s.profile = attrs["profile"];
                        if (attrs.count("features")) s.features = attrs["features"];
                        s.source = m[3].str();
                        if (s.stage.empty() || s.lang.empty()) {
                            warn("shader! " + s.name + ": missing stage/lang; will embed raw text");
                        }
                        out.push_back(std::move(s));
                    }
                }

                // Form 2: @shader Name k=v ... newline ... @endshader
                {
                    std::regex re(R"(@shader\s+([A-Za-z_]\w*)\s+([^\n]*?)\s*\n([\s\S]*?)@endshader)", std::regex::ECMAScript);
                    cmatch m; size_t pos = 0;
                    while (search_from(src, pos, m, re)) {
                        ShaderSpec s;
                        s.name = trim(m[1].str());
                        auto attrs = parse_attrs(m[2].str());
                        s.stage = lcase(attrs.count("stage") ? attrs["stage"] : "");
                        s.lang = lcase(attrs.count("lang") ? attrs["lang"] : "");
                        if (attrs.count("entry"))   s.entry = attrs["entry"];
                        if (attrs.count("profile")) s.profile = attrs["profile"];
                        if (attrs.count("features")) s.features = attrs["features"];
                        s.source = m[3].str();
                        out.push_back(std::move(s));
                    }
                }

                return out;
            }

            // Tool detection (best-effort)
            enum class ToolKind { None, GLSLANG, GLSLC, DXC, TINT };
            static bool cmd_exists(const std::string& cmd) {
#if defined(_WIN32)
                std::string c = cmd + " --version > NUL 2>&1";
#else
                std::string c = cmd + " --version > /dev/null 2>&1";
#endif
                return system(c.c_str()) == 0;
            }
            static ToolKind pick_tool_for(const std::string& lang) {
                std::string L = lcase(lang);
                if (L == "glsl") {
                    if (cmd_exists("glslangValidator")) return ToolKind::GLSLANG;
                    if (cmd_exists("glslc"))            return ToolKind::GLSLC;
                }
                else if (L == "hlsl") {
                    if (cmd_exists("dxc")) return ToolKind::DXC;
                    // Clang can compile HLSL too, but not assumed here; fall back to raw
                }
                else if (L == "wgsl") {
                    if (cmd_exists("tint")) return ToolKind::TINT;
                }
                return ToolKind::None;
            }

            // Stage mapping helpers
            static const char* stage_to_glslang(const std::string& st) {
                if (st == "vert") return "vert";
                if (st == "frag") return "frag";
                if (st == "comp") return "comp";
                if (st == "geom") return "geom";
                if (st == "tesc") return "tesc";
                if (st == "tese") return "tese";
                return "frag";
            }
            static const char* stage_to_glslc(const std::string& st) {
                // glslc uses -fshader-stage=<stage>
                return stage_to_glslang(st);
            }
            static std::string stage_to_hlsl_profile(const std::string& st, const std::string& profile) {
                if (!profile.empty()) return profile;
                if (st == "vert") return "vs_6_0";
                if (st == "frag") return "ps_6_0";
                if (st == "comp") return "cs_6_0";
                if (st == "geom") return "gs_6_0";
                if (st == "tesc") return "hs_6_0";
                if (st == "tese") return "ds_6_0";
                return "ps_6_0";
            }

            // Run one shader compile, return CompiledShader (or raw fallback)
            static CompiledShader compile_one(const ShaderSpec& s, bool echo = false) {
                CompiledShader out; out.spec = s; out.format = "raw";
                // Write source to temp
                std::string ext = (s.lang == "glsl") ? ".glsl" : (s.lang == "hlsl") ? ".hlsl" : ".wgsl";
                std::string inpath = write_temp("cscript_shader_" + s.name + ext, s.source);
                // Ensure unique out path
#if defined(_WIN32)
                std::string binpath = write_temp("cscript_shader_out_" + s.name + ".bin", "");
#else
                std::string binpath = write_temp("cscript_shader_out_" + s.name + ".bin", "");
#endif
                rm_file(binpath);

                ToolKind tk = pick_tool_for(s.lang);
                int rc = 1;
                std::string cmd;

                if (tk == ToolKind::GLSLANG) {
                    // glslangValidator -V -S frag -o out.spv in.glsl
                    std::string stage = stage_to_glslang(s.stage);
                    std::string spv = binpath + ".spv";
                    cmd = "glslangValidator -V -S " + stage + " -o \"" + spv + "\" \"" + inpath + "\"";
                    rc = system(cmd.c_str());
                    if (rc == 0) { out.format = "spirv"; binpath = spv; }
                }
                else if (tk == ToolKind::GLSLC) {
                    // glslc -fshader-stage=frag -o out.spv in.glsl
                    std::string stage = stage_to_glslc(s.stage);
                    std::string spv = binpath + ".spv";
                    cmd = "glslc -fshader-stage=" + stage + " -o \"" + spv + "\" \"" + inpath + "\"";
                    if (!s.features.empty()) cmd += " -march=" + s.features;
                    rc = system(cmd.c_str());
                    if (rc == 0) { out.format = "spirv"; binpath = spv; }
                }
                else if (tk == ToolKind::DXC) {
                    // Prefer SPIR-V if possible, else DXIL container
                    std::string prof = stage_to_hlsl_profile(s.stage, s.profile);
                    std::string spv = binpath + ".spv";
                    std::string dxil = binpath + ".dxil";
                    if (true) {
                        cmd = "dxc -spirv -E " + (s.entry.empty() ? "main" : s.entry) + " -T " + prof + " -Fo \"" + spv + "\" \"" + inpath + "\"";
                        if (!s.features.empty()) cmd += " -fspv-target-env=" + s.features;
                        rc = system(cmd.c_str());
                        if (rc == 0) { out.format = "spirv"; binpath = spv; }
                    }
                    if (rc != 0) {
                        cmd = "dxc -E " + (s.entry.empty() ? "main" : s.entry) + " -T " + prof + " -Fo \"" + dxil + "\" \"" + inpath + "\"";
                        rc = system(cmd.c_str());
                        if (rc == 0) { out.format = "dxil"; binpath = dxil; }
                    }
                }
                else if (tk == ToolKind::TINT) {
                    // tint in.wgsl --format=spirv --output out.spv
                    std::string spv = binpath + ".spv";
                    cmd = "tint \"" + inpath + "\" --format=spirv --output \"" + spv + "\"";
                    rc = system(cmd.c_str());
                    if (rc == 0) { out.format = "spirv"; binpath = spv; }
                }

                if (echo && !cmd.empty()) std::cerr << "[shader] " << s.name << ": " << cmd << "\n";

                // Read output or fall back to raw UTF-8 text bytes
                try {
                    if (rc == 0) {
                        std::string bytes = read_file(binpath);
                        out.bytes.assign(bytes.begin(), bytes.end());
                        rm_file(binpath);
                    }
                    else {
                        std::string bytes = s.source;
                        out.bytes.assign(bytes.begin(), bytes.end());
                        out.format = "raw";
                    }
                }
                catch (...) {
                    std::string bytes = s.source;
                    out.bytes.assign(bytes.begin(), bytes.end());
                    out.format = "raw";
                }

                // Clean up input
                rm_file(inpath);
                return out;
            }

            static std::vector<CompiledShader> compile_all(const std::vector<ShaderSpec>& specs, bool echo = false) {
                std::vector<CompiledShader> out; out.reserve(specs.size());
                for (auto& s : specs) out.push_back(compile_one(s, echo));
                return out;
            }

            // C identifier sanitizer
            static std::string identify(const std::string& s) {
                std::string r; r.reserve(s.size() + 8);
                if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) r.push_back('_');
                for (unsigned char c : s) {
                    if (isalnum(c) || c == '_') r.push_back((char)c);
                    else r.push_back('_');
                }
                return r;
            }

            // Emit C code embedding all shaders + registry and lookup helpers.
            static std::string emit_embedded(const std::vector<CompiledShader>& blobs) {
                std::ostringstream o;
                o << "\n/* --- Embedded Supreme Shaders (auto-generated) --- */\n";
                o << "typedef struct { const char* name; const char* lang; const char* stage; const char* fmt; const unsigned char* data; unsigned int size; const char* entry; const char* profile; } CS_EmbeddedShader;\n";
                for (auto& c : blobs) {
                    std::string in = identify(c.spec.name);
                    o << "static const unsigned char cs_shader_" << in << "_bin[] = {";
                    for (size_t i = 0; i < c.bytes.size(); ++i) {
                        if ((i % 16) == 0) o << "\n  ";
                        o << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)(unsigned char)c.bytes[i] << std::nouppercase << std::dec;
                        if (i + 1 != c.bytes.size()) o << ",";
                    }
                    if (!c.bytes.empty()) o << "\n";
                    o << "};\n";
                    o << "static const unsigned int cs_shader_" << in << "_bin_len = (unsigned int)sizeof(cs_shader_" << in << "_bin);\n";
                    o << "static const char* cs_shader_" << in << "_lang = \"" << c.spec.lang << "\";\n";
                    o << "static const char* cs_shader_" << in << "_stage = \"" << c.spec.stage << "\";\n";
                    o << "static const char* cs_shader_" << in << "_fmt = \"" << c.format << "\";\n";
                    o << "static const char* cs_shader_" << in << "_entry = \"" << (c.spec.entry.empty() ? "main" : c.spec.entry) << "\";\n";
                    o << "static const char* cs_shader_" << in << "_profile = \"" << c.spec.profile << "\";\n";
                }
                // Registry
                o << "static const CS_EmbeddedShader cs_shaders[] = {\n";
                for (auto& c : blobs) {
                    std::string in = identify(c.spec.name);
                    o << "  { \"" << c.spec.name << "\", cs_shader_" << in << "_lang, cs_shader_" << in << "_stage, cs_shader_" << in << "_fmt, cs_shader_" << in << "_bin, cs_shader_" << in << "_bin_len, cs_shader_" << in << "_entry, cs_shader_" << in << "_profile },\n";
                }
                o << "};\n";
                o << "static const unsigned int cs_shaders_count = (unsigned int)(sizeof(cs_shaders)/sizeof(cs_shaders[0]));\n";
                // Lookup helpers
                o << "static const CS_EmbeddedShader* cs_shader_find(const char* name){\n"
                    "  for (unsigned int i=0;i<cs_shaders_count;i++){ if (strcmp(cs_shaders[i].name,name)==0) return &cs_shaders[i]; }\n"
                    "  return NULL;\n}\n";
                return o.str();
            }

            // Prelude addendum: small stage constants
            static std::string prelude_shaders_addendum() {
                return R"(
/* --- Supreme Shaders addendum --- */
#ifndef CS_SHADERS_INCLUDED
#define CS_SHADERS_INCLUDED 1
enum { CS_SHADER_VERT=0, CS_SHADER_FRAG=1, CS_SHADER_COMP=2, CS_SHADER_GEOM=3, CS_SHADER_TESC=4, CS_SHADER_TESE=5 };
#endif
)";
            }

            // Convenience: one call to extract, compile and emit blocks.
            // Use: std::string embed = cs_supreme::emit_from_source(body /*original .csc text*/, /*echo*/ cfg.show_c);
            static std::string emit_from_source(const std::string& original_source, bool echo = false) {
                auto specs = extract_shaders(original_source);
                if (specs.empty()) return std::string();
                auto bins = compile_all(specs, echo);
                std::string pre = prelude_shaders_addendum();
                std::string emb = emit_embedded(bins);
                return pre + emb;
            }

        } // namespace cs_supreme

// ============================ Gates, Formatters & Contacts Pack (append-only) ============================
// DSL:
//   gate! Name require KEYID { ... }   -> guarded block authorized by key KEYID
//   lock! Name { ... }                 -> named process-wide lock guarding the block
//
// Directives in .csc (scanned from raw source string you already read as srcAll):
//   @key KEYID "value"                 -> registers a key (stored hashed)
//   @gatepolicy strict|relaxed         -> strict: env must match embedded; relaxed: embedded suffices
//   @contact "host:port"               -> registers a contact point for ping health checks
//
// Wire-up (minimal):
//   - Apply lowering before softline_lower:
//       auto gated = cs_gates::lower_all(matchLowered);
//       string lowered = softline_lower(gated, cfg.softline, hotFns, false);
//   - Append prelude + emitted registries right after prelude():
//       csrc = prelude(cfg.hardline);
//       csrc += cs_gates::emit_from_source(srcAll, cfg.show_c);
//       csrc += "\n";

        namespace cs_gates {

            using namespace cs_regex_wrap;

            struct KeyEnt { std::string id; std::string val; };
            static std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");  return s.substr(a, b - a + 1);
            }

            // Scan directives
            struct ScanOut {
                std::vector<KeyEnt> keys;
                std::vector<std::string> contacts;
                bool strict = true; // default strict
            };

            static ScanOut scan_directives(const std::string& src) {
                ScanOut so;
                std::istringstream ss(src);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = trim(line);
                    if (t.rfind("@", 0) != 0) continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    if (name == "key") {
                        std::string id; ls >> id;
                        std::string val;
                        if (ls.peek() == '"' || ls.peek() == '\'') ls >> std::quoted(val); else ls >> val;
                        if (!id.empty()) so.keys.push_back({ id,val });
                    }
                    else if (name == "contact") {
                        std::string cp; if (ls.peek() == '"' || ls.peek() == '\'') ls >> std::quoted(cp); else ls >> cp;
                        if (!cp.empty()) so.contacts.push_back(cp);
                    }
                    else if (name == "gatepolicy") {
                        std::string v; ls >> v;
                        if (!v.empty()) {
                            for (auto& c : v) c = (char)tolower((unsigned char)c);
                            so.strict = (v != "relaxed");
                        }
                    }
                }
                return so;
            }

            // Lowerings: gate!/lock!
            static std::string lower_gate_blocks(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex re(R"(gate!\s+([A-Za-z_]\w*)\s+require\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string gate = m[1].str();
                    const std::string key = m[2].str();
                    const std::string body = m[3].str();
                    out += "{ if (cs_gate_authorized(\"" + gate + "\",\"" + key + "\")) { " + body + " } else { cs_gate_on_deny(\"" + gate + "\",\"" + key + "\"); } }";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }
            static std::string lower_lock_blocks(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex re(R"(lock!\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    const std::string name = m[1].str();
                    const std::string body = m[2].str();
                    out += "{ cs_lock_guard __g = cs_named_lock_acquire(\"" + name + "\"); { " + body + " } cs_named_lock_release(__g); }";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }
            static std::string lower_all(const std::string& src) {
                return lower_lock_blocks(lower_gate_blocks(src));
            }

            // C prelude addendum (locks, keys, formatters, contacts)
            static std::string prelude_gates_addendum() {
                return R"(
/* --- Gates, Locks, Formatters & Contacts addendum --- */
#ifndef CS_GLF_INCLUDED
#define CS_GLF_INCLUDED 1
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef CRITICAL_SECTION cs_mutex_t;
  static void cs_mutex_init(cs_mutex_t* m){ InitializeCriticalSection(m); }
  static void cs_mutex_lock(cs_mutex_t* m){ EnterCriticalSection(m); }
  static void cs_mutex_unlock(cs_mutex_t* m){ LeaveCriticalSection(m); }
#else
  #include <pthread.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <unistd.h>
  typedef pthread_mutex_t cs_mutex_t;
  static void cs_mutex_init(cs_mutex_t* m){ pthread_mutex_init(m,NULL); }
  static void cs_mutex_lock(cs_mutex_t* m){ pthread_mutex_lock(m); }
  static void cs_mutex_unlock(cs_mutex_t* m){ pthread_mutex_unlock(m); }
#endif

/* ---------- FNV-1a 64-bit hash (for keys) ---------- */
static unsigned long long cs_hash64(const char* s){
    const unsigned long long FNV_OFFSET=1469598103934665603ULL, FNV_PRIME=1099511628211ULL;
    unsigned long long h=FNV_OFFSET; if(!s) return 0ULL;
    for (; *s; ++s){ h ^= (unsigned char)(*s); h *= FNV_PRIME; }
    return h;
}

/* ---------- Key registry (filled by emitter below) ---------- */
typedef struct { const char* id; unsigned long long hv; } cs_key_ent;
extern const cs_key_ent cs_embedded_keys[];
extern const int cs_gate_policy_strict;

/* ---------- Authorization ---------- */
static int cs_gate_authorized(const char* gate, const char* keyId){
    (void)gate;
    // Find embedded hash for keyId (if any)
    unsigned long long targetHv = 0ULL;
    int haveEmbedded = 0;
    for (const cs_key_ent* e=cs_embedded_keys; e && e->id; ++e){
        if (strcmp(e->id,keyId)==0){ targetHv = e->hv; haveEmbedded = 1; break; }
    }
    // Env override: CS_KEY_<ID>
    char envName[256]; size_t i=0;
    strcpy(envName, "CS_KEY_");
    i = strlen(envName);
    for (const char* p=keyId; *p && i+1 < sizeof(envName); ++p){ char c=*p; if (c=='-') c='_'; envName[i++]=(char)toupper((unsigned char)c); }
    envName[i]=0;
    const char* v = getenv(envName);
    if (cs_gate_policy_strict){
        if (!haveEmbedded || !v) return 0;
        unsigned long long hvEnv = cs_hash64(v);
        return hvEnv==targetHv;
    }else{ // relaxed: embedded suffices; or if env provided, must match when embedded exists
        if (haveEmbedded){
            if (!v) return 1;
            unsigned long long hvEnv = cs_hash64(v);
            return hvEnv==targetHv;
        }else{
            // no embedded; allow if any env provided
            return v && *v;
        }
    }
}

static void cs_gate_on_deny(const char* gate, const char* keyId){
#ifdef CS_HARDLINE
    fprintf(stderr, "[gate] DENY gate=%s key=%s\n", gate, keyId);
    abort();
#else
    fprintf(stderr, "[gate] deny (soft) gate=%s key=%s\n", gate, keyId);
#endif
}

/* ---------- Named lock registry ---------- */
typedef struct { const char* name; cs_mutex_t m; int inited; } cs_named_lock;
static cs_mutex_t cs_registry_mu; static int cs_registry_mu_inited=0;
static cs_named_lock* cs_locks = NULL; static int cs_locks_len=0, cs_locks_cap=0;

static void cs_registry_init_once(void){
    if (!cs_registry_mu_inited){ cs_mutex_init(&cs_registry_mu); cs_registry_mu_inited=1; }
}
static cs_named_lock* cs_find_or_create_lock(const char* name){
    cs_registry_init_once();
    cs_mutex_lock(&cs_registry_mu);
    for (int i=0;i<cs_locks_len;i++){ if (cs_locks[i].name && strcmp(cs_locks[i].name,name)==0){ cs_mutex_unlock(&cs_registry_mu); return &cs_locks[i]; } }
    if (cs_locks_len==cs_locks_cap){
        int ncap = cs_locks_cap? cs_locks_cap*2 : 16;
        cs_named_lock* n = (cs_named_lock*)realloc(cs_locks, (size_t)ncap*sizeof(cs_named_lock));
        if (!n){ cs_mutex_unlock(&cs_registry_mu); return NULL; }
        for (int i=cs_locks_cap;i<ncap;i++){ n[i].name=NULL; n[i].inited=0; }
        cs_locks = n; cs_locks_cap = ncap;
    }
    cs_named_lock* L = &cs_locks[cs_locks_len++];
    L->name = _strdup(name);
#if !defined(_WIN32)
    if(!L->name) L->name = strdup(name);
#endif
    cs_mutex_init(&L->m); L->inited=1;
    cs_mutex_unlock(&cs_registry_mu);
    return L;
}
typedef struct { cs_named_lock* p; } cs_lock_guard;
static cs_lock_guard cs_named_lock_acquire(const char* name){
    cs_named_lock* L = cs_find_or_create_lock(name);
    if (L && L->inited) cs_mutex_lock(&L->m);
    cs_lock_guard g; g.p=L; return g;
}
static void cs_named_lock_release(cs_lock_guard g){
    if (g.p && g.p->inited) cs_mutex_unlock(&g.p->m);
}

/* ---------- Superior Formatters ---------- */
typedef struct { char* buf; size_t cap; size_t len; } cs_buf;
static void cs_buf_init(cs_buf* b, size_t cap){ b->buf=(char*)malloc(cap); b->cap=cap; b->len=0; if(b->buf) b->buf[0]=0; }
static void cs_buf_free(cs_buf* b){ if(b->buf) free(b->buf); b->buf=NULL; b->cap=b->len=0; }
static void cs_buf_putn(cs_buf* b, const char* s, size_t n){
    if (!b->buf) return; if (b->len+n+1 > b->cap){ size_t nc = b->cap? b->cap*2 : 256; while (nc < b->len+n+1) nc*=2; char* p=(char*)realloc(b->buf,nc); if(!p) return; b->buf=p; b->cap=nc; }
    memcpy(b->buf+b->len, s, n); b->len += n; b->buf[b->len]=0;
}
static void cs_buf_puts(cs_buf* b, const char* s){ cs_buf_putn(b,s,strlen(s)); }
static void cs_buf_putc(cs_buf* b, char c){ cs_buf_putn(b,&c,1); }

static void cs_json_escape(cs_buf* b, const char* s){
    cs_buf_putc(b,'"');
    for (; *s; ++s){
        unsigned char c=(unsigned char)*s;
        if (c=='"'||c=='\\') { cs_buf_putc(b,'\\'); cs_buf_putc(b,(char)c); }
        else if (c=='\b') { cs_buf_puts(b,"\\b"); }
        else if (c=='\f') { cs_buf_puts(b,"\\f"); }
        else if (c=='\n') { cs_buf_puts(b,"\\n"); }
        else if (c=='\r') { cs_buf_puts(b,"\\r"); }
        else if (c=='\t') { cs_buf_puts(b,"\\t"); }
        else if (c<0x20) { char tmp[7]; snprintf(tmp,sizeof(tmp),"\\u%04x",c); cs_buf_puts(b,tmp); }
        else cs_buf_putc(b,(char)c);
    }
    cs_buf_putc(b,'"');
}
static void cs_hexdump(cs_buf* b, const void* data, size_t len){
    const unsigned char* p=(const unsigned char*)data;
    for (size_t i=0;i<len;i++){ char x[4]; snprintf(x,sizeof(x),"%02X", (unsigned int)p[i]); cs_buf_puts(b,x); if ((i+1)%2==0 && i+1<len) cs_buf_putc(b,' '); }
}
static void cs_logf(const char* level, const char* fmt, ...){
    time_t t=time(NULL); struct tm tmval; 
#if defined(_WIN32)
    localtime_s(&tmval,&t);
#else
    localtime_r(&t,&tmval);
#endif
    char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tmval);
    fprintf(stderr,"[%s] [%s] ", ts, level);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define CS_LOG_INFO(...)  cs_logf("INFO", __VA_ARGS__)
#define CS_LOG_WARN(...)  cs_logf("WARN", __VA_ARGS__)
#define CS_LOG_ERROR(...) cs_logf("ERROR", __VA_ARGS__)

/* ---------- Contacts & Ping (TCP connect with timeout, retries, jitter) ---------- */
extern const char* const cs_contacts[];
static int cs_net_inited=0;
static void cs_net_init_once(void){
#if defined(_WIN32)
    if (!cs_net_inited){ WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w)==0) cs_net_inited=1; }
#else
    cs_net_inited=1;
#endif
}
static int cs_connect_with_timeout(const char* host, const char* port, int timeout_ms){
    cs_net_init_once();
    struct addrinfo hints; memset(&hints,0,sizeof(hints));
    hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    struct addrinfo* res=0;
    if (getaddrinfo(host, port, &hints, &res)!=0) return 0;
    int ok=0;
    for (struct addrinfo* ai=res; ai; ai=ai->ai_next){
        int s = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s<0) continue;
#if defined(_WIN32)
        u_long nb=1; ioctlsocket(s, FIONBIO, &nb);
#else
        int flags = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
#if defined(_WIN32)
        if (rc==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK){ closesocket(s); continue; }
#else
        if (rc<0 && errno!=EINPROGRESS){ close(s); continue; }
#endif
        fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
        struct timeval tv; tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000;
        rc = select(s+1, NULL, &wfds, NULL, &tv);
        if (rc>0 && FD_ISSET(s,&wfds)) ok=1;
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        if (ok) break;
    }
    if (res) freeaddrinfo(res);
    return ok;
}
static int cs_ping_contact(const char* contact, int timeout_ms){
    // split host:port
    const char* c = strrchr(contact, ':');
    if (!c) return 0;
    char host[256]; char port[16];
    size_t hl = (size_t)(c-contact); if (hl >= sizeof(host)) return 0;
    memcpy(host, contact, hl); host[hl]=0;
    snprintf(port, sizeof(port), "%s", c+1);
    return cs_connect_with_timeout(host, port, timeout_ms);
}
static int cs_ping_all(int timeout_ms, int retries){
    int healthy=0;
    for (int i=0; cs_contacts && cs_contacts[i]; ++i){
        const char* cp = cs_contacts[i];
        int ok=0;
        for (int a=0; a<retries && !ok; ++a){
            int jitter = (rand()%50); // 0..49ms
            int tmo = timeout_ms + jitter;
            ok = cs_ping_contact(cp, tmo);
        }
        if (ok) healthy++;
    }
    return healthy;
}
#endif /* CS_GLF_INCLUDED */
)";
            }

            // Emit registries (keys, policy, contacts) from source
            static std::string emit_from_source(const std::string& srcAll, bool echo = false) {
                ScanOut so = scan_directives(srcAll);
                if (echo) {
                    std::cerr << "[gates] keys=" << so.keys.size()
                        << " contacts=" << so.contacts.size()
                        << " policy=" << (so.strict ? "strict" : "relaxed") << "\n";
                }
                std::ostringstream o;
                o << prelude_gates_addendum();
                // keys
                o << "static const cs_key_ent cs_embedded_keys[] = {\n";
                for (auto& k : so.keys) {
                    // hash at compile time (C++ host), embed as constant
                    // FNV1a in C++ (duplicate small impl here):
                    auto hash64 = [](const std::string& s)->unsigned long long {
                        const unsigned long long FNV_OFFSET = 1469598103934665603ULL, FNV_PRIME = 1099511628211ULL;
                        unsigned long long h = FNV_OFFSET;
                        for (unsigned char c : s) { h ^= c; h *= FNV_PRIME; }
                        return h;
                        };
                    unsigned long long hv = hash64(k.val);
                    o << "  { \"" << k.id << "\", " << hv << "ULL },\n";
                }
                o << "  { NULL, 0ULL }\n};\n";
                // policy
                o << "static const int cs_gate_policy_strict = " << (so.strict ? "1" : "0") << ";\n";
                // contacts
                o << "static const char* const cs_contacts[] = {\n";
                for (auto& c : so.contacts) o << "  \"" << c << "\",\n";
                o << "  NULL\n};\n";
                return o.str();
            }

        } // namespace cs_gates

// ============================ MAX Platform & Graphics Support Pack (append-only) ============================
// Directives supported in .csc:
//   @graphics directx|opengl|software
//   @opencl on|off
//   @sockets on|off
//   @particles on|off
//   @vsix id="..." name="..." version="..." publisher="..." desc="..." asset "path\to\file"
//   @msix identity="..." display="..." version="..." publisher="..." logo="path" capability "internetClient"
// Wiring (minimal):
//   - Replace build_cmd(...) calls with: cs_maxpack::build_cmd_graphics(cfg, cc, cpath, out, defineProfile, /*src_for_scan*/ c_src);
//   - When constructing the C translation unit, append once after prelude(...):
//       csrc += cs_maxpack::prelude_max_addendum(cs_maxpack::scan_features(srcAll));
//   - Optionally emit VSIX/MSIX artifacts after reading srcAll (no effect if not present):
//       cs_maxpack::maybe_emit_packages(srcAll, /*echo*/cfg.show_c);

        namespace cs_maxpack {

            // ------------------------------ Feature scan ------------------------------
            struct Features {
                std::string api;      // "directx"|"opengl"|"software"
                bool opencl = false;
                bool sockets = true;
                bool particles = true;
                // Packaging
                struct Vsix { bool on = false; std::string id, name, version, publisher, desc; std::vector<std::string> assets; } vsix;
                struct Msix { bool on = false; std::string identity, display, version, publisher, logo; std::vector<std::string> caps; } msix;
            };

            static std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");  return s.substr(a, b - a + 1);
            }
            static std::map<std::string, std::string> parse_kv_list(const std::string& s) {
                std::map<std::string, std::string> m;
                std::istringstream ls(s);
                std::string tok;
                auto unq = [](std::string t)->std::string {
                    t = trim(t);
                    if (!t.empty() && (t.front() == '"' || t.front() == '\'')) {
                        char q = t.front(); if (t.size() >= 2 && t.back() == q) t = t.substr(1, t.size() - 2);
                    }
                    return t;
                    };
                while (ls >> tok) {
                    size_t eq = tok.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = trim(tok.substr(0, eq));
                    std::string v = unq(tok.substr(eq + 1));
                    m[k] = v;
                }
                return m;
            }

            static Features scan_features(const std::string& src) {
                Features f; f.api = "software";
                std::istringstream ss(src);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = trim(line);
                    if (t.rfind("@", 0) != 0) continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    for (auto& c : name) c = (char)tolower((unsigned char)c);

                    if (name == "graphics") {
                        std::string v; ls >> v; for (auto& c : v) c = (char)tolower((unsigned char)c);
                        if (!v.empty()) f.api = v;
                    }
                    else if (name == "opencl") {
                        std::string v; ls >> v; f.opencl = (v != "off");
                    }
                    else if (name == "sockets") {
                        std::string v; ls >> v; f.sockets = (v != "off");
                    }
                    else if (name == "particles") {
                        std::string v; ls >> v; f.particles = (v != "off");
                    }
                    else if (name == "vsix") {
                        f.vsix.on = true;
                        std::string rest; std::getline(ls, rest);
                        auto kv = parse_kv_list(rest);
                        if (kv.count("id"))        f.vsix.id = kv["id"];
                        if (kv.count("name"))      f.vsix.name = kv["name"];
                        if (kv.count("version"))   f.vsix.version = kv["version"];
                        if (kv.count("publisher")) f.vsix.publisher = kv["publisher"];
                        if (kv.count("desc"))      f.vsix.desc = kv["desc"];
                    }
                    else if (name == "vsixasset") {
                        std::string p; if (ls.peek() == '"' || ls.peek() == '\'') ls >> std::quoted(p); else ls >> p;
                        if (!p.empty()) f.vsix.assets.push_back(p);
                    }
                    else if (name == "msix") {
                        f.msix.on = true;
                        std::string rest; std::getline(ls, rest);
                        auto kv = parse_kv_list(rest);
                        if (kv.count("identity"))  f.msix.identity = kv["identity"];
                        if (kv.count("display"))   f.msix.display = kv["display"];
                        if (kv.count("version"))   f.msix.version = kv["version"];
                        if (kv.count("publisher")) f.msix.publisher = kv["publisher"];
                        if (kv.count("logo"))      f.msix.logo = kv["logo"];
                    }
                    else if (name == "msixcap") {
                        std::string cap; ls >> cap; if (!cap.empty()) f.msix.caps.push_back(cap);
                    }
                }
                return f;
            }

            // ------------------------------ Build command (augmented) ------------------------------
            static void add(std::vector<std::string>& v, const std::string& s) { v.push_back(s); }

            static std::string build_cmd_graphics(const Config& cfg,
                const std::string& cc,
                const std::string& cpath,
                const std::string& out,
                bool defineProfile = false,
                const std::string& src_for_scan = std::string()) {
                Features feat = scan_features(src_for_scan);
                std::vector<std::string> cmd; add(cmd, cc);
                bool msvc = (cc == "cl" || cc == "clang-cl");

                // Baseline identical to existing build_cmd(...)
                if (msvc) {
                    add(cmd, "/nologo");
                    if (cfg.opt == "O0") add(cmd, "/Od");
                    else if (cfg.opt == "O1") add(cmd, "/O1");
                    else add(cmd, "/O2");
                    if (cfg.hardline || cfg.strict) { add(cmd, "/Wall"); add(cmd, "/WX"); }
                    if (cfg.lto) add(cmd, "/GL");
                    if (cfg.hardline) add(cmd, "/DCS_HARDLINE=1");
                    if (defineProfile) add(cmd, "/DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) add(cmd, "/D" + d);
                    for (auto& p : cfg.incs)    add(cmd, "/I" + p);

                    // Graphics/OpenCL toggles
                    if (feat.api == "directx") {
                        add(cmd, "/DCS_HAS_D3D=1");
                    }
                    else if (feat.api == "opengl") {
                        add(cmd, "/DCS_HAS_GL=1");
                    }
                    if (feat.opencl) add(cmd, "/DCS_HAS_OPENCL=1");
                    if (feat.particles) add(cmd, "/DCS_HAS_PARTICLES=1");
                    if (feat.sockets) add(cmd, "/DCS_HAS_SOCKETS=1");

                    add(cmd, cpath);
                    add(cmd, "/Fe:" + out);

                    // Link libraries
                    std::vector<std::string> linkTail;
                    if (feat.api == "directx") {
                        linkTail.push_back("d3d11.lib"); linkTail.push_back("dxgi.lib"); linkTail.push_back("d3dcompiler.lib");
                    }
                    else if (feat.api == "opengl") {
                        linkTail.push_back("opengl32.lib"); linkTail.push_back("gdi32.lib"); linkTail.push_back("user32.lib");
                    }
                    else {
                        linkTail.push_back("user32.lib"); linkTail.push_back("gdi32.lib");
                    }
                    if (feat.opencl) linkTail.push_back("OpenCL.lib");
                    // Always good to have ws2_32 for sockets when enabled
                    if (feat.sockets) linkTail.push_back("ws2_32.lib");

                    for (auto& lp : cfg.libpaths) add(cmd, "/link /LIBPATH:\"" + lp + "\"");
                    for (auto& l : cfg.links) {
                        std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib";
                        add(cmd, "/link " + lib);
                    }
                    for (auto& lib : linkTail) add(cmd, "/link " + lib);
                }
                else {
                    add(cmd, "-std=c11");
                    if (cfg.opt == "O0") add(cmd, "-O0");
                    else if (cfg.opt == "O1") add(cmd, "-O1");
                    else if (cfg.opt == "O2") add(cmd, "-O2");
                    else if (cfg.opt == "O3") add(cmd, "-O3");
                    else if (cfg.opt == "size") add(cmd, "-Os");
                    else if (cfg.opt == "max") { add(cmd, "-O3"); if (cfg.lto) add(cmd, "-flto"); }
                    if (cfg.hardline) { add(cmd, "-Wall"); add(cmd, "-Wextra"); add(cmd, "-Werror"); add(cmd, "-Wconversion"); add(cmd, "-Wsign-conversion"); }
                    if (cfg.lto) add(cmd, "-flto");
                    if (cfg.hardline) add(cmd, "-DCS_HARDLINE=1");
                    if (defineProfile) add(cmd, "-DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) add(cmd, "-D" + d);
                    for (auto& p : cfg.incs)    add(cmd, "-I" + p);

                    if (feat.api == "opengl") add(cmd, "-DCS_HAS_GL=1");
                    if (feat.api == "directx") add(cmd, "-DCS_HAS_D3D=1"); // non-Windows: no effect
                    if (feat.opencl) add(cmd, "-DCS_HAS_OPENCL=1");
                    if (feat.particles) add(cmd, "-DCS_HAS_PARTICLES=1");
                    if (feat.sockets) add(cmd, "-DCS_HAS_SOCKETS=1");

                    add(cmd, cpath);
                    add(cmd, "-o"); add(cmd, out);
                    for (auto& lp : cfg.libpaths) add(cmd, "-L" + lp);
                    for (auto& l : cfg.links)     add(cmd, "-l" + l);

#if defined(__APPLE__)
                    if (feat.api == "opengl") { add(cmd, "-framework"); add(cmd, "OpenGL"); add(cmd, "-framework"); add(cmd, "Cocoa"); }
#else
                    if (feat.api == "opengl") { add(cmd, "-lGL"); add(cmd, "-lX11"); }
#endif
                    if (feat.opencl) { add(cmd, "-lOpenCL"); }
                    if (feat.sockets) {
#if defined(__APPLE__)
                        add(cmd, "-liconv"); // harmless
#endif
                        add(cmd, "-lpthread");
                    }
                }

                std::string full;
                for (size_t i = 0; i < cmd.size(); ++i) {
                    if (i) full.push_back(' ');
                    bool q = cmd[i].find(' ') != std::string::npos;
                    if (q) full.push_back('"');
                    full += cmd[i];
                    if (q) full.push_back('"');
                }
                return full;
            }

            // ------------------------------ Prelude (math, particles, sockets, OpenCL helper) ------------------------------
            static std::string prelude_max_addendum(const Features& f) {
                std::ostringstream o;
                o << "/* --- MAX Platform & Graphics Prelude Addendum --- */\n";
                o << "#ifndef CS_MAXPACK_INCLUDED\n#define CS_MAXPACK_INCLUDED 1\n";
                o << "#include <math.h>\n";
                if (f.sockets) {
                    o << "#define CS_HAS_SOCKETS 1\n";
#if defined(_WIN32)
                    o << "#include <winsock2.h>\n#include <ws2tcpip.h>\n#pragma comment(lib, \"ws2_32.lib\")\n";
#else
                    o << "#include <sys/types.h>\n#include <sys/socket.h>\n#include <netdb.h>\n#include <arpa/inet.h>\n#include <fcntl.h>\n#include <unistd.h>\n#include <pthread.h>\n";
#endif
                }
                if (f.opencl) {
                    o << "#define CS_HAS_OPENCL 1\n";
                    o << "#if __has_include(<CL/cl.h>)\n#include <CL/cl.h>\n#define CS_OPENCL_AVAILABLE 1\n#else\n#define CS_OPENCL_AVAILABLE 0\n#endif\n";
                }
                if (f.particles) o << "#define CS_HAS_PARTICLES 1\n";

                // Math types
                o << "typedef struct { float x,y; } cs_vec2;\n";
                o << "typedef struct { float x,y,z; } cs_vec3;\n";
                o << "typedef struct { float x,y,z,w; } cs_vec4;\n";
                o << "typedef struct { float m[16]; } cs_mat4;\n";
                o << "static cs_vec3 cs_v3(float x,float y,float z){ cs_vec3 v={x,y,z}; return v; }\n";
                o << "static cs_mat4 cs_m4_identity(void){ cs_mat4 r={{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}; return r; }\n";
                o << "static cs_mat4 cs_m4_mul(cs_mat4 a, cs_mat4 b){ cs_mat4 r; for(int i=0;i<4;i++){ for(int j=0;j<4;j++){ r.m[i*4+j]=0; for(int k=0;k<4;k++) r.m[i*4+j]+=a.m[i*4+k]*b.m[k*4+j]; } } return r; }\n";
                o << "static cs_mat4 cs_m4_perspective(float fovy,float aspect,float zn,float zf){ float f=1.0f/tanf(fovy*0.5f); cs_mat4 r={{f/aspect,0,0,0, 0,f,0,0, 0,0,(zf+zn)/(zn-zf),-1, 0,0,(2*zf*zn)/(zn-zf),0}}; return r; }\n";
                o << "static cs_vec3 cs_v3_sub(cs_vec3 a, cs_vec3 b){ return cs_v3(a.x-b.x,a.y-b.y,a.z-b.z);} static cs_vec3 cs_v3_cross(cs_vec3 a, cs_vec3 b){ return cs_v3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);} static float cs_v3_dot(cs_vec3 a, cs_vec3 b){ return a.x*b.x+a.y*b.y+a.z*b.z;} static cs_vec3 cs_v3_norm(cs_vec3 v){ float L=sqrtf(cs_v3_dot(v,v)); return L>0? cs_v3(v.x/L,v.y/L,v.z/L):v; }\n";
                o << "static cs_mat4 cs_m4_lookat(cs_vec3 eye,cs_vec3 center,cs_vec3 up){ cs_vec3 f=cs_v3_norm(cs_v3_sub(center,eye)); cs_vec3 s=cs_v3_norm(cs_v3_cross(f, up)); cs_vec3 u=cs_v3_cross(s,f); cs_mat4 r={{ s.x, u.x,-f.x,0,  s.y, u.y,-f.y,0,  s.z, u.z,-f.z,0,  -cs_v3_dot(s,eye), -cs_v3_dot(u,eye), cs_v3_dot(f,eye), 1}}; return r; }\n";

                // Particles
                if (f.particles) {
                    o << "typedef struct { cs_vec3 p; cs_vec3 v; float life; } cs_particle;\n";
                    o << "static void cs_particles_emit(cs_particle* a,int n, cs_vec3 origin, float speed){ for(int i=0;i<n;i++){ a[i].p=origin; float ux=(float)rand()/RAND_MAX*2.f-1.f; float uy=(float)rand()/RAND_MAX*2.f-1.f; float uz=(float)rand()/RAND_MAX*2.f-1.f; cs_vec3 dir=cs_v3_norm(cs_v3(ux,uy,uz)); a[i].v=cs_v3(dir.x*speed,dir.y*speed,dir.z*speed); a[i].life=1.f; }}\n";
                    o << "static int cs_particles_update(cs_particle* a,int n,float dt, cs_vec3 gravity){ int alive=0; for(int i=0;i<n;i++){ if (a[i].life<=0.f) continue; a[i].v.x+=gravity.x*dt; a[i].v.y+=gravity.y*dt; a[i].v.z+=gravity.z*dt; a[i].p.x+=a[i].v.x*dt; a[i].p.y+=a[i].v.y*dt; a[i].p.z+=a[i].v.z*dt; a[i].life-=dt; if (a[i].life>0.f) alive++; } return alive; }\n";
                }

                // Sockets helpers
                if (f.sockets) {
                    o << "typedef struct { int ok; "
#if defined(_WIN32)
                        << "SOCKET s;"
#else
                        << "int s;"
#endif
                        << " } cs_tcp;\n";
                    o << "static int cs_sock_init_once(void){"
#if defined(_WIN32)
                        << " static int inited=0; if(!inited){ WSADATA w; if(WSAStartup(MAKEWORD(2,2),&w)!=0) return 0; inited=1;} return 1;"
#else
                        << " return 1;"
#endif
                        << "}\n";
                    o << "static cs_tcp cs_tcp_connect_host(const char* host,const char* port,int timeout_ms){ cs_tcp r; r.ok=0; r.s="
#if defined(_WIN32)
                        << "INVALID_SOCKET"
#else
                        << "-1"
#endif
                        << "; if(!cs_sock_init_once()) return r; struct addrinfo hints; memset(&hints,0,sizeof(hints)); hints.ai_socktype=SOCK_STREAM; hints.ai_family=AF_UNSPEC; struct addrinfo* res=0; if(getaddrinfo(host,port,&hints,&res)!=0) return r; for(struct addrinfo* ai=res; ai; ai=ai->ai_next){ "
#if defined(_WIN32)
                        << "SOCKET s=socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol); if(s==INVALID_SOCKET) continue; u_long nb=1; ioctlsocket(s,FIONBIO,&nb);"
#else
                        << "int s=socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol); if(s<0) continue; int flags=fcntl(s,F_GETFL,0); fcntl(s,F_SETFL,flags|O_NONBLOCK);"
#endif
                        << " int rc=connect(s, ai->ai_addr, (int)ai->ai_addrlen); "
#if defined(_WIN32)
                        << " if(rc==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK){ closesocket(s); continue; }"
#else
                        << " if(rc<0 && errno!=EINPROGRESS){ close(s); continue; }"
#endif
                        << " fd_set wf; FD_ZERO(&wf); FD_SET(s,&wf); struct timeval tv; tv.tv_sec=timeout_ms/1000; tv.tv_usec=(timeout_ms%1000)*1000; rc=select((int)(s+1),NULL,&wf,NULL,&tv); if(rc>0 && FD_ISSET(s,&wf)){ r.ok=1; r.s=s; break; } "
#if defined(_WIN32)
                        << " closesocket(s);"
#else
                        << " close(s);"
#endif
                        << " } if(res) freeaddrinfo(res); return r; }\n";
                    o << "static int cs_tcp_send_all(cs_tcp* c, const void* buf, int len){ if(!c||!c->ok) return 0; const char* p=(const char*)buf; int sent=0; while(sent<len){ "
#if defined(_WIN32)
                        << "int rc=send(c->s, p+sent, len-sent, 0); if(rc==SOCKET_ERROR) return 0;"
#else
                        << "int rc=(int)send(c->s, p+sent, (size_t)(len-sent), 0); if(rc<=0) return 0;"
#endif
                        << " sent+=rc; } return 1; }\n";
                    o << "static int cs_tcp_recv_some(cs_tcp* c, void* buf, int cap){ if(!c||!c->ok) return -1; "
#if defined(_WIN32)
                        << "int rc=recv(c->s,(char*)buf, cap, 0); return rc;"
#else
                        << "int rc=(int)recv(c->s, buf, (size_t)cap, 0); return rc;"
#endif
                        << " }\n";
                    o << "static void cs_tcp_close(cs_tcp* c){ if(!c||!c->ok) return; "
#if defined(_WIN32)
                        << "closesocket(c->s);"
#else
                        << "close(c->s);"
#endif
                        << " c->ok=0; }\n";
                }

                // OpenCL helper
                if (f.opencl) {
                    o << "typedef struct { int ok; "
                        "cl_platform_id plat; cl_device_id dev; cl_context ctx; cl_command_queue queue; } cs_cl;\n";
                    o << "static cs_cl cs_cl_init_first(void){ cs_cl r; memset(&r,0,sizeof(r)); #if CS_OPENCL_AVAILABLE\n"
                        "cl_platform_id plats[8]; cl_uint np=0; if (clGetPlatformIDs(8, plats, &np)!=CL_SUCCESS || np==0) return r; r.plat = plats[0];\n"
                        "cl_device_id devs[8]; cl_uint nd=0; if (clGetDeviceIDs(r.plat, CL_DEVICE_TYPE_DEFAULT, 8, devs, &nd)!=CL_SUCCESS || nd==0) return r; r.dev = devs[0];\n"
#if defined(CL_VERSION_2_0)
                        "r.ctx = clCreateContext(NULL, 1, &r.dev, NULL, NULL, NULL);\n"
                        "r.queue = clCreateCommandQueueWithProperties(r.ctx, r.dev, 0, NULL);\n"
#else
                        "r.ctx = clCreateContext(NULL, 1, &r.dev, NULL, NULL, NULL);\n"
                        "r.queue = clCreateCommandQueue(r.ctx, r.dev, 0, NULL);\n"
#endif
                        "r.ok = (r.ctx && r.queue); #else (void)r; #endif return r; }\n";
                    o << "static cl_program cs_cl_build_src(cs_cl* cl, const char* src, const char* opts){ #if CS_OPENCL_AVAILABLE\n"
                        "size_t len=strlen(src); cl_int err=0; const char* s=src; cl_program p=clCreateProgramWithSource(cl->ctx, 1, &s, &len, &err);\n"
                        "if (err!=CL_SUCCESS) return 0; err=clBuildProgram(p, 1, &cl->dev, opts?opts:\"\", NULL, NULL);\n"
                        "if (err!=CL_SUCCESS){ size_t logsz=0; clGetProgramBuildInfo(p, cl->dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &logsz); char* log=(char*)malloc(logsz+1);\n"
                        "clGetProgramBuildInfo(p, cl->dev, CL_PROGRAM_BUILD_LOG, logsz, log, NULL); log[logsz]=0; fprintf(stderr,\"[opencl] build log:\\n%s\\n\", log); free(log); clReleaseProgram(p); return 0; }\n"
                        "return p; #else (void)cl;(void)src;(void)opts; return 0; #endif }\n";
                }

                o << "#endif /* CS_MAXPACK_INCLUDED */\n";
                return o.str();
            }

            // ------------------------------ Packaging helpers (VSIX/MSIX best-effort) ------------------------------
            static void write_file(const std::string& path, const std::string& content) {
                std::ofstream o(path, std::ios::binary); o << content;
            }

            static void maybe_emit_packages(const std::string& srcAll, bool echo = false) {
#if defined(_WIN32)
                Features f = scan_features(srcAll);
                if (f.vsix.on) {
                    char tmpPath[MAX_PATH]; GetTempPathA(MAX_PATH, tmpPath);
                    std::string base = std::string(tmpPath) + "vsix_" + (f.vsix.id.empty() ? "ext" : f.vsix.id) + "\\";
                    CreateDirectoryA(base.c_str(), NULL);
                    std::ostringstream man;
                    man << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                        "<PackageManifest Version=\"2.0.0\" xmlns=\"http://schemas.microsoft.com/developer/vsx-schema/2011\">\n"
                        "  <Metadata>\n"
                        "    <Identity Id=\"" << (f.vsix.id.empty() ? "com.example.ext" : f.vsix.id) << "\" Version=\"" << (f.vsix.version.empty() ? "1.0.0" : f.vsix.version) << "\" Publisher=\"" << (f.vsix.publisher.empty() ? "Unknown" : f.vsix.publisher) << "\"/>\n"
                        "    <DisplayName>" << (f.vsix.name.empty() ? "Extension" : f.vsix.name) << "</DisplayName>\n"
                        "    <Description xml:space=\"preserve\">" << (f.vsix.desc.empty() ? "Generated VSIX" : f.vsix.desc) << "</Description>\n"
                        "  </Metadata>\n"
                        "  <Installation>\n"
                        "    <InstallationTarget Id=\"Microsoft.VisualStudio.Community\" Version=\"[17.0,18.0)\" />\n"
                        "  </Installation>\n"
                        "</PackageManifest>\n";
                    write_file(base + "extension.vsixmanifest", man.str());
                    // Copy assets if any (best-effort)
                    for (auto& a : f.vsix.assets) {
                        std::ifstream in(a, std::ios::binary); if (!in) continue;
                        std::ostringstream ss; ss << in.rdbuf();
                        std::string outPath = base + (strrchr(a.c_str(), '\\') ? strrchr(a.c_str(), '\\') + 1 : (strrchr(a.c_str(), '/') ? strrchr(a.c_str(), '/') + 1 : a.c_str()));
                        write_file(outPath, ss.str());
                    }
                    // Pack using PowerShell Compress-Archive if available
                    std::string vsixPath = base.substr(0, base.size() - 1) + ".vsix";
                    std::string cmd = "powershell -NoProfile -Command \"Try { Compress-Archive -Force -Path '" + base + "*' -DestinationPath '" + vsixPath + "' } Catch { exit 1 }\"";
                    int rc = system(cmd.c_str());
                    if (echo) std::cerr << "[vsix] manifest at " << base << " -> " << vsixPath << " rc=" << rc << "\n";
                }
                if (f.msix.on) {
                    char tmpPath[MAX_PATH]; GetTempPathA(MAX_PATH, tmpPath);
                    std::string base = std::string(tmpPath) + "msix_" + (f.msix.identity.empty() ? "app" : f.msix.identity) + "\\";
                    CreateDirectoryA(base.c_str(), NULL);
                    std::ostringstream appx;
                    appx << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                        "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\" xmlns:uap=\"http://schemas.microsoft.com/appx/manifest/uap/windows10\" IgnorableNamespaces=\"uap\">\n"
                        "  <Identity Name=\"" << (f.msix.identity.empty() ? "com.example.app" : f.msix.identity) << "\" Publisher=\"" << (f.msix.publisher.empty() ? "CN=Publisher" : f.msix.publisher) << "\" Version=\"" << (f.msix.version.empty() ? "1.0.0.0" : f.msix.version) << "\"/>\n"
                        "  <Properties>\n"
                        "    <DisplayName>" << (f.msix.display.empty() ? "App" : f.msix.display) << "</DisplayName>\n"
                        "    <PublisherDisplayName>" << (f.msix.publisher.empty() ? "Publisher" : f.msix.publisher) << "</PublisherDisplayName>\n"
                        "    <Logo>" << (f.msix.logo.empty() ? "StoreLogo.png" : f.msix.logo) << "</Logo>\n"
                        "  </Properties>\n"
                        "  <Resources><Resource Language=\"en-us\"/></Resources>\n"
                        "  <Applications>\n"
                        "    <Application Id=\"App\" Executable=\"app.exe\" EntryPoint=\"Windows.FullTrustApplication\">\n"
                        "      <uap:VisualElements DisplayName=\"" << (f.msix.display.empty() ? "App" : f.msix.display) << "\" Description=\"" << (f.msix.display.empty() ? "" : f.msix.display) << "\" BackgroundColor=\"transparent\" Square150x150Logo=\"" << (f.msix.logo.empty() ? "Logo.png" : f.msix.logo) << "\" Square44x44Logo=\"" << (f.msix.logo.empty() ? "SmallLogo.png" : f.msix.logo) << "\"/>\n"
                        "    </Application>\n"
                        "  </Applications>\n"
                        "</Package>\n";
                    write_file(base + "AppxManifest.xml", appx.str());
                    std::string msixPath = base.substr(0, base.size() - 1) + ".msix";
                    std::string cmd = "MakeAppx.exe pack /o /d \"" + base + "\" /p \"" + msixPath + "\"";
                    int rc = system(cmd.c_str());
                    if (echo) std::cerr << "[msix] manifest at " << base << " -> " << msixPath << " rc=" << rc << "\n";
                }
#else
                (void)srcAll; (void)echo;
#endif
            }

        } // namespace cs_maxpack

// ============================ Ultimate Systems Pack (append-only) ============================
// Zero-cost helpers via inline/static macros + conservative DSL lowerings.
// Wire-up (minimal):
//  1) Apply lowerings before softline_lower:
//       auto u = cs_ultimate::apply_lowerings(matchLowered);
//       string lowered = softline_lower(u, cfg.softline, hotFns, false);
//  2) After prelude(cfg.hardline), append once:
//       csrc += cs_ultimate::prelude_ultimate_addendum(/*srcAll for toggles*/ srcAll);
//  3) Optional: choose build_cmd_* variant you already use (arch/vector/graphics) — no change required here.

        namespace cs_ultimate {

            // ---------- Feature toggles from directives ----------
            struct Feat {
                bool on = true;
                int  threads = 0;       // 0=auto
                bool channels = true;
                bool arenas = true;
                bool async_on = true;
                int  pool = 0;          // worker count (0=auto=hardware_concurrency)
                size_t arena_block = 1 << 20; // 1 MiB
            };
            static std::string trim(std::string s) { size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return ""; size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1); }

            static Feat scan(const std::string& src) {
                Feat f; std::istringstream ss(src); std::string line;
                auto toint = [](const std::string& v)->int { return v.empty() ? 0 : std::atoi(v.c_str()); };
                auto tosize = [](const std::string& v)->size_t { return v.empty() ? 0 : (size_t)std::strtoull(v.c_str(), nullptr, 10); };
                while (std::getline(ss, line)) {
                    std::string t = trim(line);
                    if (t.rfind("@", 0) != 0) continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    if (name == "ultimate") { std::string v; ls >> v; f.on = (v != "off"); }
                    else if (name == "threads") { std::string v; ls >> v; f.threads = toint(v); }
                    else if (name == "pool") { std::string v; ls >> v; f.pool = toint(v); }
                    else if (name == "channels") { std::string v; ls >> v; f.channels = (v != "off"); }
                    else if (name == "arenas") { std::string v; ls >> v; f.arenas = (v != "off"); }
                    else if (name == "async") { std::string v; ls >> v; f.async_on = (v != "off"); }
                    else if (name == "arena_block") { std::string v; ls >> v; size_t z = tosize(v); if (z) f.arena_block = z; }
                }
                return f;
            }

            // ---------- Prelude addendum (C side) ----------
            static std::string prelude_ultimate_addendum(const std::string& srcForScan) {
                Feat f = scan(srcForScan);
                (void)f;
                std::ostringstream o;
                o << "/* --- Ultimate Systems Prelude Addendum --- */\n";
                o << "#ifndef CS_ULT_INCLUDED\n#define CS_ULT_INCLUDED 1\n";
                o << "#include <stdint.h>\n#include <stddef.h>\n#include <time.h>\n";
                o << "#if __STDC_VERSION__>=201112L && !defined(__STDC_NO_ATOMICS__)\n#include <stdatomic.h>\n#define CS_HAS_ATOMICS 1\n#else\n#define CS_HAS_ATOMICS 0\n#endif\n";
                o << "#if defined(_WIN32)\n#include <windows.h>\n#else\n#include <pthread.h>\n#include <sys/time.h>\n#include <unistd.h>\n#endif\n";

                // Qualifiers, attributes, inline, export/import
                o << "#if defined(_MSC_VER)\n#define CS_FORCE_INLINE __forceinline\n#define CS_NOINLINE __declspec(noinline)\n"
                    "#define CS_EXPORT __declspec(dllexport)\n#define CS_IMPORT __declspec(dllimport)\n"
                    "#else\n#define CS_FORCE_INLINE inline __attribute__((always_inline))\n#define CS_NOINLINE __attribute__((noinline))\n"
                    "#define CS_EXPORT __attribute__((visibility(\"default\")))\n#define CS_IMPORT\n#endif\n";
                o << "#ifndef CS_RESTRICT\n#  if defined(__STDC_VERSION__) && __STDC_VERSION__>=199901L\n#    define CS_RESTRICT restrict\n#  elif defined(_MSC_VER)\n#    define CS_RESTRICT __restrict\n#  else\n#    define CS_RESTRICT\n#  endif\n#endif\n";
                o << "#if defined(__clang__) || defined(__GNUC__)\n#define CS_ASSUME(x) do{ if(!(x)) __builtin_unreachable(); }while(0)\n#define CS_PREFETCH(p,wr,loc) __builtin_prefetch((p),(wr),(loc))\n#else\n#define CS_ASSUME(x) do{}while(0)\n#define CS_PREFETCH(p,wr,loc) do{(void)(p);(void)(wr);(void)(loc);}while(0)\n#endif\n";

                // Durations
                o << "typedef struct { uint64_t ns; } cs_dur;\n";
                o << "static CS_FORCE_INLINE cs_dur cs_ns(uint64_t x){ cs_dur d={x}; return d; }\n";
                o << "static CS_FORCE_INLINE cs_dur cs_us(uint64_t x){ return cs_ns(x*1000ULL); }\n";
                o << "static CS_FORCE_INLINE cs_dur cs_ms(uint64_t x){ return cs_ns(x*1000000ULL); }\n";
                o << "static CS_FORCE_INLINE cs_dur cs_s(uint64_t x){ return cs_ns(x*1000000000ULL); }\n";
                o << "static CS_FORCE_INLINE uint64_t cs_dur_ms(cs_dur d){ return d.ns/1000000ULL; }\n";
                o << "static uint64_t cs_now_ns(void){\n"
                    "#if defined(_WIN32)\n LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((1000000000.0*c.QuadPart)/f.QuadPart);\n"
                    "#else\n struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000000000ULL + (uint64_t)ts.tv_nsec;\n"
                    "#endif\n}\n";

                // Arena allocator (per-thread)
                o << "#if " << (f.arenas ? 1 : 0) << "\n";
                o << "typedef struct cs_arena_block { struct cs_arena_block* next; size_t cap, used; unsigned char data[1]; } cs_arena_block;\n";
                o << "typedef struct { cs_arena_block* head; size_t blk_cap; } cs_arena;\n";
                o << "static void* cs_arena_alloc_block(size_t cap){ cs_arena_block* b=(cs_arena_block*)malloc(sizeof(cs_arena_block)+cap-1); if(!b) return NULL; b->next=NULL; b->cap=cap; b->used=0; return b; }\n";
                o << "static void cs_arena_init(cs_arena* A, size_t blk){ A->head=(cs_arena_block*)cs_arena_alloc_block(blk?blk:(size_t)" << (unsigned long long)f.arena_block << "); A->blk_cap=blk?blk:(size_t)" << (unsigned long long)f.arena_block << "; }\n";
                o << "static void* cs_arena_push(cs_arena* A, size_t n, size_t align){ size_t a = align?align:8; cs_arena_block* b=A->head; size_t off=(b->used + (a-1)) & ~(a-1); if (off+n <= b->cap){ void* p=b->data+off; b->used=off+n; return p; } cs_arena_block* nb=(cs_arena_block*)cs_arena_alloc_block((n+(A->blk_cap-1))&~(A->blk_cap-1)); if(!nb) return NULL; nb->next=b; A->head=nb; nb->used = (n + (a-1)) & ~(a-1); return nb->data; }\n";
                o << "static void cs_arena_reset(cs_arena* A){ for(cs_arena_block* b=A->head;b;b=b->next) b->used=0; }\n";
                o << "static void cs_arena_free(cs_arena* A){ cs_arena_block* b=A->head; while(b){ cs_arena_block* n=b->next; free(b); b=n; } A->head=NULL; }\n";
                o << "#endif\n";

                // Threading primitives + thread pool + futures
                o << "typedef struct cs_future { volatile int done; void* result; "
#if defined(_WIN32)
                    "HANDLE evt;"
#else
                    "pthread_mutex_t mu; pthread_cond_t cv;"
#endif
                    " } cs_future;\n";
                o << "typedef void* (*cs_task_fn)(void* arg);\n";
                o << "typedef struct { cs_task_fn fn; void* arg; cs_future* fut; } cs_task;\n";
                // Basic MPMC queue (mutex/cond) for portability
                o << "typedef struct cs_mpmc_q_node{ cs_task t; struct cs_mpmc_q_node* next; } cs_mpmc_q_node;\n";
                o << "typedef struct { cs_mpmc_q_node* head; cs_mpmc_q_node* tail; "
#if defined(_WIN32)
                    "CRITICAL_SECTION mu; CONDITION_VARIABLE cv;"
#else
                    "pthread_mutex_t mu; pthread_cond_t cv;"
#endif
                    " int stop; } cs_mpmc_q;\n";
#if defined(_WIN32)
                o << "static void cs_q_init(cs_mpmc_q* q){ q->head=q->tail=(cs_mpmc_q_node*)malloc(sizeof(cs_mpmc_q_node)); q->head->next=NULL; InitializeCriticalSection(&q->mu); InitializeConditionVariable(&q->cv); q->stop=0; }\n";
                o << "static void cs_q_push(cs_mpmc_q* q, cs_task t){ EnterCriticalSection(&q->mu); cs_mpmc_q_node* n=(cs_mpmc_q_node*)malloc(sizeof(*n)); n->t=t; n->next=NULL; q->tail->next=n; q->tail=n; WakeConditionVariable(&q->cv); LeaveCriticalSection(&q->mu);} \n";
                o << "static int cs_q_pop(cs_mpmc_q* q, cs_task* out){ EnterCriticalSection(&q->mu); while(!q->head->next && !q->stop) SleepConditionVariableCS(&q->cv,&q->mu,INFINITE); if(q->stop){ LeaveCriticalSection(&q->mu); return 0;} cs_mpmc_q_node* n=q->head->next; q->head->next=n->next; if(q->tail==n) q->tail=q->head; *out=n->t; free(n); LeaveCriticalSection(&q->mu); return 1; }\n";
                o << "static void cs_q_stop(cs_mpmc_q* q){ EnterCriticalSection(&q->mu); q->stop=1; WakeAllConditionVariable(&q->cv); LeaveCriticalSection(&q->mu);} \n";
#else
                o << "static void cs_q_init(cs_mpmc_q* q){ q->head=q->tail=(cs_mpmc_q_node*)malloc(sizeof(cs_mpmc_q_node)); q->head->next=NULL; pthread_mutex_init(&q->mu,NULL); pthread_cond_init(&q->cv,NULL); q->stop=0; }\n";
                o << "static void cs_q_push(cs_mpmc_q* q, cs_task t){ pthread_mutex_lock(&q->mu); cs_mpmc_q_node* n=(cs_mpmc_q_node*)malloc(sizeof(*n)); n->t=t; n->next=NULL; q->tail->next=n; q->tail=n; pthread_cond_signal(&q->cv); pthread_mutex_unlock(&q->mu);} \n";
                o << "static int cs_q_pop(cs_mpmc_q* q, cs_task* out){ pthread_mutex_lock(&q->mu); while(!q->head->next && !q->stop) pthread_cond_wait(&q->cv,&q->mu); if(q->stop){ pthread_mutex_unlock(&q->mu); return 0;} cs_mpmc_q_node* n=q->head->next; q->head->next=n->next; if(q->tail==n) q->tail=q->head; *out=n->t; free(n); pthread_mutex_unlock(&q->mu); return 1; }\n";
                o << "static void cs_q_stop(cs_mpmc_q* q){ pthread_mutex_lock(&q->mu); q->stop=1; pthread_cond_broadcast(&q->cv); pthread_mutex_unlock(&q->mu);} \n";
#endif

                o << "typedef struct { cs_mpmc_q q; int n; "
#if defined(_WIN32)
                    "HANDLE* th;"
#else
                    "pthread_t* th;"
#endif
                    " } cs_threadpool;\n";
                // Future helpers
#if defined(_WIN32)
                o << "static void cs_future_init(cs_future* f){ f->done=0; f->result=NULL; f->evt=CreateEventA(NULL,TRUE,FALSE,NULL);} static void cs_future_set(cs_future* f, void* r){ f->result=r; f->done=1; SetEvent(f->evt);} static void* cs_future_get(cs_future* f){ WaitForSingleObject(f->evt, INFINITE); return f->result; }\n";
#else
                o << "static void cs_future_init(cs_future* f){ f->done=0; f->result=NULL; pthread_mutex_init(&f->mu,NULL); pthread_cond_init(&f->cv,NULL);} static void cs_future_set(cs_future* f, void* r){ pthread_mutex_lock(&f->mu); f->result=r; f->done=1; pthread_cond_broadcast(&f->cv); pthread_mutex_unlock(&f->mu);} static void* cs_future_get(cs_future* f){ pthread_mutex_lock(&f->mu); while(!f->done) pthread_cond_wait(&f->cv,&f->mu); void* r=f->result; pthread_mutex_unlock(&f->mu); return r; }\n";
#endif
                // Worker entry
                o << "static "
#if defined(_WIN32)
                    "DWORD WINAPI"
#else
                    "void*"
#endif
                    " cs_worker(void* arg){ cs_threadpool* P=(cs_threadpool*)arg; cs_task t; while(cs_q_pop(&P->q,&t)){ void* r = t.fn? t.fn(t.arg):NULL; if (t.fut) cs_future_set(t.fut,r);} return 0; }\n";
                // Pool init/shutdown/submit
                o << "static void cs_pool_init(cs_threadpool* P, int n){ if(n<=0){ "
#if defined(_WIN32)
                    "SYSTEM_INFO si; GetSystemInfo(&si); n=(int)si.dwNumberOfProcessors; if(n<=0) n=1;"
#else
                    "long c = sysconf(_SC_NPROCESSORS_ONLN); n = (int)(c>0?c:1);"
#endif
                    "} cs_q_init(&P->q); P->n=n; P->th=("
#if defined(_WIN32)
                    "HANDLE*"
#else
                    "pthread_t*"
#endif
                    ")malloc(sizeof(*P->th)*n); for(int i=0;i<n;i++){ "
#if defined(_WIN32)
                    "P->th[i]=CreateThread(NULL,0,(LPTHREAD_START_ROUTINE)cs_worker,P,0,NULL);"
#else
                    "pthread_create(&P->th[i],NULL,cs_worker,P);"
#endif
                    " } }\n";
                o << "static void cs_pool_stop(cs_threadpool* P){ cs_q_stop(&P->q); for(int i=0;i<P->n;i++){ "
#if defined(_WIN32)
                    "WaitForSingleObject(P->th[i], INFINITE); CloseHandle(P->th[i]);"
#else
                    "pthread_join(P->th[i], NULL);"
#endif
                    " } free(P->th); }\n";
                o << "static cs_future cs_pool_submit(cs_threadpool* P, cs_task_fn fn, void* arg){ cs_future fut; cs_future_init(&fut); cs_task t; t.fn=fn; t.arg=arg; t.fut=&fut; cs_q_push(&P->q,t); return fut; }\n";

                // spawn! one-off
#if defined(_WIN32)
                o << "typedef struct { cs_task_fn fn; void* arg; } cs_spawn_arg; static DWORD WINAPI cs_spawn_thunk(LPVOID p){ cs_spawn_arg* a=(cs_spawn_arg*)p; a->fn(a->arg); free(a); return 0; }\n";
                o << "static void cs_spawn(cs_task_fn fn, void* arg){ cs_spawn_arg* a=(cs_spawn_arg*)malloc(sizeof(*a)); a->fn=fn; a->arg=arg; HANDLE h=CreateThread(NULL,0,cs_spawn_thunk,a,0,NULL); CloseHandle(h);} \n";
#else
                o << "typedef struct { cs_task_fn fn; void* arg; } cs_spawn_arg; static void* cs_spawn_thunk(void* p){ cs_spawn_arg* a=(cs_spawn_arg*)p; a->fn(a->arg); free(a); return 0; }\n";
                o << "static void cs_spawn(cs_task_fn fn, void* arg){ pthread_t t; cs_spawn_arg* a=(cs_spawn_arg*)malloc(sizeof(*a)); a->fn=fn; a->arg=arg; pthread_create(&t,NULL,cs_spawn_thunk,a); pthread_detach(t);} \n";
#endif

                // parfor + packetizer (load dithering/assigning)
                o << "typedef void (*cs_for_body)(int i, int end, void* arg);\n";
                o << "static void cs_packetize_and_run(cs_threadpool* P, int begin, int end, int grainsz, cs_for_body body, void* arg){ int N=end-begin; if (N<=0){ return; } if (grainsz<=0){ grainsz = (N / (P?P->n:1)); if (grainsz<1) grainsz=1; } int i=begin; \n"
                    "  if (P && P->n>1){ int pend=end; for(int s=i; s<pend; s+=grainsz){ int e = s+grainsz; if(e>pend) e=pend; cs_task_fn fn = (cs_task_fn)[](void* a)->void*{ struct _p{cs_for_body b; int s,e; void* arg;}; struct _p* p=(struct _p*)a; p->b(p->s,p->e,p->arg); free(p); return NULL; }; auto pack=(struct _p*)malloc(sizeof(*pack)); pack->b=body; pack->s=s; pack->e=e; pack->arg=arg; cs_future f = cs_pool_submit(P, fn, pack); (void)f; }\n"
                    "    /* barrier: submit an empty and wait by exhausting queue */ cs_task t; while (cs_q_pop(&P->q,&t)){ void* r = t.fn? t.fn(t.arg):NULL; if (t.fut) cs_future_set(t.fut,r); if (!t.fn) break; } }\n"
                    "  else { for (int s=i; s<end; s+=grainsz){ int e=s+grainsz; if(e>end) e=end; body(s,e,arg); } } }\n";

                // Channels (SPSC lock-free if atomics, else mutex)
                o << "#if " << (f.channels ? 1 : 0) << "\n";
                o << "typedef struct { size_t elem, cap; unsigned char* buf; "
                    "#if CS_HAS_ATOMICS\n atomic_size_t r; atomic_size_t w;\n"
                    "#else\n size_t r; size_t w; "
#if defined(_WIN32)
                    "CRITICAL_SECTION mu; CONDITION_VARIABLE cv;"
#else
                    "pthread_mutex_t mu; pthread_cond_t cv;"
#endif
                    "\n#endif } cs_chan;\n";
                o << "static cs_chan cs_chan_make(size_t elem, size_t cap){ cs_chan c; c.elem=elem; c.cap=cap?cap:64; c.buf=(unsigned char*)malloc(c.elem*c.cap); "
                    "#if CS_HAS_ATOMICS\n atomic_init(&c.r,0); atomic_init(&c.w,0);\n"
                    "#else\n c.r=c.w=0; "
#if defined(_WIN32)
                    "InitializeCriticalSection(&c.mu); InitializeConditionVariable(&c.cv);"
#else
                    "pthread_mutex_init(&c.mu,NULL); pthread_cond_init(&c.cv,NULL);"
#endif
                    "\n#endif return c; }\n";
                o << "static int cs_chan_send(cs_chan* c, const void* src, size_t n){ for(size_t i=0;i<n;i++){ "
                    "#if CS_HAS_ATOMICS\n size_t w = atomic_load_explicit(&c->w, memory_order_relaxed); size_t r = atomic_load_explicit(&c->r, memory_order_acquire); while (((w+1)%c->cap)==r){ r = atomic_load_explicit(&c->r, memory_order_acquire); }\n"
                    "memcpy(c->buf + (w*c->elem), (const unsigned char*)src + i*c->elem, c->elem); atomic_store_explicit(&c->w, (w+1)%c->cap, memory_order_release);\n"
                    "#else\n EnterCriticalSection(&c->mu); while (((c->w+1)%c->cap)==c->r){ SleepConditionVariableCS(&c->cv,&c->mu,INFINITE);} memcpy(c->buf + (c->w*c->elem), (const unsigned char*)src + i*c->elem, c->elem); c->w=(c->w+1)%c->cap; WakeConditionVariable(&c->cv); LeaveCriticalSection(&c->mu);\n"
                    "#endif } return 1; }\n";
                o << "static int cs_chan_recv(cs_chan* c, void* dst, size_t n){ for(size_t i=0;i<n;i++){ "
                    "#if CS_HAS_ATOMICS\n size_t r = atomic_load_explicit(&c->r, memory_order_relaxed); size_t w = atomic_load_explicit(&c->w, memory_order_acquire); while (r==w){ w = atomic_load_explicit(&c->w, memory_order_acquire); }\n"
                    "memcpy((unsigned char*)dst + i*c->elem, c->buf + (r*c->elem), c->elem); atomic_store_explicit(&c->r, (r+1)%c->cap, memory_order_release);\n"
                    "#else\n EnterCriticalSection(&c->mu); while (c->r==c->w){ SleepConditionVariableCS(&c->cv,&c->mu,INFINITE);} memcpy((unsigned char*)dst + i*c->elem, c->buf + (c->r*c->elem), c->elem); c->r=(c->r+1)%c->cap; WakeConditionVariable(&c->cv); LeaveCriticalSection(&c->mu);\n"
                    "#endif } return 1; }\n";
                o << "#endif\n";

                // Dynamic import loader
                o << "static void* cs_import_symbol(const char* lib, const char* sym){\n"
#if defined(_WIN32)
                    " HMODULE h = LoadLibraryA(lib); if (!h) return NULL; return (void*)GetProcAddress(h, sym);\n"
#else
                    " void* h = dlopen(lib, RTLD_LAZY); if(!h) return NULL; return dlsym(h, sym);\n"
#endif
                    "}\n";

                // Generic rendering hook (backend-agnostic)
                o << "typedef struct { void (*begin)(void* u); void (*draw)(void* u, const float* mvp16); void (*end)(void* u); void* user; } cs_renderer;\n";
                o << "static CS_FORCE_INLINE void cs_render_frame(cs_renderer* R, const float* mvp16){ if (!R) return; if (R->begin) R->begin(R->user); if (R->draw) R->draw(R->user, mvp16); if (R->end) R->end(R->user); }\n";

                o << "#endif /* CS_ULT_INCLUDED */\n";
                return o.str();
            }

            // ---------- DSL lowerings (conservative, regex-based) ----------
            static std::string lower_spawn(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // spawn! func(ptr)  -> cs_spawn((cs_task_fn)func, (void*)(ptr));
                std::regex re(R"(spawn!\s*([A-Za-z_]\w*)\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    std::string fn = m[1].str(); std::string arg = m[2].str();
                    out += "cs_spawn((cs_task_fn)" + fn + ", (void*)(" + arg + "))";
                }
                out.append(s, pos, std::string::npos); return out;
            }

            static std::string lower_async_await(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // async! func(ptr)  -> cs_pool_submit(&__cs_pool, (cs_task_fn)func, (void*)(ptr))
                std::regex reAsync(R"(async!\s*([A-Za-z_]\w*)\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // await!(expr)      -> cs_future_get(expr)
                std::regex reAwait(R"(await!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, reAsync)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_pool_submit(&__cs_pool, (cs_task_fn)" + m[1].str() + ", (void*)(" + m[2].str() + "))";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;
                while (search_from(s, pos, m, reAwait)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_future_get(" + m[1].str() + ")";
                }
                out.append(s, pos, std::string::npos); return out;
            }

            static std::string lower_parfor(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // parfor!(i, begin, end, grain, { body using i; })
                std::regex re(R"(parfor!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*,\s*\{([\s\S]*?)\}\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    std::string idx = m[1].str(), beg = m[2].str(), end = m[3].str(), grain = m[4].str(), body = m[5].str();
                    std::ostringstream gen;
                    gen << "{ cs_for_body __b = (cs_for_body)[](int __s,int __e,void* __a){ (void)__a; for(int " << idx << "=__s; " << idx << "<__e; ++" << idx << "){ " << body << " } }; cs_packetize_and_run(&__cs_pool, (" << beg << "), (" << end << "), (" << grain << "), __b, NULL); }";
                    out += gen.str();
                }
                out.append(s, pos, std::string::npos); return out;
            }

            static std::string lower_channels(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // chan!(name, T, cap) -> cs_chan name = cs_chan_make(sizeof(T), cap);
                std::regex reMake(R"(chan!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // send!(ch, T, expr)  -> do { T __tmp = (expr); cs_chan_send(&ch, &__tmp, 1); } while(0)
                std::regex reSend(R"(send!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // recv!(ch, T, dst)   -> cs_chan_recv(&ch, &(dst), 1)
                std::regex reRecv(R"(recv!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_][\w\.\->\[\]]*)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m = reMake)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_chan " + m[1].str() + " = cs_chan_make(sizeof(" + m[2].str() + "), (" + m[3].str() + "))";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;
                while (search_from(s, pos, m = reSend)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "do{ " + m[2].str() + " __tmp = (" + m[3].str() + "); cs_chan_send(&" + m[1].str() + ", &__tmp, 1); }while(0)";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;
                while (search_from(s, pos, m = reRecv)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_chan_recv(&" + m[1].str() + ", &(" + m[3].str() + "), 1)";
                }
                out.append(s, pos, std::string::npos); return out;
            }

            // Import/export wrappers
            static std::string lower_import_export(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // export! fn_sig {body} => CS_EXPORT fn_sig {body}
                std::regex reExp(R"(export!\s*([A-Za-z_][\s\S]*?\{[\s\S]*?\}))", std::regex::ECMAScript);
                // import!(lib, sym, type) => (type)cs_import_symbol(lib, sym)
                std::regex reImp(R"(import!\s*\(\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, reExp)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_EXPORT " + m[1].str();
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;
                while (search_from(s, pos, m, reImp)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "((" + m[3].str() + ")cs_import_symbol(\"" + m[1].str() + "\",\"" + m[2].str() + "\"))";
                }
                out.append(s, pos, std::string::npos); return out;
            }

            // Apply in stable order
            static std::string apply_lowerings(const std::string& src) {
                std::string t = lower_import_export(src);
                t = lower_spawn(t);
                t = lower_async_await(t);
                t = lower_parfor(t);
                t = lower_channels(t);
                return t;
            }

        } // namespace cs_ultimate

// ============================ Low-level Control Pack (append-only) ============================
// Zero-cost stack/heap/register control. Non-invasive: emits C prelude macros and conservative DSL lowerings.
// Wire-up (minimal):
//   1) Before softline_lower: auto ll = cs_lowlevel::apply_lowlevel(matchLowered);
//                             string lowered = softline_lower(ll, cfg.softline, hotFns, false);
//   2) After prelude(cfg.hardline): csrc += cs_lowlevel::prelude_lowlevel_addendum();

        namespace cs_lowlevel {

            // ---------- Prelude addendum: C-side zero-cost primitives ----------
            static std::string prelude_lowlevel_addendum() {
                std::ostringstream o;
                o << "/* --- Low-level Control Prelude Addendum --- */\n";
                o << "#ifndef CS_LOWLEVEL_INCLUDED\n#define CS_LOWLEVEL_INCLUDED 1\n";
                o << "#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n";
                o << "#if defined(_WIN32)\n  #include <windows.h>\n  #include <malloc.h>\n  #define CS_ALLOCA _alloca\n#else\n  #include <unistd.h>\n  #include <sys/mman.h>\n  #if __has_include(<alloca.h>)\n    #include <alloca.h>\n  #endif\n  #ifndef CS_ALLOCA\n    #define CS_ALLOCA alloca\n  #endif\n#endif\n";
                // Alignas/alignof spellings for C11
                o << "#ifndef CS_ALIGNAS\n"
                    "#  if defined(__STDC_VERSION__) && __STDC_VERSION__>=201112L\n"
                    "#    define CS_ALIGNAS _Alignas\n"
                    "#    define CS_ALIGNOF _Alignof\n"
                    "#  else\n"
                    "#    define CS_ALIGNAS(x)\n"
                    "#    define CS_ALIGNOF(x) sizeof(x)\n"
                    "#  endif\n"
                    "#endif\n";
                // Prefetch already exists in Ultimate Pack; define if absent
                o << "#ifndef CS_PREFETCH\n"
                    "#  if defined(__clang__) || defined(__GNUC__)\n"
                    "#    define CS_PREFETCH(p,wr,loc) __builtin_prefetch((p),(wr),(loc))\n"
                    "#  else\n"
                    "#    define CS_PREFETCH(p,wr,loc) do{(void)(p);(void)(wr);(void)(loc);}while(0)\n"
                    "#  endif\n"
                    "#endif\n";
                // Force-inline/noinline
                o << "#ifndef CS_FORCE_INLINE\n"
                    "#  if defined(_MSC_VER)\n"
                    "#    define CS_FORCE_INLINE __forceinline\n"
                    "#    define CS_NOINLINE __declspec(noinline)\n"
                    "#  else\n"
                    "#    define CS_FORCE_INLINE inline __attribute__((always_inline))\n"
                    "#    define CS_NOINLINE __attribute__((noinline))\n"
                    "#  endif\n"
                    "#endif\n";
                // Calling convention hints (best-effort; no-ops where unsupported)
                o << "#if defined(_MSC_VER)\n"
                    "  #define CS_CDECL __cdecl\n"
                    "  #define CS_STDCALL __stdcall\n"
                    "  #define CS_FASTCALL __fastcall\n"
                    "  #define CS_VECTORCALL __vectorcall\n"
                    "#else\n"
                    "  #define CS_CDECL\n"
                    "  #define CS_STDCALL\n"
                    "  #define CS_FASTCALL\n"
                    "  #define CS_VECTORCALL\n"
                    "#endif\n";
                // -------- Stack control --------
                o << "#define CS_STACK_ALLOC(T, count) ((T*)CS_ALLOCA(sizeof(T)*(size_t)(count)))\n";
                o << "#define CS_STACK_BYTES(n) ((void*)CS_ALLOCA((size_t)(n)))\n";
                o << "static CS_FORCE_INLINE void* cs_stack_top(void){ volatile int __x; return (void*)&__x; }\n";
                o << "static CS_FORCE_INLINE void* cs_frame_ptr(void){\n"
                    "#if defined(__clang__) || defined(__GNUC__)\n"
                    "  return __builtin_frame_address(0);\n"
                    "#elif defined(_MSC_VER)\n"
                    "  return _AddressOfReturnAddress();\n"
                    "#else\n"
                    "  return NULL;\n"
                    "#endif }\n";
                o << "static CS_FORCE_INLINE void* cs_return_addr(void){\n"
                    "#if defined(__clang__) || defined(__GNUC__)\n"
                    "  return __builtin_return_address(0);\n"
                    "#elif defined(_MSC_VER)\n"
                    "  return _ReturnAddress();\n"
                    "#else\n"
                    "  return NULL;\n"
                    "#endif }\n";
                // -------- Heap control (selectable) --------
                o << "#ifndef CS_MALLOC\n#define CS_MALLOC(n) malloc((size_t)(n))\n#endif\n";
                o << "#ifndef CS_CALLOC\n#define CS_CALLOC(c,s) calloc((size_t)(c),(size_t)(s))\n#endif\n";
                o << "#ifndef CS_REALLOC\n#define CS_REALLOC(p,n) realloc((p),(size_t)(n))\n#endif\n";
                o << "#ifndef CS_FREE\n#define CS_FREE(p) free((p))\n#endif\n";
                // Arena bridging (if Ultimate arenas present)
                o << "#if defined(CS_ULT_INCLUDED)\n"
                    "static CS_FORCE_INLINE void* cs_arena_alloc_bytes(cs_arena* A, size_t n, size_t align){ return cs_arena_push(A, n, align?align:8); }\n"
                    "#endif\n";
                // -------- Page allocator / protection --------
                o << "static CS_FORCE_INLINE size_t cs_page_size(void){\n"
                    "#if defined(_WIN32)\n"
                    "  SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwPageSize;\n"
                    "#else\n"
                    "  long p = sysconf(_SC_PAGESIZE); return (size_t)(p>0?p:4096);\n"
                    "#endif }\n";
                o << "static void* cs_pages_alloc(size_t nbytes, int commit, int large){\n"
                    "#if defined(_WIN32)\n"
                    "  DWORD flAlloc = MEM_RESERVE | (commit?MEM_COMMIT:0) | (large?MEM_LARGE_PAGES:0);\n"
                    "  return VirtualAlloc(NULL, nbytes, flAlloc, PAGE_READWRITE);\n"
                    "#else\n"
                    "  int flags = MAP_PRIVATE|MAP_ANON; (void)large; \n"
                    "  void* p = mmap(NULL, nbytes, PROT_READ|PROT_WRITE, flags, -1, 0);\n"
                    "  return (p==MAP_FAILED)?NULL:p;\n"
                    "#endif }\n";
                o << "static int cs_pages_free(void* p, size_t nbytes){\n"
                    "#if defined(_WIN32)\n"
                    "  (void)nbytes; return VirtualFree(p, 0, MEM_RELEASE)!=0;\n"
                    "#else\n"
                    "  return (munmap(p, nbytes)==0);\n"
                    "#endif }\n";
                o << "enum { CS_PROT_RW=0, CS_PROT_RO=1, CS_PROT_RX=2, CS_PROT_NO=3 };\n";
                o << "static int cs_pages_protect(void* p, size_t nbytes, int prot){\n"
                    "#if defined(_WIN32)\n"
                    "  DWORD newp = PAGE_READWRITE; if(prot==1) newp=PAGE_READONLY; else if(prot==2) newp=PAGE_EXECUTE_READ; else if(prot==3) newp=PAGE_NOACCESS;\n"
                    "  DWORD oldp=0; return VirtualProtect(p, nbytes, newp, &oldp)!=0;\n"
                    "#else\n"
                    "  int pr=PROT_READ|PROT_WRITE; if(prot==1) pr=PROT_READ; else if(prot==2) pr=PROT_READ|PROT_EXEC; else if(prot==3) pr=PROT_NONE;\n"
                    "  return (mprotect(p, nbytes, pr)==0);\n"
                    "#endif }\n";
                // -------- Register control (best-effort) --------
                o << "#if defined(__GNUC__)\n"
                    "#  define CS_REG_T(type,name,regstr) register type name __asm__(regstr)\n"
                    "#else\n"
                    "#  define CS_REG_T(type,name,regstr) type name /* reg hint unsupported; no-op */\n"
                    "#endif\n";
                // -------- Alignment helpers --------
                o << "#if defined(__clang__) || defined(__GNUC__)\n"
                    "#  define CS_ASSUME_ALIGNED(p,a) ((typeof(p))__builtin_assume_aligned((p),(a)))\n"
                    "#else\n"
                    "#  define CS_ASSUME_ALIGNED(p,a) (p)\n"
                    "#endif\n";
                o << "#endif /* CS_LOWLEVEL_INCLUDED */\n";
                return o.str();
            }

            // ---------- DSL lowerings (regex-based, conservative) ----------
            static std::string lower_stack_heap_regs(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());

                // stackalloc!(T, n)           -> ((T*)CS_ALLOCA(sizeof(T)*(n)))
                std::regex reStack(R"(stackalloc!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // assume_aligned!(p, a)       -> CS_ASSUME_ALIGNED((p), (a))
                std::regex reAssume(R"(assume_aligned!\s*\(\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // prefetch!(p[, wr, loc])     -> CS_PREFETCH((p), wr, loc)
                std::regex rePref3(R"(prefetch!\s*\(\s*([\s\S]*?)\s*,\s*([01])\s*,\s*([0-3])\s*\))", std::regex::ECMAScript);
                std::regex rePref1(R"(prefetch!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // reg!(type, name, "rx")      -> CS_REG_T(type, name, "rx")
                std::regex reReg(R"(reg!\s*\(\s*([A-Za-z_][\w\s\*]+?)\s*,\s*([A-Za-z_]\w*)\s*,\s*\"([^\"]+)\"\s*\))", std::regex::ECMAScript);
                // pagealloc!(n)               -> cs_pages_alloc((n),1,0)
                std::regex rePAlloc(R"(pagealloc!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // pagefree!(p, n)             -> cs_pages_free((p),(n))
                std::regex rePFree(R"(pagefree!\s*\(\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                // protect!(p, n, mode)        -> cs_pages_protect((p),(n),mode)
                std::regex rePProt(R"(protect!\s*\(\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);

                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, reStack)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "(( " + m[1].str() + "*)CS_ALLOCA(sizeof(" + m[1].str() + ")*(" + m[2].str() + ")))";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, reAssume)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_ASSUME_ALIGNED((" + m[1].str() + "),(" + m[2].str() + "))";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, rePref3)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_PREFETCH((" + m[1].str() + ")," + m[2].str() + "," + m[3].str() + ")";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, rePref1)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_PREFETCH((" + m[1].str() + "),0,3)";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, reReg)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    // Expands to a declaration; leave as-is in expression contexts by wrapping in a do{}while(0) when needed by user.
                    out += "CS_REG_T(" + m[1].str() + "," + m[2].str() + ",\"" + m[3].str() + "\")";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, rePAlloc)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_pages_alloc((" + m[1].str() + "),1,0)";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, rePFree)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_pages_free((" + m[1].str() + "),(" + m[2].str() + "))";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;

                while (search_from(s, pos, m, rePProt)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_pages_protect((" + m[1].str() + "),(" + m[2].str() + "),(" + m[3].str() + "))";
                }
                out.append(s, pos, std::string::npos);

                return out;
            }

            // Apply pack lowerings (call before softline_lower)
            static std::string apply_lowlevel(const std::string& src) {
                return lower_stack_heap_regs(src);
            }
        } // namespace cs_lowlevel

// ============================ Resilience & Continuations Pack (append-only) ============================
// Zero-cost style (inline/macros) runtime helpers + conservative DSL lowerings.
// Guarantees: never aborts; all errors are soft/log-only. Append-only.

        namespace cs_resilience {

            // ---------- Prelude addendum (C side) ----------
            static std::string prelude_resilience_addendum() {
                std::ostringstream o;
                o << "/* --- Resilience & Continuations Prelude Addendum --- */\n";
                o << "#ifndef CS_RESILIENCE_INCLUDED\n#define CS_RESILIENCE_INCLUDED 1\n";
                o << "#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <string.h>\n#include <time.h>\n";
                o << "#if defined(_WIN32)\n  #include <windows.h>\n#else\n  #include <pthread.h>\n  #include <sys/time.h>\n  #include <unistd.h>\n#endif\n";

                // ---- Soft errors (never abort) ----
                o << "static void cs_soft_error(const char* msg){ if(msg){ fprintf(stderr, \"[soft-error] %s\\n\", msg); fflush(stderr);} }\n";
                o << "#define CS_ENSURE_SOFT(cond,msg) do{ if(!(cond)){ cs_soft_error(msg); } }while(0)\n";

                // ---- Time helpers ----
                o << "static uint64_t cs_now_ms(void){\n"
                    "#if defined(_WIN32)\n LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((1000.0*c.QuadPart)/f.QuadPart);\n"
                    "#else\n struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000ULL + (uint64_t)(ts.tv_nsec/1000000ULL);\n"
                    "#endif }\n";

                // ---- Checksum (FNV-1a 64-bit, fast, single-pass) ----
                o << "static uint64_t cs_checksum64(const void* data, size_t len){ const unsigned char* p=(const unsigned char*)data; uint64_t h=1469598103934665603ULL; const uint64_t F=1099511628211ULL; for(size_t i=0;i<len;i++){ h^=p[i]; h*=F; } return h; }\n";
                o << "#define CS_CHECKSUM64(ptr,len) cs_checksum64((const void*)(ptr),(size_t)(len))\n";
                o << "static void cs_checksum_guard_set(const void* p, size_t n, uint64_t* out){ if(out) *out = cs_checksum64(p,n); }\n";
                o << "static int  cs_checksum_guard_verify(const void* p, size_t n, uint64_t expect){ uint64_t c = cs_checksum64(p,n); if (c!=expect){ cs_soft_error(\"checksum mismatch\"); return 0; } return 1; }\n";

                // ---- Checkpoints (ring buffer) ----
                o << "typedef struct { const char* tag; const char* file; int line; uint64_t t_ms; } cs_checkpoint_t;\n";
                o << "enum{ CS_CK_CAP=128 };\n";
                o << "static cs_checkpoint_t cs_ck_ring[CS_CK_CAP]; static unsigned cs_ck_head=0u;\n";
                o << "static void cs_checkpoint_hit(const char* tag, const char* file, int line){ cs_checkpoint_t e; e.tag=tag; e.file=file; e.line=line; e.t_ms=cs_now_ms(); cs_ck_ring[cs_ck_head++%CS_CK_CAP]=e; }\n";
                o << "#define CS_CHECKPOINT(tag) cs_checkpoint_hit((tag), __FILE__, __LINE__)\n";

                // ---- Heartbeat watchdog (anti-freeze; non-fatal) ----
                o << "static volatile uint64_t cs_hb_last=0ULL; static unsigned cs_hb_timeout_ms=0u; static int cs_hb_on=0;\n";
#if defined(_WIN32)
                o << "static DWORD WINAPI cs_watchdog_th(LPVOID){ while(cs_hb_on){ uint64_t now=cs_now_ms(); uint64_t last=cs_hb_last; if (cs_hb_timeout_ms && last && (now>last) && (now-last>cs_hb_timeout_ms)){ cs_soft_error(\"watchdog: heartbeat gap\"); cs_hb_last=now; } Sleep(50); } return 0; }\n";
                o << "static void cs_watchdog_start(unsigned timeout_ms){ if(cs_hb_on) return; cs_hb_timeout_ms=timeout_ms; cs_hb_last=cs_now_ms(); cs_hb_on=1; HANDLE h=CreateThread(NULL,0,cs_watchdog_th,NULL,0,NULL); if(h) CloseHandle(h); }\n";
                o << "static void cs_watchdog_stop(void){ cs_hb_on=0; }\n";
#else
                o << "static void* cs_watchdog_th(void*){ while(cs_hb_on){ uint64_t now=cs_now_ms(); uint64_t last=cs_hb_last; if (cs_hb_timeout_ms && last && (now>last) && (now-last>cs_hb_timeout_ms)){ cs_soft_error(\"watchdog: heartbeat gap\"); cs_hb_last=now; } usleep(50*1000); } return NULL; }\n";
                o << "static void cs_watchdog_start(unsigned timeout_ms){ if(cs_hb_on) return; cs_hb_timeout_ms=timeout_ms; cs_hb_last=cs_now_ms(); cs_hb_on=1; pthread_t t; pthread_create(&t,NULL,cs_watchdog_th,NULL); pthread_detach(t); }\n";
                o << "static void cs_watchdog_stop(void){ cs_hb_on=0; }\n";
#endif
                o << "static void cs_beat(void){ cs_hb_last = cs_now_ms(); }\n";
                o << "#define CS_HEARTBEAT() cs_beat()\n";

                // ---- Timed mutex (anti-lock) ----
#if defined(_WIN32)
                o << "typedef struct { CRITICAL_SECTION cs; } cs_res_mutex;\n";
                o << "static void cs_res_mutex_init(cs_res_mutex* m){ InitializeCriticalSection(&m->cs); }\n";
                o << "static int  cs_res_mutex_lock_timeout(cs_res_mutex* m, unsigned timeout_ms){ unsigned waited=0; while(waited<=timeout_ms){ if (TryEnterCriticalSection(&m->cs)) return 1; Sleep(1); waited+=1; } cs_soft_error(\"lock timeout\"); return 0; }\n";
                o << "static void cs_res_mutex_unlock(cs_res_mutex* m){ LeaveCriticalSection(&m->cs); }\n";
#else
                o << "typedef struct { pthread_mutex_t mu; } cs_res_mutex;\n";
                o << "static void cs_res_mutex_init(cs_res_mutex* m){ pthread_mutex_init(&m->mu, NULL); }\n";
                o << "static int  cs_res_mutex_lock_timeout(cs_res_mutex* m, unsigned timeout_ms){\n"
                    "#if defined(_POSIX_TIMEOUTS) && _POSIX_TIMEOUTS>0\n"
                    "  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t ns = (uint64_t)ts.tv_sec*1000000000ULL + ts.tv_nsec + (uint64_t)timeout_ms*1000000ULL; struct timespec dl = { (time_t)(ns/1000000000ULL), (long)(ns%1000000000ULL) }; int rc=pthread_mutex_timedlock(&m->mu, &dl); if(rc==0) return 1; cs_soft_error(\"lock timeout\"); return 0;\n"
                    "#else\n"
                    "  unsigned waited=0; while(waited<=timeout_ms){ if(pthread_mutex_trylock(&m->mu)==0) return 1; usleep(1000); waited+=1; } cs_soft_error(\"lock timeout\"); return 0;\n"
                    "#endif }\n";
                o << "static void cs_res_mutex_unlock(cs_res_mutex* m){ pthread_mutex_unlock(&m->mu); }\n";
#endif
                o << "#define CS_SAFE_LOCK(m,ms,body) do{ if(cs_res_mutex_lock_timeout((m),(ms))){ body; cs_res_mutex_unlock((m)); } else { /* continue */ } }while(0)\n";

                // ---- Convenience toggles ----
                o << "#ifndef CS_WATCHDOG_DEFAULT_MS\n#define CS_WATCHDOG_DEFAULT_MS 2000u\n#endif\n";
                o << "static void cs_resilience_init(void){ cs_watchdog_start(CS_WATCHDOG_DEFAULT_MS); }\n";
                o << "static void cs_resilience_shutdown(void){ cs_watchdog_stop(); }\n";

                o << "#endif /* CS_RESILIENCE_INCLUDED */\n";
                return o.str();
            }

            // ---------- DSL lowerings (conservative, regex-based) ----------
            static std::string lower_dead_blocks(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // dead! { ... }  -> removed entirely
                std::regex re(R"(dead!\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m); // append prefix only (strip match)
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            static std::string lower_checkpoint(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // checkpoint!("tag") -> CS_CHECKPOINT("tag")
                std::regex re(R"(checkpoint!\s*\(\s*\"([^\"]*)\"\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_CHECKPOINT(\"" + m[1].str() + "\")";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            static std::string lower_checksum(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // checksum!(ptr,len) -> CS_CHECKSUM64(ptr,len)
                std::regex re(R"(checksum!\s*\(\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_CHECKSUM64((" + m[1].str() + "),(" + m[2].str() + "))";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            static std::string lower_heartbeat(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // beat!() -> CS_HEARTBEAT()
                std::regex re(R"(beat!\s*\(\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_HEARTBEAT()";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            static std::string lower_ensure(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // ensure!(cond, "msg") -> CS_ENSURE_SOFT(cond,"msg")
                std::regex re(R"(ensure!\s*\(\s*([\s\S]*?)\s*,\s*\"([^\"]*)\"\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_ENSURE_SOFT((" + m[1].str() + "),\"" + m[2].str() + "\")";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            static std::string lower_safelock(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // safelock!(mtx, timeout_ms, { body }) -> CS_SAFE_LOCK(&mtx,timeout_ms, body )
                std::regex re(R"(safelock!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*,\s*\{([\s\S]*?)\}\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_SAFE_LOCK(&" + m[1].str() + "," + m[2].str() + ", " + m[3].str() + " )";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // Apply in stable order (dead-code removal first)
            static std::string apply_lowerings(const std::string& src) {
                std::string t = lower_dead_blocks(src);
                t = lower_checkpoint(t);
                t = lower_checksum(t);
                t = lower_heartbeat(t);
                t = lower_ensure(t);
                t = lower_safelock(t);
                return t;
            }

        } // namespace cs_resilience

// ============================ Optimization & Algebraic Pack (append-only) ============================
// Portable attributes/macros + conservative DSL lowerings and max-speed build helper.
// All transforms are opt-in and scoped to explicit DSL markers.

        namespace cs_optimax {

            // ---------- Prelude addendum: cross-compiler perf attributes, unroll, fences, math ----------
            static std::string prelude_optimax_addendum() {
                std::ostringstream o;
                o << "/* --- Optimization & Algebraic Prelude Addendum --- */\n";
                o << "#ifndef CS_OPTIMAX_INCLUDED\n#define CS_OPTIMAX_INCLUDED 1\n";
                o << "#include <stdint.h>\n#include <stddef.h>\n";
                o << "#if defined(_WIN32)\n  #include <windows.h>\n#else\n  #include <sched.h>\n  #include <unistd.h>\n#endif\n";
                o << "#if !defined(likely)\n  #if defined(__GNUC__)||defined(__clang__)\n    #define likely(x)   __builtin_expect(!!(x),1)\n"
                    "    #define unlikely(x) __builtin_expect(!!(x),0)\n"
                    "  #else\n    #define likely(x)   (x)\n    #define unlikely(x) (x)\n  #endif\n#endif\n";
                // Force-inline, flatten, pure/const
                o << "#if defined(_MSC_VER)\n"
                    "  #define CS_ALWAYS_INLINE __forceinline\n"
                    "  #define CS_FLATTEN /* no-op on MSVC */\n"
                    "  #define CS_PURE /* no-op */\n"
                    "  #define CS_CONST /* no-op */\n"
                    "  #define CS_ASSUME(x) __assume(x)\n"
                    "#else\n"
                    "  #define CS_ALWAYS_INLINE inline __attribute__((always_inline))\n"
                    "  #define CS_FLATTEN __attribute__((flatten))\n"
                    "  #define CS_PURE __attribute__((pure))\n"
                    "  #define CS_CONST __attribute__((const))\n"
                    "  #define CS_ASSUME(x) do{ if(!(x)) __builtin_unreachable(); }while(0)\n"
                    "#endif\n";
                // Tailcall marker (best-effort; keeps tail position explicit)
                o << "#define CS_TAILCALL(expr) return (expr)\n";
                // Unroll (portable pragmas)
                o << "#define CS_UNROLL(N) \\\n"
                    "  _Pragma(\"clang loop unroll_count(\" #N \")\") \\\n"
                    "  _Pragma(\"GCC unroll \" #N) \\\n"
                    "  _Pragma(\"loop(unroll(\" #N \"))\")\n";
                // Portable fences/yield
                o << "#if __STDC_VERSION__>=201112L && !defined(__STDC_NO_ATOMICS__)\n"
                    "  #include <stdatomic.h>\n"
                    "  #define CS_FENCE_ACQ() atomic_thread_fence(memory_order_acquire)\n"
                    "  #define CS_FENCE_REL() atomic_thread_fence(memory_order_release)\n"
                    "  #define CS_FENCE_SEQ() atomic_thread_fence(memory_order_seq_cst)\n"
                    "#else\n"
                    "  #define CS_FENCE_ACQ() do{}while(0)\n"
                    "  #define CS_FENCE_REL() do{}while(0)\n"
                    "  #define CS_FENCE_SEQ() do{}while(0)\n"
                    "#endif\n";
                o << "#if defined(_WIN32)\n"
                    "  #define CS_YIELD() Sleep(0)\n"
                    "#else\n"
                    "  #define CS_YIELD() sched_yield()\n"
                    "#endif\n";
                // Math fast-path toggles (guarded)
                o << "#if defined(CS_FAST_MATH)\n"
                    "  #if defined(__GNUC__)||defined(__clang__)\n"
                    "    #pragma STDC FENV_ACCESS OFF\n"
                    "  #endif\n"
                    "#endif\n";
                o << "#endif /* CS_OPTIMAX_INCLUDED */\n";
                return o.str();
            }

            // ---------- Helpers: tiny integer expression folder (safe subset) ----------
            struct Tok {
                enum K { NUM, OP, L, R, END } k; long long v; char op;
            };
            static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
            static int prec(char op) { return (op == '+' || op == '-') ? 1 : (op == '*' || op == '/' || op == '%') ? 2 : -1; }
            static bool isop(char c) { return c == '+' || c == '-' || c == '*' || c == '/' || c == '%'; }

            static std::vector<Tok> lex_i64(const std::string& s) {
                std::vector<Tok> t; size_t i = 0, n = s.size();
                while (i < n) {
                    if (is_space(s[i])) { ++i; continue; }
                    if (s[i] == '(') { t.push_back({ Tok::L,0,0 }); ++i; continue; }
                    if (s[i] == ')') { t.push_back({ Tok::R,0,0 }); ++i; continue; }
                    if (isop(s[i])) { t.push_back({ Tok::OP,0,s[i++] }); continue; }
                    // number: dec or 0x...
                    bool neg = false; if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
                    int base = 10;
                    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
                    size_t j = i;
                    while (j < n && (std::isxdigit((unsigned char)s[j]) || (base == 10 && std::isdigit((unsigned char)s[j])))) ++j;
                    if (j == i) { t.clear(); break; }
                    long long v = 0;
                    try {
                        v = std::stoll(s.substr(i, j - i), nullptr, base);
                    }
                    catch (...) { t.clear(); break; }
                    if (neg) v = -v;
                    t.push_back({ Tok::NUM,v,0 }); i = j;
                }
                t.push_back({ Tok::END,0,0 });
                return t;
            }

            static bool fold_i64(const std::string& expr, long long& out) {
                auto toks = lex_i64(expr);
                if (toks.empty()) return false;
                std::vector<long long> stv; std::vector<char> sto;
                auto apply = [&]() {
                    if (sto.empty() || stv.size() < 2) return false;
                    char op = sto.back(); sto.pop_back();
                    long long b = stv.back(); stv.pop_back();
                    long long a = stv.back(); stv.pop_back();
                    long long r = 0;
                    switch (op) {
                    case '+': r = a + b; break;
                    case '-': r = a - b; break;
                    case '*': r = a * b; break;
                    case '/': if (b == 0) return false; r = a / b; break;
                    case '%': if (b == 0) return false; r = a % b; break;
                    default: return false;
                    }
                    stv.push_back(r); return true;
                    };
                size_t i = 0;
                while (i < toks.size()) {
                    Tok tk = toks[i++];
                    if (tk.k == Tok::NUM) { stv.push_back(tk.v); }
                    else if (tk.k == Tok::L) { sto.push_back('('); }
                    else if (tk.k == Tok::R) {
                        while (!sto.empty() && sto.back() != '(') { if (!apply()) return false; }
                        if (sto.empty() || sto.back() != '(') return false;
                        sto.pop_back();
                    }
                    else if (tk.k == Tok::OP) {
                        while (!sto.empty() && sto.back() != '(' && prec(sto.back()) >= prec(tk.op)) {
                            if (!apply()) return false;
                        }
                        sto.push_back(tk.op);
                    }
                    else if (tk.k == Tok::END) { break; }
                }
                while (!sto.empty()) { if (sto.back() == '(') return false; if (!apply()) return false; }
                if (stv.size() != 1) return false;
                out = stv.back(); return true;
            }

            // ---------- DSL lowerings ----------
            using namespace cs_regex_wrap;

            // inline! fn name(args) -> Ret { .. }  => fn name(args) -> CS_ALWAYS_INLINE Ret { .. }
            // flatten! fn name(args) -> Ret { .. } => fn name(args) -> CS_FLATTEN Ret { .. }
            static std::string lower_inline_flatten(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex r1(R"(\binline!\s*fn\s+([A-Za-z_]\w*)\s*\()", std::regex::ECMAScript);
                std::regex r2(R"(\bflatten!\s*fn\s+([A-Za-z_]\w*)\s*\()", std::regex::ECMAScript);
                cmatch m; size_t pos = 0, last = 0;
                // Replace the marker; we will inject attribute into the return type via '-> RET'
                while (search_from(s, pos, m, r1)) {
                    append_prefix(out, s, last, m);
                    out += "fn " + m[1].str() + "(";
                    last = pos;
                }
                out.append(s, last, std::string::npos);
                s.swap(out); out.clear(); out.reserve(s.size()); pos = 0; last = 0;

                // Inject attribute token into return type: "-> RET" -> "-> CS_ALWAYS_INLINE RET"
                std::regex inj1(R"(\-\>\s*([^\{\n;]+))");
                while (search_from(s, pos, m, inj1)) {
                    append_prefix(out, s, last, m);
                    std::string ret = m[1].str();
                    // Only rewrite if this occurrence belonged to an inline! block; heuristic: keep all safe
                    out += "-> CS_ALWAYS_INLINE " + ret;
                    last = pos;
                }
                out.append(s, last, std::string::npos);
                s.swap(out); out.clear(); out.reserve(s.size()); pos = 0; last = 0;

                // flatten!
                while (search_from(s, pos, m, r2)) {
                    append_prefix(out, s, last, m);
                    out += "fn " + m[1].str() + "(";
                    last = pos;
                }
                out.append(s, last, std::string::npos);
                s.swap(out); out.clear(); out.reserve(s.size()); pos = 0; last = 0;

                // Inject CS_FLATTEN similarly
                while (search_from(s, pos, m, inj1)) {
                    append_prefix(out, s, last, m);
                    std::string ret = m[1].str();
                    out += "-> CS_FLATTEN " + ret;
                    last = pos;
                }
                out.append(s, last, std::string::npos);
                return out;
            }

            // tail!(expr) => CS_TAILCALL(expr)
            static std::string lower_tailcall(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex r(R"(tail!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, r)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "CS_TAILCALL(" + m[1].str() + ")";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // unroll!(N) => inject portable unroll pragmas (applies to the next loop)
            static std::string lower_unroll(const std::string& src) {
                std::string s = src;
                std::regex r(R"(unroll!\s*\(\s*([0-9]+)\s*\))");
                s = std::regex_replace(s, r,
                    "#pragma clang loop unroll_count(\\1)\n#pragma GCC unroll \\1\n#pragma loop(unroll(\\1))");
                return s;
            }

            // opt!(expr) => algebraic simplifications inside the wrapper only (conservative)
            static std::string lower_opt_expr(const std::string& src) {
                using std::string;
                string s = src, out; out.reserve(s.size());
                std::regex r(R"(opt!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                auto simp = [](string e)->string {
                    // trivial, side-effect free identities (order matters)
                    // x*1 -> x ; 1*x -> x ; x+0 -> x ; 0+x -> x ; x-0 -> x ; x/1 -> x ; x^0->x ; x|0->x ; x&-1->x ; x*0->0 ; 0*x->0
                    // Keep conservative identifiers
                    struct Rule { std::regex re; const char* rep; };
                    static const Rule R[] = {
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\*\s*1\b)"), "$1" },
                        { std::regex(R"(\b1\s*\*\s*(\b[A-Za-z_][\w\.\->\[\]]*))"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\+\s*0\b)"), "$1" },
                        { std::regex(R"(\b0\s*\+\s*(\b[A-Za-z_][\w\.\->\[\]]*))"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\-\s*0\b)"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\/\s*1\b)"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\^\s*0\b)"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\|\s*0\b)"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*&\s*-?1\b)"), "$1" },
                        { std::regex(R"((\b[A-Za-z_][\w\.\->\[\]]*)\s*\*\s*0\b)"), "0" },
                        { std::regex(R"(\b0\s*\*\s*(\b[A-Za-z_][\w\.\->\[\]]*))"), "0" },
                    };
                    string t = e; for (int pass = 0; pass < 2; ++pass) { for (const auto& rr : R) { t = std::regex_replace(t, rr.re, rr.rep); } }
                    return t;
                    };
                while (search_from(s, pos, m, r)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    std::string inner = m[1].str();
                    out += "(" + simp(inner) + ")";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // foldi!(pure-integer-expression) => literal 64-bit integer (safe subset)
            static std::string lower_foldi(const std::string& src) {
                std::string s = src, out; out.reserve(s.size());
                std::regex r(R"(foldi!\s*\(\s*([0-9xXa-fA-F\(\)\+\-\*/%\s]+)\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, r)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    long long v = 0; if (fold_i64(m[1].str(), v)) { out += std::to_string(v); }
                    else { out += "(" + m[1].str() + ")"; }
                }
                out.append(s, pos, std::string::npos);
                return out;
            }

            // Apply in stable order
            static std::string apply_lowerings(const std::string& src) {
                std::string t = lower_inline_flatten(src);
                t = lower_tailcall(t);
                t = lower_unroll(t);
                t = lower_opt_expr(t);
                t = lower_foldi(t);
                return t;
            }

            // ---------- Max-speed build command helper (portable) ----------
            // Drop-in alternative to build_cmd(...). Honors same signature, with extra speed flags.
            static std::string build_cmd_speed(const Config& cfg, const std::string& cc, const std::string& cpath, const std::string& out,
                bool defineProfile = false, const std::string& c_translation_unit_for_scan = std::string()) {
                (void)c_translation_unit_for_scan;
                std::vector<std::string> cmd; cmd.push_back(cc);
                bool msvc = (cc == "cl" || cc == "clang-cl");
                auto add = [&](const std::string& s) { cmd.push_back(s); };

                if (msvc) {
                    add("/nologo");
                    // Favor speed
                    add("/O2"); add("/Ot"); add("/GL"); add("/Gw"); add("/Gy");
                    add("/favor:INTEL64"); // best-effort
                    add("/fp:fast");
                    if (cfg.hardline || cfg.strict) { add("/Wall"); add("/WX"); }
                    if (cfg.hardline) add("/DCS_HARDLINE=1");
                    if (defineProfile) add("/DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) add("/D" + d);
                    for (auto& i : cfg.incs)    add("/I" + i);
                    add(cpath);
                    add("/Fe:" + out);
                    // Link: fold-away dead code
                    add("/link /OPT:REF /OPT:ICF");
                    for (auto& lp : cfg.libpaths) add("/link /LIBPATH:\"" + lp + "\"");
                    for (auto& l : cfg.links) { std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib"; add("/link " + lib); }
                }
                else {
                    // Baseline std
                    add("-std=c11");
                    // Speed flags
                    add("-O3");
                    if (cfg.lto) add("-flto");
                    add("-fomit-frame-pointer");
                    add("-fstrict-aliasing");
                    add("-ffunction-sections"); add("-fdata-sections");
                    add("-fno-math-errno"); add("-fno-signed-zeros");
                    // Optional fast-math if user defines CS_FAST_MATH via @define or CLI
                    // (kept under macro to preserve correctness unless opted-in)
                    add("-DNDEBUG");
                    if (cfg.hardline) { add("-Wall"); add("-Wextra"); add("-Werror"); add("-Wconversion"); add("-Wsign-conversion"); }
                    if (cfg.hardline) add("-DCS_HARDLINE=1");
                    if (defineProfile) add("-DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) add("-D" + d);
                    for (auto& i : cfg.incs)    add("-I" + i);
                    add(cpath);
                    add("-o"); add(out);
                    for (auto& lp : cfg.libpaths) add("-L" + lp);
                    for (auto& l : cfg.links)     add("-l" + l);
                    // Link-time dead code elimination
#if defined(__APPLE__)
                    add("-Wl,-dead_strip");
#else
                    add("-Wl,--gc-sections");
#endif
                }

                std::string full;
                for (size_t i = 0; i < cmd.size(); ++i) {
                    if (i) full.push_back(' ');
                    bool q = cmd[i].find(' ') != std::string::npos;
                    if (q) full.push_back('"'); full += cmd[i]; if (q) full.push_back('"');
                }
                return full;
            }

        } // namespace cs_optimax

        namespace cs_annotations {

            // ------------ C prelude addendum (annotations + tiny regex engine) ------------
            static std::string prelude_annotations_addendum() {
                return R"(
/* --- Annotations & Regex Synthesis Addendum --- */
#ifndef CS_ANNOTATIONS_INCLUDED
#define CS_ANNOTATIONS_INCLUDED 1
#include <stddef.h>
#include <string.h>

/* Flat annotation triplets (kind,target,key=value) + line */
typedef struct {
    const char* kind;      /* e.g., "fn","var","type","file","custom" */
    const char* target;    /* target name; may be "" (file/global) */
    const char* key;       /* annotation key */
    const char* value;     /* annotation value */
    int line;              /* source line of the @annot directive */
} CS_Annotation;

/* Externs (arrays are emitted per-source by the compiler front-end) */
extern const CS_Annotation cs_annotations[];
extern const unsigned cs_annotations_count;

/* Lookups (non-allocating, safe if arrays are empty) */
static int cs_anno_has(const char* kind, const char* target, const char* key) {
    if (!cs_annotations) return 0;
    for (unsigned i=0;i<cs_annotations_count;i++){
        const CS_Annotation* a = &cs_annotations[i];
        if ((!kind   || strcmp(a->kind,   kind  )==0) &&
            (!target || strcmp(a->target, target)==0) &&
            (!key    || strcmp(a->key,    key   )==0)) {
            return 1;
        }
    }
    return 0;
}
static const char* cs_anno_get(const char* kind, const char* target, const char* key) {
    if (!cs_annotations) return NULL;
    for (unsigned i=0;i<cs_annotations_count;i++){
        const CS_Annotation* a = &cs_annotations[i];
        if ((!kind   || strcmp(a->kind,   kind  )==0) &&
            (!target || strcmp(a->target, target)==0) &&
            (!key    || strcmp(a->key,    key   )==0)) {
            return a->value ? a->value : "";
        }
    }
    return NULL;
}
static unsigned cs_anno_count(const char* kind, const char* target) {
    unsigned n=0;
    if (!cs_annotations) return 0;
    for (unsigned i=0;i<cs_annotations_count;i++){
        const CS_Annotation* a = &cs_annotations[i];
        if ((!kind   || strcmp(a->kind,   kind  )==0) &&
            (!target || strcmp(a->target, target)==0)) n++;
    }
    return n;
}

/* Tiny portable regex: supports ., *, ^, $ (greedy); backslash escapes for ^$.* */
static int cs_rx_match_here(const char* re, const char* text);
static int cs_rx_match_star(int c, const char* re, const char* text) {
    /* c '*' followed by re, try all tails (greedy) */
    do { if (cs_rx_match_here(re, text)) return 1; }
    while (*text && (c=='.' || *text++==c));
    return 0;
}
static int cs_rx_match_here(const char* re, const char* text) {
    if (re[0] == '\0') return 1;
    if (re[0] == '$' && re[1] == '\0') return *text == '\0';
    if (re[1] == '*') {
        int c = re[0];
        return cs_rx_match_star(c, re+2, text);
    }
    if (re[0] == '\\') { /* escape next as literal if present */
        if (re[1] == '\0') return 0;
        if (*text && *text == re[1]) return cs_rx_match_here(re+2, text+1);
        return 0;
    }
    if (*text && (re[0] == '.' || re[0] == *text))
        return cs_rx_match_here(re+1, text+1);
    return 0;
}
static int cs_rx_match(const char* re, const char* text) {
    if (!re) return 0;
    if (re[0] == '^') return cs_rx_match_here(re+1, text);
    do {
        if (cs_rx_match_here(re, text)) return 1;
    } while (*text++ != '\0');
    return 0;
}

#endif /* CS_ANNOTATIONS_INCLUDED */
)";
            }

            // ------------ Internal parsing helpers (compiler-side) ------------
            struct KV { std::string key, val; };
            struct AnnRec { std::string kind, target; std::vector<KV> kvs; int line = 0; };

            static std::string trim(std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
                size_t b = s.find_last_not_of(" \t\r\n");  return s.substr(a, b - a + 1);
            }
            static void parse_kv_items(const std::string& rest, std::vector<KV>& out) {
                std::istringstream ss(rest);
                std::string tok;
                auto unq = [](std::string t)->std::string {
                    t = trim(t);
                    if (!t.empty() && (t.front() == '"' || t.front() == '\'')) {
                        char q = t.front(); if (t.size() >= 2 && t.back() == q) t = t.substr(1, t.size() - 2);
                    }
                    return t;
                    };
                while (ss >> tok) {
                    size_t eq = tok.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = trim(tok.substr(0, eq));
                    std::string v = unq(tok.substr(eq + 1));
                    if (!k.empty()) out.push_back({ k,v });
                }
            }

            struct RegexSpec { std::string name; std::string pattern; };
            static std::string unescape_quoted(const std::string& in) {
                std::string out; out.reserve(in.size());
                for (size_t i = 0; i < in.size(); ++i) {
                    char c = in[i];
                    if (c == '\\' && i + 1 < in.size()) {
                        char n = in[++i];
                        switch (n) {
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case '\\': out.push_back('\\'); break;
                        case '"': out.push_back('"'); break;
                        default: out.push_back(n); break;
                        }
                    }
                    else out.push_back(c);
                }
                return out;
            }
            static bool has_meta(const std::string& p) {
                for (char c : p) {
                    if (c == '.' || c == '*' || c == '^' || c == '$' || c == '\\') return true;
                }
                return false;
            }

            // ------------ Scan @annot and @regex directives from raw .csc ------------
            static void scan(const std::string& src,
                std::vector<AnnRec>& anns,
                std::vector<RegexSpec>& regs) {
                std::istringstream ss(src);
                std::string line;
                int lineno = 0;
                while (std::getline(ss, line)) {
                    ++lineno;
                    std::string t = trim(line);
                    if (t.empty() || t[0] != '@') continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    if (name == "annot") {
                        std::string kind, target;
                        ls >> kind; ls >> target;
                        std::string rest; std::getline(ls, rest);
                        if (!kind.empty()) {
                            AnnRec rec; rec.kind = kind; rec.target = target; rec.line = lineno;
                            parse_kv_items(rest, rec.kvs);
                            anns.push_back(std::move(rec));
                        }
                    }
                    else if (name == "regex") {
                        std::string rname; ls >> rname;
                        std::string pat;
                        if (ls.peek() == '"' || ls.peek() == '\'') ls >> std::quoted(pat);
                        if (!rname.empty()) {
                            RegexSpec rs; rs.name = rname; rs.pattern = unescape_quoted(pat);
                            regs.push_back(std::move(rs));
                        }
                    }
                }
            }

            // ------------ Emit C registry and regex wrappers ------------
            static std::string emit_from_source(const std::string& srcAll, bool echo = false) {
                std::vector<AnnRec> anns;
                std::vector<RegexSpec> regs;
                scan(srcAll, anns, regs);

                std::ostringstream o;
                o << prelude_annotations_addendum();

                // Flatten annotations into triplets
                o << "static const CS_Annotation cs_annotations[] = {\n";
                size_t triplets = 0;
                for (const auto& a : anns) {
                    for (const auto& kv : a.kvs) {
                        o << "  { \"" << a.kind << "\", \"" << a.target << "\", \"" << kv.key << "\", \"" << kv.val << "\", " << a.line << " },\n";
                        triplets++;
                    }
                }
                o << "};\n";
                o << "static const unsigned cs_annotations_count = (unsigned)(sizeof(cs_annotations)/sizeof(cs_annotations[0]));\n";

                // Regex wrappers: choose fast path if no meta, or anchored ^...$
                for (const auto& r : regs) {
                    const std::string& nm = r.name;
                    const std::string& pat = r.pattern;
                    bool meta = has_meta(pat);
                    bool anchored = (!pat.empty() && pat.front() == '^' && pat.back() == '$' && pat.size() >= 2);
                    if (!meta) {
                        // Fast substring search
                        o << "static int cs_rx_" << nm << "(const char* s){ return (s && strstr(s, \"" << pat << "\")!=NULL); }\n";
                    }
                    else if (anchored) {
                        std::string mid = pat.substr(1, pat.size() - 2);
                        if (!has_meta(mid)) {
                            o << "static int cs_rx_" << nm << "(const char* s){ return (s && strcmp(s, \"" << mid << "\")==0); }\n";
                        }
                        else {
                            o << "static int cs_rx_" << nm << "(const char* s){ return cs_rx_match(\"" << pat << "\", s); }\n";
                        }
                    }
                    else {
                        o << "static int cs_rx_" << nm << "(const char* s){ return cs_rx_match(\"" << pat << "\", s); }\n";
                    }
                }

                if (echo) {
                    std::cerr << "[annotations] triplets=" << triplets << " regex=" << regs.size() << "\n";
                }

                return o.str();
            }

            // ------------ Optional DSL lowerings ------------
            static std::string lower_regex_calls(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // regexis!(Name, expr) -> (cs_rx_Name(expr)!=0)
                std::regex re(R"(regexis!\s*\(\s*([A-Za-z_]\w*)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "(cs_rx_" + m[1].str() + "(" + m[2].str() + ")!=0)";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }
            static std::string lower_annotation_queries(const std::string& src) {
                using namespace cs_regex_wrap;
                std::string s = src, out; out.reserve(s.size());
                // annhas!("kind","target","key")
                std::regex reHas(R"(annhas!\s*\(\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*\))", std::regex::ECMAScript);
                // annget!("kind","target","key")
                std::regex reGet(R"(annget!\s*\(\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*\))", std::regex::ECMAScript);
                cs_regex_wrap::cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, reHas)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_anno_has(\"" + m[1].str() + "\",\"" + m[2].str() + "\",\"" + m[3].str() + "\")";
                }
                out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size()); pos = 0;
                while (search_from(s, pos, m, reGet)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += "cs_anno_get(\"" + m[1].str() + "\",\"" + m[2].str() + "\",\"" + m[3].str() + "\")";
                }
                out.append(s, pos, std::string::npos);
                return out;
            }
            static std::string apply_lowerings(const std::string& src) {
                std::string t = lower_regex_calls(src);
                t = lower_annotation_queries(t);
                return t;
            }

        } // namespace cs_annotations

		// ---------- Public API ----------
        // Apply all DSL lowerings and emit prelude addenda
        std::string optimax_and_annotations_process(const std::string& src, bool echo = false) {
            std::string t = cs_optimax::apply_lowerings(src);
            t = cs_annotations::apply_lowerings(t);
            std::string pre1 = cs_optimax::prelude_optimax_addendum();
            std::string pre2 = cs_annotations::prelude_annotations_addendum();
            std::string pre = pre1 + "\n" + pre2 + "\n";
            if (echo) {
                std::cerr << "[optimax] prelude lines=" << std::count(pre.begin(), pre.end(), '\n')
                    << " optimax changes=" << (t != src ? 1 : 0) << "\n";
            }
            return pre + t;
        }
        // Emit C code for annotations and regexes from raw .csc source
        std::string annotations_emit_from_source(const std::string& srcAll, bool echo = false) {
            return cs_annotations::emit_from_source(srcAll, echo);
        }
        // Build command helper for max-speed builds
        std::string build_cmd_optimax_speed(const Config& cfg, const std::string& cc, const std::string& cpath, const std::string& out,
            bool defineProfile, const std::string& c_translation_unit_for_scan) {
            return cs_optimax::build_cmd_speed(cfg, cc, cpath, out, defineProfile, c_translation_unit_for_scan);
		}
		} // namespace csc
		} // namespace cs

		// end of Source2.cpp

// ============================ Validational‑Reinforced Adaptive Learning Pack (append-only) ============================
        namespace cs_vral {

#include <random>
#include <mutex>

            struct LearnStat {
                uint64_t n = 0;           // trials
                double   sum = 0.0;       // total reward
                double   ema = 0.0;       // exponential moving average
                double   last = 0.0;      // last reward
            };
            static std::map<std::string, LearnStat> g_db;
            static uint64_t g_total = 0;
            static bool g_loaded = false;
            static std::mutex g_mu;

            // Simple, robust DB path in temp dir
            static std::string db_path() {
#if defined(_WIN32)
                char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
                return std::string(buf) + "cscript_learn_db.txt";
#else
                const char* t = getenv("TMPDIR");
                std::string dir = t ? t : "/tmp/";
                return dir + "cscript_learn_db.txt";
#endif
            }

            static void load_db_unlocked() {
                if (g_loaded) return;
                g_db.clear(); g_total = 0;
                std::ifstream f(db_path());
                if (f) {
                    std::string key; LearnStat s;
                    while (f >> key >> s.n >> s.sum >> s.ema >> s.last) {
                        g_db[key] = s; g_total += s.n;
                    }
                }
                g_loaded = true;
            }

            static void save_db_unlocked() {
                std::ofstream o(db_path(), std::ios::binary | std::ios::trunc);
                for (auto& kv : g_db) {
                    o << kv.first << " " << kv.second.n << " " << kv.second.sum << " " << kv.second.ema << " " << kv.second.last << "\n";
                }
            }

            static void learn_update(const std::string& key, double reward) {
                std::lock_guard<std::mutex> lk(g_mu);
                load_db_unlocked();
                LearnStat& s = g_db[key];
                s.n++;
                s.sum += reward;
                s.last = reward;
                // EMA with alpha=0.2
                s.ema = (s.n == 1 ? reward : 0.8 * s.ema + 0.2 * reward);
                g_total++;
                save_db_unlocked();
            }

            static LearnStat get_stat(const std::string& key) {
                std::lock_guard<std::mutex> lk(g_mu);
                load_db_unlocked();
                auto it = g_db.find(key);
                return it == g_db.end() ? LearnStat{} : it->second;
            }

            // Optional: scan directives to passively learn tendency (counts are small positive rewards)
            static void scan_and_record(const std::string& srcAll) {
                std::istringstream ss(srcAll);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = line;
                    // trim
                    size_t a = t.find_first_not_of(" \t\r\n"); if (a == std::string::npos) continue;
                    size_t b = t.find_last_not_of(" \t\r\n"); t = t.substr(a, b - a + 1);
                    if (t.rfind("@", 0) == 0) {
                        // Normalize: learn:directive:name=value
                        std::istringstream ls(t.substr(1));
                        std::string name; ls >> name;
                        if (!name.empty()) {
                            std::string rest; std::getline(ls, rest);
                            std::string key = "directive:" + name;
                            learn_update(key, +0.02); // small bias toward used directives
                        }
                    }
                }
            }

            // Public manual reward/penalty hooks
            static void reward(const std::string& name, double r = +1.0) { learn_update("manual:" + name, r); }
            static void penalize(const std::string& name, double r = -1.0) { learn_update("manual:" + name, r); }

            // Choice over build flag mixes
            struct Choice {
                std::string key;   // identity of the chosen arm
                std::string opt;   // "O2"/"O3"/"size"/"max"
                bool lto = true;
                bool fastmath = false; // best-effort toggle (/fp:fast or -ffast-math)
            };

            static std::string make_key(const Choice& c) {
                std::ostringstream o;
                o << "build:" << "opt=" << c.opt << ";lto=" << (c.lto ? 1 : 0) << ";fm=" << (c.fastmath ? 1 : 0);
                return o.str();
            }

            struct Arm { Choice c; double prior = 0.0; };

            // Score = UCB on EMA + small prior; epsilon-greedy exploration
            static double score_ucb(const std::string& key, double prior, uint64_t totalN, double c = 1.2) {
                auto s = get_stat(key);
                double avg = (s.n ? s.ema : 0.0);
                double bonus = (s.n ? c * std::sqrt(std::log(double(std::max<uint64_t>(1, totalN))) / double(s.n)) : 1.0);
                return prior + avg + bonus;
            }

            // Build command synthesis with adaptive selection
            static std::string build_cmd_from_choice(const Config& cfg_in,
                const std::string& cc,
                const std::string& cpath,
                const std::string& out,
                bool defineProfile,
                const Choice& choice) {
                Config cfg = cfg_in; // do not mutate caller
                cfg.opt = choice.opt;
                if (!choice.lto) cfg.lto = false;

                std::vector<std::string> cmd; cmd.push_back(cc);
                bool msvc = (cc == "cl" || cc == "clang-cl");

                auto add = [&](const std::string& s) { cmd.push_back(s); };

                if (msvc) {
                    add("/nologo");
                    if (cfg.opt == "O0") add("/Od");
                    else if (cfg.opt == "O1") add("/O1");
                    else add("/O2");
                    if (cfg.hardline || cfg.strict) { add("/Wall"); add("/WX"); }
                    if (cfg.lto) add("/GL");
                    if (cfg.hardline) add("/DCS_HARDLINE=1");
                    if (defineProfile) add("/DCS_PROFILE_BUILD=1");
                    for (auto& d : cfg.defines) add("/D" + d);
                    for (auto& p : cfg.incs)    add("/I" + p);
                    if (choice.fastmath) add("/fp:fast");
                    add(cpath);
                    add("/Fe:" + out);
                    for (auto& lp : cfg.libpaths) add("/link /LIBPATH:\"" + lp + "\"");
                    for (auto& l : cfg.links) {
                        std::string lib = l; if (lib.rfind(".lib") == std::string::npos) lib += ".lib";
                        add("/link " + lib);
                    }
                }
                else {
                    // std: keep caller default (c11 is fine)
                    add("-std=c11");
                    if (cfg.opt == "O0") add("-O0");
                    else if (cfg.opt == "O1") add("-O1");
                    else if (cfg.opt == "O2") add("-O2");
                    else if (cfg.opt == "size") add("-Os");
                    else add("-O3");
                    if (cfg.hardline) { add("-Wall"); add("-Wextra"); add("-Werror"); add("-Wconversion"); add("-Wsign-conversion"); }
                    if (cfg.lto) add("-flto");
                    if (cfg.hardline) add("-DCS_HARDLINE=1");
                    if (defineProfile) add("-DCS_PROFILE_BUILD=1");
                    if (choice.fastmath) { add("-ffast-math"); add("-fno-trapping-math"); }
                    for (auto& d : cfg.defines) add("-D" + d);
                    for (auto& p : cfg.incs)    add("-I" + p);
                    add(cpath);
                    add("-o"); add(out);
                    for (auto& lp : cfg.libpaths) add("-L" + lp);
                    for (auto& l : cfg.links)     add("-l" + l);
#if !defined(__APPLE__)
                    // help dead code elimination slightly (harmless)
                    add("-Wl,--gc-sections");
#endif
                }

                std::string full;
                for (size_t i = 0; i < cmd.size(); ++i) {
                    if (i) full.push_back(' ');
                    bool q = cmd[i].find(' ') != std::string::npos;
                    if (q) full.push_back('"'); full += cmd[i]; if (q) full.push_back('"');
                }
                return full;
            }

            // Public: pick a build command adaptively. Returns cmd and fills outChoice (key included).
            static std::string build_cmd_adaptive(const Config& cfg,
                const std::string& cc,
                const std::string& cpath,
                const std::string& out,
                bool defineProfile,
                const std::string& src_for_scan,
                Choice& outChoice) {
                (void)src_for_scan;
                std::lock_guard<std::mutex> lk(g_mu);
                load_db_unlocked();

                // Candidate arms (compact, safe space)
                std::vector<Arm> arms;
                for (std::string opt : {"O2", "O3"}) {
                    for (int lto : {0, 1}) {
                        for (int fm : {0, 1}) {
                            Choice c; c.opt = opt; c.lto = (lto != 0); c.fastmath = (fm != 0);
                            c.key = make_key(c);
                            double prior = (opt == "O3" ? 0.02 : 0.0) + (lto ? 0.01 : 0.0) + (fm ? 0.0 : 0.0); // gentle bias
                            arms.push_back({ c, prior });
                        }
                    }
                }

                // Epsilon-greedy with UCB tie-break
                std::mt19937_64 prng{ std::random_device{}() };
                std::uniform_real_distribution<double> uni(0.0, 1.0);
                double eps = 0.12; // explore 12%
                Arm chosen;
                if (uni(prng) < eps) {
                    chosen = arms[std::uniform_int_distribution<int>(0, (int)arms.size() - 1)(prng)];
                }
                else {
                    double best = -1e100; int bi = -1;
                    for (int i = 0; i < (int)arms.size(); ++i) {
                        double s = score_ucb(arms[i].c.key, arms[i].prior, g_total ? g_total : 1);
                        if (s > best) { best = s; bi = i; }
                    }
                    chosen = arms[bi >= 0 ? bi : 0];
                }

                outChoice = chosen.c;
                // Pre-emptively note a selection event (very small reward to count usage)
                g_db[outChoice.key]; // ensure presence
                save_db_unlocked();

                return build_cmd_from_choice(cfg, cc, cpath, out, defineProfile, outChoice);
            }

            // Commit result after running the build. Reward function mixes success and speed.
            static void learn_commit(const Choice& choice, int build_rc, long long compile_ms) {
                // Reward: +1 for success, -1 for fail; small speed component (faster is better)
                double r = (build_rc == 0 ? +1.0 : -1.0);
                if (build_rc == 0) {
                    // Normalize compile_ms to ~[-0.2..+0] using soft scale (favor faster)
                    double speedBonus = -std::tanh(std::max(0.0, (double)compile_ms) / 8000.0) * 0.2;
                    r += speedBonus;
                }
                learn_update(choice.key, r);
            }

            // Optional: parse @learn/@reward/@penalize in .csc
            static void parse_learn_directives(const std::string& srcAll) {
                std::istringstream ss(srcAll);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = line;
                    size_t a = t.find_first_not_of(" \t\r\n"); if (a == std::string::npos) continue;
                    size_t b = t.find_last_not_of(" \t\r\n"); t = t.substr(a, b - a + 1);
                    if (t.rfind("@", 0) != 0) continue;
                    std::istringstream ls(t.substr(1));
                    std::string name; ls >> name;
                    if (name == "reward") {
                        std::string id; double val = 1.0; ls >> id >> val; reward(id, val);
                    }
                    else if (name == "penalize") {
                        std::string id; double val = -1.0; ls >> id >> val; penalize(id, val);
                    }
                    else if (name == "learn") {
                        // currently informational; could toggle exploration if needed
                    }
                }
            }

            // Convenience: time a callable in ms
            template <class F>
            static long long time_ms(F&& f) {
                auto t0 = std::chrono::high_resolution_clock::now();
                f();
                auto t1 = std::chrono::high_resolution_clock::now();
                return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            }

        } // namespace cs_vral

		// end of Validational‑Reinforced Adaptive Learning Pack

		// ============================ End of Source2.cpp ============================

		// Example usage (uncomment main to enable standalone build of this file)
#if 0
        int main(int argc, char** argv) {
            using namespace cs::csc;
            using namespace cs::cs_vral;
            if (argc < 4) {
                std::cerr << "Usage: csc Source.csc Output.exe cc [flags...]\n";
                return 1;
            }
            std::string srcPath = argv[1];
            std::string outPath = argv[2];
            std::string cc = argv[3];
            Config cfg;
            for (int i = 4; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--hardline") cfg.hardline = true;
                else if (a == "--strict") cfg.strict = true;
                else if (a == "--lto") cfg.lto = true;
                else if (a.rfind("-D", 0) == 0) cfg.defines.push_back(a.substr(2));
                else if (a.rfind("-I", 0) == 0) cfg.incs.push_back(a.substr(2));
                else if (a.rfind("-L", 0) == 0) cfg.libpaths.push_back(a.substr(2));
                else if (a.rfind("-l", 0) == 0) cfg.links.push_back(a.substr(2));
                else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3" || a == "-size") cfg.opt = a.substr(1);
            }
            // Read source
            std::ifstream f(srcPath);
            if (!f) {
                std::cerr << "Error: cannot open source file: " << srcPath << "\n";
                return 1;
            }
            std::string srcAll((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // Optional: parse @learn directives to passively learn usage
            scan_and_record(srcAll);
            // Optional: parse @learn/@reward/@penalize directives to update DB
            parse_learn_directives(srcAll);
            // Process source: apply DSL lowerings and emit prelude addenda
            bool echo = true;
            std::string procSrc = optimax_and_annotations_process(srcAll, echo);
            // Emit processed source to temp file
			std::string tmp
                = db_path() + ".c";
            {
                std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
                o << procSrc;
            }
            // Build command (adaptive choice)
            Choice choice;
            std::string cmd = build_cmd_adaptive(cfg, cc, tmp, outPath, true, srcAll, choice);
            if (echo) {
                std::cerr << "[build] choice: opt=" << choice.opt << " lto=" << (choice.lto ? "on" : "off") << " fm=" << (choice.fastmath ? "on" : "off") << "\n";
                std::cerr << "[build] cmd: " << cmd << "\n";
            }
            // Run build and time it
            int rc = -1;
			long long ms = time_ms([&]() {
                rc = std::system(cmd.c_str());
                });
            if (echo) {
                std::cerr << "[build] rc=" << rc << " time=" << ms << "ms\n";
            }
            // Commit result to learning DB
            learn_commit(choice, rc, ms);
            // Done
            return rc;
		}
		}
#endif
		} // namespace cs
		// end of cscript.cpp
		// ============================ End of cscript.cpp ============================

// ============================ Reflection + Bounds + Runtime Core Pack (append-only) ============================
        namespace cs_runtime_core {

            // Portable runtime core (time/sleep/cpu count, file I/O, entropy, panics)
            static std::string prelude_runtime_addendum() {
                return R"(
/* --- Runtime Core Addendum --- */
#ifndef CS_RUNTIME_CORE_INCLUDED
#define CS_RUNTIME_CORE_INCLUDED 1
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #include <sysinfoapi.h>
  #include <bcrypt.h>
  #pragma comment(lib, "bcrypt.lib")
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <fcntl.h>
#endif

/* Time */
static uint64_t cs_rt_now_ns(void){
#if defined(_WIN32)
    LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)((1000000000.0 * (double)c.QuadPart) / (double)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}
static void cs_rt_sleep_ms(unsigned ms){
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L; nanosleep(&ts, NULL);
#endif
}
static int cs_rt_cpu_count(void){
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); return (int)(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN); return (int)(n>0?n:1);
#endif
}

/* File I/O (read/write whole file) */
static int cs_rt_read_file(const char* path, char** outData, size_t* outLen){
    *outData = NULL; if (outLen) *outLen = 0;
    FILE* f = fopen(path, "rb"); if(!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); if (sz < 0){ fclose(f); return 0; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz+1); if(!buf){ fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[n] = 0; *outData = buf; if (outLen) *outLen = n; return 1;
}
static int cs_rt_write_file(const char* path, const void* data, size_t len){
    FILE* f = fopen(path, "wb"); if(!f) return 0;
    size_t n = fwrite(data, 1, len, f); fclose(f); return n==len;
}

/* Entropy */
static int cs_rt_entropy(void* dst, size_t len){
#if defined(_WIN32)
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)dst, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st==0;
#else
    #if defined(__linux__)
      ssize_t r = getrandom(dst, len, 0); return r==(ssize_t)len;
    #else
      int fd = open("/dev/urandom", O_RDONLY); if (fd<0) return 0;
      size_t got = 0; while (got < len){ ssize_t n = read(fd, (char*)dst+got, len-got); if (n<=0){ close(fd); return 0; } got += (size_t)n; }
      close(fd); return 1;
    #endif
#endif
}

/* Panics */
static void cs_panic(const char* msg){
#if defined(CS_HARDLINE)
    fprintf(stderr, "[panic] %s\n", msg?msg:"(null)"); fflush(stderr); abort();
#else
fprintf(stderr, "[panic-soft] %s\n", msg ? msg : "(null)"); fflush(stderr);
#endif
            }
            static void cs_panicf(const char* fmt, ...) {
                va_list ap; va_start(ap, fmt); fprintf(stderr, "[panic] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
#if defined(CS_HARDLINE)
                abort();
#endif
            }

#endif /* CS_RUNTIME_CORE_INCLUDED */
                )";
        }

} // namespace cs_runtime_core

// ---------------------------- Hidden bounds + panics (with DSL lowerings) ----------------------------
namespace cs_bounds {

    static bool scan_bounds_directive(const std::string& src) {
        std::istringstream ss(src); std::string line;
        while (std::getline(ss, line)) {
            std::string t = line; size_t a = t.find_first_not_of(" \t\r\n"); if (a == std::string::npos) continue;
            size_t b = t.find_last_not_of(" \t\r\n"); t = t.substr(a, b - a + 1);
            if (t.rfind("@bounds", 0) == 0) { std::istringstream ls(t.substr(8)); std::string v; ls >> v; return v != "off"; }
        }
        return false;
    }

    static std::string prelude_bounds_addendum() {
        return R"(
/* --- Bounds & Panic Addendum --- */
#ifndef CS_BOUNDS_INCLUDED
#define CS_BOUNDS_INCLUDED 1
#ifndef CS_COUNT_OF
  #if defined(_MSC_VER)
    #define CS_COUNT_OF(a) _countof(a)
  #else
    #define CS_COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
  #endif
#endif

static void cs_bounds_panic(const char* arr, const char* idx, size_t limit){
    char msg[256];
    snprintf(msg, sizeof(msg), "index out of bounds: %s[%s] (limit=%zu)", arr ? arr : "?", idx ? idx : "?", (size_t)limit);
    cs_panic(msg);
    }

    /* GCC/Clang array detection; MSVC falls back to _countof for real arrays */
#if defined(__clang__) || defined(__GNUC__)
#define __CS_IS_ARRAY(a) (!__builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0])))
#else
#define __CS_IS_ARRAY(a) 1 /* best-effort */
#endif

#define CS_IDX_AUTO(a,i) \
    ( (( __CS_IS_ARRAY(a) && ((size_t)(i) >= CS_COUNT_OF(a)) ) ) ? \
        (cs_bounds_panic(#a,#i,(size_t)CS_COUNT_OF(a)), (a)[(i)]) : (a)[(i)] )

#define CS_CHECK(cond) do{ if(!(cond)){ cs_panicf("check failed: %s at %s:%d", #cond, __FILE__, __LINE__); } }while(0)
#define CS_PANIC(msg)  do{ cs_panic(msg); }while(0)

#endif /* CS_BOUNDS_INCLUDED */
        )";
}

// DSL lowerings (idx!, panic!, check!, and optional hidden [ ] wrapping)
static std::string apply_lowerings(const std::string& src, bool enableHidden) {
    using namespace cs_regex_wrap;
    std::string s = src, out; out.reserve(s.size());

    // idx!(a, i) -> CS_IDX_AUTO(a,i)
    {
        std::regex re(R"(idx!\s*\(\s*([\s\S]*?)\s*,\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
        cmatch m; size_t pos = 0;
        while (search_from(s, pos, m, re)) {
            append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
            out += "CS_IDX_AUTO((" + m[1].str() + "),(" + m[2].str() + "))";
        }
        out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size());
    }
    // panic!("msg") -> CS_PANIC("msg")
    {
        std::regex re(R"(panic!\s*\(\s*\"([\s\S]*?)\"\s*\))", std::regex::ECMAScript);
        cmatch m; size_t pos = 0;
        while (search_from(s, pos, m, re)) {
            append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
            out += "CS_PANIC(\"" + m[1].str() + "\")";
        }
        out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size());
    }
    // check!(cond) -> CS_CHECK(cond)
    {
        std::regex re(R"(check!\s*\(\s*([\s\S]*?)\s*\))", std::regex::ECMAScript);
        cmatch m; size_t pos = 0;
        while (search_from(s, pos, m, re)) {
            append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
            out += "CS_CHECK(" + m[1].str() + ")";
        }
        out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size());
    }
    // Optional hidden rewrite: ident[expr] -> CS_IDX_AUTO(ident, expr)
    if (enableHidden) {
        // Heuristic: identifier '[' ... ']' not immediately preceded by 'sizeof' or '#' or in a declaration.
        // Conservatively avoid matches with 'struct', 'typedef', 'enum', 'return'.
        std::regex re(R"((?<!sizeof\s|\#|\bstruct\b|\btypedef\b|\benum\b|\breturn\b)\b([A-Za-z_]\w*)\s*\[\s*([\s\S]*?)\s*\])", std::regex::ECMAScript);
        s = std::regex_replace(s, re, "CS_IDX_AUTO($1, $2)");
    }
    return s;
}

} // namespace cs_bounds

// ---------------------------- Complete runtime reflection (types, fields, functions) ----------------------------
namespace cs_reflect {

    struct Field { std::string type, name; };
    struct Struct { std::string name; std::vector<Field> fields; };
    struct EnumRec { std::string name; std::vector<std::string> members; };
    struct FuncRec { std::string name, args, ret; };

    static std::string trim(std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
    }

    static void scan(const std::string& src, std::vector<Struct>& structs, std::vector<EnumRec>& enums, std::vector<FuncRec>& funcs) {
        using namespace cs_regex_wrap;
        // struct! Name { fields; }
        {
            std::regex re(R"(struct!\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
            cmatch m; size_t pos = 0;
            while (search_from(src, pos, m, re)) {
                Struct S; S.name = trim(m[1].str());
                std::istringstream ss(m[2].str()); std::string line;
                while (std::getline(ss, line, ';')) {
                    line = trim(line); if (line.empty()) continue;
                    // split "Type name" (best-effort, supports '*', '[]')
                    size_t sp = line.find_last_of(" \t");
                    if (sp == std::string::npos) continue;
                    Field f; f.type = trim(line.substr(0, sp)); f.name = trim(line.substr(sp + 1));
                    if (!f.name.empty() && !f.type.empty()) S.fields.push_back(std::move(f));
                }
                structs.push_back(std::move(S));
            }
        }
        // enum! Name { A=..., B, C, ... }
        {
            std::regex re(R"(enum!\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
            cmatch m; size_t pos = 0;
            while (search_from(src, pos, m, re)) {
                EnumRec E; E.name = trim(m[1].str());
                std::string body = m[2].str(); std::string tok; std::istringstream ss(body);
                while (std::getline(ss, tok, ',')) {
                    std::string t = trim(tok); if (t.empty()) continue;
                    size_t eq = t.find('='); if (eq != std::string::npos) t = trim(t.substr(0, eq));
                    E.members.push_back(t);
                }
                enums.push_back(std::move(E));
            }
        }
        // fn name(args) -> ret
        {
            std::regex r1(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^\{\n;]+)\s*\{)", std::regex::ECMAScript);
            std::regex r2(R"(\bfn\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*->\s*([^\n;]+)\s*=>)", std::regex::ECMAScript);
            for (auto& r : { r1, r2 }) {
                cmatch m; size_t pos = 0;
                while (cs_regex_wrap::search_from(src, pos, m, r)) {
                    FuncRec F; F.name = trim(m[1].str()); F.args = trim(m[2].str()); F.ret = trim(m[3].str());
                    funcs.push_back(std::move(F));
                }
            }
        }
    }

    // C prelude for reflection types and API
    static std::string prelude_reflect_addendum() {
        return R"(
/* --- Reflection Addendum --- */
#ifndef CS_REFLECT_INCLUDED
#define CS_REFLECT_INCLUDED 1
#include <stddef.h>
typedef struct { const char* name; const char* type; size_t offset; } CS_FieldInfo;
typedef struct { const char* name; const CS_FieldInfo* fields; unsigned field_count; size_t size; size_t align; } CS_TypeInfo;
typedef struct { const char* name; const char* ret; const char* args; } CS_FuncInfo;
typedef struct { const char* name; const char* const* members; unsigned count; } CS_EnumInfo;

/* Extern registries emitted below */
extern const CS_TypeInfo cs_types[];
extern const unsigned    cs_types_count;
extern const CS_FuncInfo cs_funcs[];
extern const unsigned    cs_funcs_count;
extern const CS_EnumInfo cs_enums[];
extern const unsigned    cs_enums_count;

/* Lookup helpers */
static const CS_TypeInfo* cs_type_find(const char* name){
    for (unsigned i=0;i<cs_types_count;i++){ if (strcmp(cs_types[i].name,name)==0) return &cs_types[i]; }
    return NULL;
}
static const CS_FuncInfo* cs_func_find(const char* name){
    for (unsigned i=0;i<cs_funcs_count;i++){ if (strcmp(cs_funcs[i].name,name)==0) return &cs_funcs[i]; }
    return NULL;
}
static const CS_EnumInfo* cs_enum_find(const char* name){
    for (unsigned i=0;i<cs_enums_count;i++){ if (strcmp(cs_enums[i].name,name)==0) return &cs_enums[i]; }
    return NULL;
}
#endif /* CS_REFLECT_INCLUDED */
)";
    }

    // Emit reflection registries. Must be appended AFTER types/functions are lowered into C.
    static std::string emit_from_source(const std::string& srcAll, bool echo = false) {
        std::vector<Struct> structs; std::vector<EnumRec> enums; std::vector<FuncRec> funcs;
        scan(srcAll, structs, enums, funcs);

        std::ostringstream o;
        o << prelude_reflect_addendum();

        // Emit enum member static arrays
        for (auto& e : enums) {
            o << "static const char* const cs_enum_" << e.name << "_members[] = {";
            for (size_t i = 0; i < e.members.size(); ++i) { o << "\"" << e.members[i] << "\""; if (i + 1 < e.members.size()) o << ","; }
            o << "};\n";
        }

        // Emit fields arrays with offsetof(Type, field)
        for (auto& s : structs) {
            o << "static const CS_FieldInfo cs_fields_" << s.name << "[] = {";
            if (!s.fields.empty()) o << "\n";
            for (size_t i = 0; i < s.fields.size(); ++i) {
                const Field& f = s.fields[i];
                o << "  { \"" << f.name << "\", \"" << f.type << "\", offsetof(" << s.name << ", " << f.name << ") }";
                if (i + 1 < s.fields.size()) o << ",";
                o << "\n";
            }
            o << "};\n";
        }

        // Emit type table with sizeof/alignof
        o << "static const CS_TypeInfo cs_types[] = {\n";
        for (auto& s : structs) {
            o << "  { \"" << s.name << "\", cs_fields_" << s.name << ", (unsigned)(sizeof(cs_fields_" << s.name << ")/sizeof(cs_fields_" << s.name << "[0])), sizeof(" << s.name << "), ";
#if defined(_MSC_VER)
            o << "__alignof(" << s.name << ")";
#else
            o << "_Alignof(" << s.name << ")";
#endif
            o << " },\n";
        }
        o << "};\n";
        o << "static const unsigned cs_types_count = (unsigned)(sizeof(cs_types)/sizeof(cs_types[0]));\n";

        // Emit funcs
        o << "static const CS_FuncInfo cs_funcs[] = {\n";
        for (auto& f : funcs) {
            // signature strings kept as-is (portable)
            o << "  { \"" << f.name << "\", \"" << f.ret << "\", \"" << f.args << "\" },\n";
        }
        o << "};\n";
        o << "static const unsigned cs_funcs_count = (unsigned)(sizeof(cs_funcs)/sizeof(cs_funcs[0]));\n";

        // Emit enums table
        o << "static const CS_EnumInfo cs_enums[] = {\n";
        for (auto& e : enums) {
            o << "  { \"" << e.name << "\", cs_enum_" << e.name << "_members, (unsigned)(sizeof(cs_enum_" << e.name << "_members)/sizeof(cs_enum_" << e.name << "_members[0])) },\n";
        }
        o << "};\n";
        o << "static const unsigned cs_enums_count = (unsigned)(sizeof(cs_enums)/sizeof(cs_enums[0]));\n";

        if (echo) {
            std::cerr << "[reflect] structs=" << structs.size() << " enums=" << enums.size() << " funcs=" << funcs.size() << "\n";
        }
        return o.str();
    }

} // namespace cs_reflect

// ============================ Serialization + C23 + DB + Command Prompt Pack (append-only) ============================
namespace cs_serdb {

    // ------------------------------ C prelude addendum (JSON, Reflection-based serialization, DB, Command) ------------------------------
    static std::string prelude_serdb_addendum() {
        return R"(
/* --- Serialization + DB + Command Addendum --- */
#ifndef CS_SERDB_INCLUDED
#define CS_SERDB_INCLUDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===== JSON buffer and escaping ===== */
typedef struct { char* p; size_t len, cap; } CS_JBuf;
static void cs_jb_init(CS_JBuf* b, size_t cap){ b->p=(char*)malloc(cap?cap:256); b->len=0; b->cap=cap?cap:256; if(b->p) b->p[0]=0; }
static void cs_jb_free(CS_JBuf* b){ if(b->p) free(b->p); b->p=NULL; b->len=b->cap=0; }
static void cs_jb_putn(CS_JBuf* b, const char* s, size_t n){
    if(!b->p) return; if (b->len+n+1>b->cap){ size_t nc=b->cap*2; if(!nc) nc=256; while(nc<b->len+n+1) nc*=2; char* q=(char*)realloc(b->p,nc); if(!q) return; b->p=q; b->cap=nc; }
    memcpy(b->p+b->len, s, n); b->len+=n; b->p[b->len]=0;
}
static void cs_jb_puts(CS_JBuf* b, const char* s){ cs_jb_putn(b,s,strlen(s)); }
static void cs_jb_putc(CS_JBuf* b, char c){ cs_jb_putn(b,&c,1); }
static void cs_json_str(CS_JBuf* b, const char* s){
    cs_jb_putc(b,'"'); for(;*s;++s){ unsigned char c=(unsigned char)*s;
      if(c=='"'||c=='\\'){ cs_jb_putc(b,'\\'); cs_jb_putc(b,(char)c); }
      else if(c=='\b'){ cs_jb_puts(b,"\\b"); } else if(c=='\f'){ cs_jb_puts(b,"\\f"); }
      else if(c=='\n'){ cs_jb_puts(b,"\\n"); } else if(c=='\r'){ cs_jb_puts(b,"\\r"); }
      else if(c=='\t'){ cs_jb_puts(b,"\\t"); } else if(c<0x20){ char tmp[7]; snprintf(tmp,sizeof(tmp),"\\u%04x",c); cs_jb_puts(b,tmp); }
      else cs_jb_putc(b,(char)c);
    } cs_jb_putc(b,'"');
}
static void cs_json_key(CS_JBuf* b, const char* k){ cs_json_str(b,k); cs_jb_putc(b,':'); }

/* ===== Reflection-aware serialization (best-effort) ===== */
#if defined(CS_REFLECT_INCLUDED)
extern const CS_TypeInfo cs_types[]; extern const unsigned cs_types_count;
static const CS_TypeInfo* cs__type_find_local(const char* name){
    for(unsigned i=0;i<cs_types_count;i++){ if(strcmp(cs_types[i].name,name)==0) return &cs_types[i]; }
    return NULL;
}
static void cs_json_any(CS_JBuf* b, const char* typeName, const void* obj){
    const CS_TypeInfo* T = cs__type_find_local(typeName);
    if(!T){ cs_jb_puts(b,"null"); return; }
    cs_jb_putc(b,'{');
    for(unsigned i=0;i<T->field_count;i++){
        const CS_FieldInfo* f = &T->fields[i];
        if(i) cs_jb_putc(b,',');
        cs_json_key(b, f->name);
        /* Heuristic basic types by field->type string */
        const char* t = f->type;
        const unsigned char* base = (const unsigned char*)obj;
        const void* ptr = base + f->offset;
        if (strstr(t,"char*")) { const char* s = *(const char* const*)ptr; if(s) cs_json_str(b,s); else cs_jb_puts(b,"null"); }
        else if (strstr(t,"float")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%g", *(const float*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"double")){ char tmp[64]; snprintf(tmp,sizeof(tmp),"%g", *(const double*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"bool"))  { cs_jb_puts(b, (*(const int*)ptr) ? "true":"false"); }
        else if (strstr(t,"int8")||strstr(t,"char"))  { char tmp[64]; snprintf(tmp,sizeof(tmp),"%d",(int)*(const signed char*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"uint8")||strstr(t,"unsigned char")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%u",(unsigned)*(const unsigned char*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"int16")||strstr(t,"short")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%d",(int)*(const short*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"uint16")||strstr(t,"unsigned short")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%u",(unsigned)*(const unsigned short*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"int64")||strstr(t,"long long")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%lld",(long long)*(const long long*)ptr); cs_jb_puts(b,tmp); }
        else if (strstr(t,"uint64")||strstr(t,"unsigned long long")) { char tmp[64]; snprintf(tmp,sizeof(tmp),"%llu",(unsigned long long)*(const unsigned long long*)ptr); cs_jb_puts(b,tmp); }
        else { /* fallback: 32-bit signed */ char tmp[64]; snprintf(tmp,sizeof(tmp),"%d", *(const int*)ptr); cs_jb_puts(b,tmp); }
    }
    cs_jb_putc(b,'}');
}
#else
static void cs_json_any(CS_JBuf* b, const char* typeName, const void* obj){
    (void)typeName;(void)obj; cs_jb_puts(b,"null"); /* reflection not available */
}
#endif

/* Pretty printer for convenience */
static void cs_json_print(const char* typeName, const void* obj){
    CS_JBuf b; cs_jb_init(&b, 0); cs_json_any(&b, typeName, obj); fwrite(b.p,1,b.len,stdout); fputc('\n',stdout); cs_jb_free(&b);
}

/* ===== DSL helpers (optional): json!(ptr, TypeName) and cmd!("line") ===== */
#define CS_JSON_OF(ptr,TypeName) do{ cs_json_print(#TypeName,(const void*)(ptr)); }while(0)

/* ===== Command runner and prompt ===== */
static int cs_cmd_run(const char* line){
#if defined(_WIN32)
    (void)line; /* Use system directly; cmd.exe is the default shell for system() */
    return system(line);
#else
    return system(line);
#endif
}
static void cs_cmd_prompt(const char* banner){
    char buf[1024]; if(banner) fprintf(stderr,"%s\n", banner);
    fprintf(stderr,"> "); fflush(stderr);
    while (fgets(buf, sizeof(buf), stdin)){
        size_t n=strlen(buf); if(n && (buf[n-1]=='\n'||buf[n-1]=='\r')) buf[n-1]=0;
        if(!strcmp(buf,"exit")||!strcmp(buf,"quit")) break;
        (void)cs_cmd_run(buf);
        fprintf(stderr,"> "); fflush(stderr);
    }
}

/* ===== SQLite dynamic loader (best-effort) + minimal API ===== */
typedef struct {
    void* h;
    int (*sqlite3_open)(const char*, void**);
    int (*sqlite3_close)(void*);
    int (*sqlite3_exec)(void*, const char*, int (*)(void*,int,char**,char**), void*, char**);
} CS_Sqlite;
static CS_Sqlite cs_sqlite_load(void){
    CS_Sqlite S; memset(&S,0,sizeof(S));
#if defined(_WIN32)
    HMODULE h = LoadLibraryA("sqlite3.dll");
    if(!h) return S; S.h=(void*)h;
    S.sqlite3_open  = (int(*)(const char*,void**))GetProcAddress(h,"sqlite3_open");
    S.sqlite3_close = (int(*)(void*))GetProcAddress(h,"sqlite3_close");
    S.sqlite3_exec  = (int(*)(void*,const char*,int(*)(void*,int,char**,char**),void*,char**))GetProcAddress(h,"sqlite3_exec");
#else
    void* h = dlopen("libsqlite3.so", RTLD_LAZY);
    if(!h) h = dlopen("libsqlite3.dylib", RTLD_LAZY);
    if(!h) return S; S.h=h;
    S.sqlite3_open  = (int(*)(const char*,void**))dlsym(h,"sqlite3_open");
    S.sqlite3_close = (int(*)(void*))dlsym(h,"sqlite3_close");
    S.sqlite3_exec  = (int(*)(void*,const char*,int(*)(void*,int,char**,char**),void*,char**))dlsym(h,"sqlite3_exec");
#endif
    return S;
}
typedef struct { CS_Sqlite api; void* db; } CS_DB;
static CS_DB cs_db_open(const char* path){
    CS_DB D; memset(&D,0,sizeof(D)); D.api = cs_sqlite_load();
    if (!D.api.sqlite3_open) { fprintf(stderr,"[db] sqlite3 not found; open failed\n"); return D; }
    if (D.api.sqlite3_open(path?path:":memory:", &D.db)!=0) { D.db=NULL; fprintf(stderr,"[db] open failed\n"); }
    return D;
}
static void cs_db_close(CS_DB* D){ if(D && D->db && D->api.sqlite3_close){ D->api.sqlite3_close(D->db); D->db=NULL; } }
static int cs_db_exec(CS_DB* D, const char* sql){
    if(!D||!D->db||!D->api.sqlite3_exec) return -1;
    char* err=0; int rc = D->api.sqlite3_exec(D->db, sql, NULL, NULL, &err);
    if (rc!=0 && err){ fprintf(stderr,"[db] %s\n", err); /* sqlite3_free if present – omitted */ }
    return rc;
}

/* ===== Lightweight macros for DSL lowerings (optional) ===== */
#define CS_CMD(line) cs_cmd_run((line))
/* json! lowering uses CS_JSON_OF via compiler front-end */

#endif /* CS_SERDB_INCLUDED */
)";
    }

    // ------------------------------ Optional build helper: C23 glue ------------------------------
    static std::string build_cmd_c23_glue(const Config& cfg, const std::string& cc, const std::string& cpath, const std::string& out,
        bool defineProfile = false, const std::string& src_for_scan = std::string()) {
        // If your earlier C23 pack exists (cs_c23_support::build_cmd_c23), prefer it; otherwise fall back to legacy build_cmd.
        // We call the existing build_cmd in this translation unit.
        (void)src_for_scan;
        return build_cmd(cfg, cc, cpath, out, defineProfile);
    }

    // ------------------------------ Optional DSL lowerings (json!, cmd!) ------------------------------
    static std::string apply_lowerings(const std::string& src) {
        using namespace cs_regex_wrap;
        std::string s = src, out; out.reserve(s.size());
        // json!(ptr, TypeName) -> CS_JSON_OF(ptr, TypeName)
        {
            std::regex re(R"(json!\s*\(\s*([\s\S]*?)\s*,\s*([A-Za-z_]\w*)\s*\))", std::regex::ECMAScript);
            cmatch m; size_t pos = 0;
            while (search_from(s, pos, m, re)) {
                append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                out += "CS_JSON_OF((" + m[1].str() + ")," + m[2].str() + ")";
            }
            out.append(s, pos, std::string::npos); s.swap(out); out.clear(); out.reserve(s.size());
        }
        // cmd!("line") -> CS_CMD("line")
        {
            std::regex re(R"(cmd!\s*\(\s*\"([\s\S]*?)\"\s*\))", std::regex::ECMAScript);
            cmatch m; size_t pos = 0;
            while (search_from(s, pos, m, re)) {
                append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                out += "CS_CMD(\"" + m[1].str() + "\")";
            }
            out.append(s, pos, std::string::npos);
        }
        return s;
    }

} // namespace cs_serdb
// ============================ End of cscript.cpp ============================
// ============================ Complete CScript Pack (append-only) ============================

namespace cs {
    // Build C source code with optional DSL lowerings and addenda
    int build_cscript(const Config& cfg, const std::string& cc, const std::string& procSrc, const std::string& outPath,
        bool echo = false, bool srcAll = false) {
        using namespace cs_regex_wrap;
        // Scan for directives
        bool enableBounds = cs_bounds::scan_bounds_directive(procSrc);
        if (echo) {
            std::cerr << "[build] bounds: " << (enableBounds ? "on" : "off") << "\n";
        }
        // Apply DSL lowerings
        std::string src = procSrc;
        src = cs_bounds::apply_lowerings(src, enableBounds);
        src = cs_serdb::apply_lowerings(src);
        // Compose final source with addenda
        std::string finalSrc;
        finalSrc += "#include <stddef.h>\n"; // for offsetof
        finalSrc += cs_runtime_core::prelude_runtime_addendum();
        if (enableBounds) {
            finalSrc += cs_bounds::prelude_bounds_addendum();
        }
        finalSrc += src;
        finalSrc += cs_reflect::emit_from_source(finalSrc, echo);
        finalSrc += cs_serdb::prelude_serdb_addendum();
		// Write to temporary .c file and build
        std::string tmpCPath = outPath + ".tmp.c";
        if (!cs_write_file(tmpCPath, finalSrc.data(), finalSrc.size())) {
            std::cerr << "[error] failed to write temporary C file: " << tmpCPath << "\n";
            return 1;
        }
        if (echo) {
            std::cerr << "[build] written temporary C file: " << tmpCPath << " (" << finalSrc.size() << " bytes)\n";
            if (srcAll) {
                std::cerr << "----- begin full source -----\n";
                std::cerr << finalSrc;
                std::cerr << "----- end full source -----\n";
            }
        }
        // Build command
        std::string buildCmd = cs_serdb::build_cmd_c23_glue(cfg, cc, tmpCPath, outPath, true, finalSrc);
        if (echo) {
            std::cerr << "[build] running: " << buildCmd << "\n";
        }
        int rc = system(buildCmd.c_str());
        if (rc != 0) {
            std::cerr << "[error] build failed with code: " << rc << "\n";
            return rc;
        }
        // Success
        if (echo) {
            std::cerr << "[build] success, output: " << outPath << "\n";
        }
        // Cleanup temporary C file
        cs_remove_file(tmpCPath);
        return 0;
	}
} // namespace cs
// ============================ End of cscript.cpp ============================
// ============================ End of CScript Pack ============================
/* --- Runtime Core Addendum --- */
#ifndef CS_RUNTIME_CORE_INCLUDED
#define CS_RUNTIME_CORE_INCLUDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#if defined(_WIN32)
  #include <windows.h>
#include <bcrypt.h> /* link with -lbcrypt */
  #include <io.h>
#include <process.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #if defined(__linux__)
    #include <sys/sysinfo.h>
    #include <sys/random.h>
  #else
    #include <sys/param.h>
    #include <sys/sysctl.h>
    #include <dlfcn.h>
#endif
#endif
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <float.h>

/* ===== File I/O ===== */
static int cs_rt_read_file(const char* path, void** outData, size_t* outLen){
    FILE* f = fopen(path, "rb"); if(!f) return 0;
    if (fseek(f, 0, SEEK_END)!=0){ fclose(f); return 0; }
    long sz = ftell(f); if(sz<0){ fclose(f); return 0; }
	if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    void* data = malloc((size_t)sz + 1); if(!data){ fclose(f); return 0; }
    size_t n = fread(data, 1, (size_t)sz, f); fclose(f);
    if(n!=(size_t)sz){ free(data); return 0; }
    ((char*)data)[n]=0; if(outData) *outData=data; else free(data);
	if (outLen) *outLen = n; return 1;
}
static int cs_rt_write_file(const char* path, const void* data, size_t len){
	FILE* f = fopen(path, "wb
		"); if (!f) return 0;
		size_t n = fwrite(data, 1, len, f); fclose(f);
	if (n != len) return 0; return 1;
}
/* ===== Time ===== */
static double cs_rt_time_now() {
    #if defined(_WIN32)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (((uint64_t)ft.dwHighDateTime)<<32) | ((uint64_t)ft.dwLowDateTime);
	return (double)(t / 10000000.0 - 11644473600.0); /* seconds since Unix epoch */
    #else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}
/* ===== Randomness ===== */
static int cs_rt_random_bytes(void* buf, size_t len) {
	if (!buf || len == 0) return 0;
    #if defined(_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return 1;
	return 0;
    #elif defined(__linux__)
    if (getrandom(buf, len, 0) == (ssize_t)len) return 1;
    // fallback to /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, len); close(fd);
    return n == (ssize_t)len ? 1 : 0;
    #else
    // BSD/macOS: use sysctl
    size_t req = len;
    if (sysctlbyname("kern.random", buf, &req, NULL, 0) == 0 && req == len) return 1;
    // fallback to /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, len); close(fd);
	return n == (ssize_t)len ? 1 : 0;
#endif

    // ============================ Inline Hex + Assembly Pack (append-only) ============================
    namespace cs_asmpack {

        // ---------- Prelude for generated C (inline asm + helpers) ----------
        static std::string prelude_asm_addendum() {
            return R"(
/* --- Inline Assembly + Hex Prelude Addendum --- */
#ifndef CS_ASM_INCLUDED
#define CS_ASM_INCLUDED 1
#include <stdint.h>
#if defined(_MSC_VER)
  #include <intrin.h>
  #ifndef __has_builtin
    #define __has_builtin(x) 0
  #endif
#endif

/* Portable inline asm wrappers:
   - CS_ASM("...") inserts best-effort inline assembly.
   - On MSVC x64, inline asm is not supported: empty bodies become __debugbreak();
*/
#if defined(_MSC_VER)
  #if defined(_M_X64)
    #define CS_ASM(x) do{ (void)(x); __debugbreak(); }while(0) /* no inline asm on MSVC x64 */
  #else
    #define CS_ASM(x) __asm { x }
  #endif
#else
  #define CS_ASM(x) __asm__ __volatile__(x : : : "memory")
#endif

/* Small helpers for byte arrays composed at translation time (from front-end lowerings) */
#define CS_HEX_BYTES_LIT(...) ((const unsigned char[]){ __VA_ARGS__ })

#endif /* CS_ASM_INCLUDED */
)";
        }

        // ---------- DSL lowerings: hex, inline asm (regex-based) ----------
        using namespace cs_regex_wrap;

        // Remove underscores in hex numerics: 0xDE_AD_BE_EF -> 0xDEADBEEF
        static std::string lower_hex_numeric_underscores(const std::string& src) {
            std::string s = src, out; out.reserve(s.size());
            std::regex re(R"(0x[0-9A-Fa-f_]+)");
            cmatch m; size_t pos = 0;
            while (search_from(s, pos, m, re)) {
                append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                std::string t = m[0].str();
                t.erase(std::remove(t.begin(), t.end(), '_'), t.end());
                out += t;
            }
            out.append(s, pos, std::string::npos);
            return out;
        }

        // hexu{8,16,32,64}!("DE AD ...") -> 0xDEAD...ULL etc.
        static std::string lower_hex_uints(const std::string& src) {
            auto fold = [](const std::string& hex, unsigned bits)->std::string {
                std::string h; h.reserve(hex.size());
                for (char c : hex) { if (isxdigit((unsigned char)c)) h.push_back((char)toupper((unsigned char)c)); }
                if (h.empty()) h = "0";
                std::string suf = (bits == 64 ? "ULL" : (bits == 32 ? "U" : "U"));
                return "0x" + h + suf;
                };
            std::string s = src, out; out.reserve(s.size());
            struct Rule { std::regex re; unsigned bits; };
            std::vector<Rule> rules = {
                { std::regex(R"(hexu8!\s*\(\s*\"([\s\S]*?)\"\s*\))"), 8u },
                { std::regex(R"(hexu16!\s*\(\s*\"([\s\S]*?)\"\s*\))"), 16u },
                { std::regex(R"(hexu32!\s*\(\s*\"([\s\S]*?)\"\s*\))"), 32u },
                { std::regex(R"(hexu64!\s*\(\s*\"([\s\S]*?)\"\s*\))"), 64u }
            };
            for (size_t r = 0; r < rules.size(); ++r) {
                cmatch m; size_t pos = 0; out.clear(); out.reserve(s.size());
                while (search_from(s, pos, m, rules[r].re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    out += fold(m[1].str(), rules[r].bits);
                }
                out.append(s, pos, std::string::npos); s.swap(out);
            }
            return s;
        }

        // hexbytes!("DE AD BE EF") -> ((const unsigned char[]){0xDE,0xAD,0xBE,0xEF})
        // hexlen!("DE AD") -> integer count
        static std::string lower_hex_bytes_and_len(const std::string& src) {
            auto mkbytes = [](const std::string& in)->std::pair<std::string, int> {
                std::string h; h.reserve(in.size());
                for (char c : in) { if (isxdigit((unsigned char)c)) h.push_back((char)toupper((unsigned char)c)); }
                if (h.size() % 2 == 1) h = "0" + h;
                std::ostringstream b;
                int n = 0;
                for (size_t i = 0; i + 1 < h.size(); i += 2) {
                    b << "0x" << h[i] << h[i + 1];
                    if (i + 2 < h.size()) b << ",";
                    n++;
                }
                return { b.str(), n };
                };
            std::string s = src, out; out.reserve(s.size());
            // hexbytes!
            {
                std::regex re(R"(hexbytes!\s*\(\s*\"([\s\S]*?)\"\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    auto pr = mkbytes(m[1].str());
                    out += "(CS_HEX_BYTES_LIT(" + pr.first + "))";
                }
                out.append(s, pos, std::string::npos); s.swap(out);
            }
            // hexlen!
            {
                std::regex re(R"(hexlen!\s*\(\s*\"([\s\S]*?)\"\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0; out.clear(); out.reserve(s.size());
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    auto pr = mkbytes(m[1].str());
                    out += std::to_string(pr.second);
                }
                out.append(s, pos, std::string::npos);
            }
            return out;
        }

        // asm!("...") and asm!{ ... } -> CS_ASM("...") or mapped MSVC/GNU forms (via CS_ASM macro in prelude)
        static std::string lower_inline_asm(const std::string& src) {
            std::string s = src, out; out.reserve(s.size());
            // asm!("text")
            {
                std::regex re(R"(asm!\s*\(\s*\"([\s\S]*?)\"\s*\))", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    // Escape internal quotes for C string (already inside C since we compile a C TU)
                    std::string body = m[1].str();
                    // Keep as-is; the front-end is outputting raw C.
                    out += "CS_ASM(\"" + body + "\")";
                }
                out.append(s, pos, std::string::npos); s.swap(out);
            }
            // asm!{ ... } -> CS_ASM("...") with newlines escaped
            {
                std::regex re(R"(asm!\s*\{\s*([\s\S]*?)\s*\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0; out.clear(); out.reserve(s.size());
                while (search_from(s, pos, m, re)) {
                    append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);
                    std::string body = m[1].str();
                    // Replace newlines with \n and quotes with \"
                    std::string esc; esc.reserve(body.size() * 2);
                    for (char c : body) {
                        if (c == '\\') { esc += "\\\\"; }
                        else if (c == '"') { esc += "\\\""; }
                        else if (c == '\n' || c == '\r') { esc += "\\n"; }
                        else { esc.push_back(c); }
                    }
                    out += "CS_ASM(\"" + esc + "\")";
                }
                out.append(s, pos, std::string::npos);
            }
            return out;
        }

        // Apply all source lowerings (call before softline_lower)
        static std::string apply_lowerings(const std::string& src) {
            std::string t = lower_hex_numeric_underscores(src);
            t = lower_hex_uints(t);
            t = lower_hex_bytes_and_len(t);
            t = lower_inline_asm(t);
            return t;
        }

        // ---------- External assembler blocks (nasm!/masm!/gas!/wasm!) ----------
        struct AsmBlock { std::string flavor; std::string name; std::string source; };
        struct WasmBlock { std::string name; std::string wat; std::string wasmPath; std::vector<unsigned char> bytes; };

        static std::string trim(std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
        }

        static std::vector<AsmBlock> scan_asm_blocks(const std::string& src) {
            std::vector<AsmBlock> v;
            auto scan = [&](const char* tag)->void {
                std::regex re(std::string(tag) + R"(\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
                cmatch m; size_t pos = 0;
                while (search_from(src, pos, m, re)) {
                    AsmBlock b; b.flavor = trim(std::string(tag, tag + std::strlen(tag) - 1)); b.name = trim(m[1].str()); b.source = m[2].str();
                    v.push_back(std::move(b));
                }
                };
            scan("nasm!"); scan("masm!"); scan("gas!"); // wasm handled separately
            return v;
        }

        static std::vector<WasmBlock> scan_wasm_blocks(const std::string& src) {
            std::vector<WasmBlock> v;
            std::regex re(R"(wasm!\s+([A-Za-z_]\w*)\s*\{([\s\S]*?)\})", std::regex::ECMAScript);
            cmatch m; size_t pos = 0;
            while (search_from(src, pos, m, re)) {
                WasmBlock w; w.name = trim(m[1].str()); w.wat = m[2].str(); v.push_back(std::move(w));
            }
            return v;
        }

        static bool tool_exists(const std::string& cmd) {
#if defined(_WIN32)
            std::string c = cmd + " --version > NUL 2>&1";
#else
            std::string c = cmd + " --version > /dev/null 2>&1";
#endif
            return system(c.c_str()) == 0;
        }

        // Assembles all assembler blocks to object files and returns their paths.
        static std::vector<std::string> assemble_all(const std::string& srcAll, const std::string& cc, const Config& cfg, bool echo = false) {
            (void)cfg;
            auto blocks = scan_asm_blocks(srcAll);
            std::vector<std::string> objs;
            for (auto& b : blocks) {
                // Write source to temp
                std::string ext = (b.flavor == "gas" ? ".S" : ".asm");
                std::string inpath = write_temp("cscript_asm_" + b.name + ext, b.source);
                // Choose object extension
#if defined(_WIN32)
                std::string obj = write_temp("cscript_asm_" + b.name + ".obj", "");
                rm_file(obj);
#else
                std::string obj = write_temp("cscript_asm_" + b.name + ".o", "");
                rm_file(obj);
#endif
                std::string cmd; int rc = 1;
                if (b.flavor == "nasm") {
#if defined(_WIN32)
                    std::string fmt = "win64";
#else
                    std::string fmt = "elf64";
#endif
                    cmd = "nasm -f " + fmt + " -o \"" + obj + "\" \"" + inpath + "\"";
                    rc = system(cmd.c_str());
                }
                else if (b.flavor == "masm") {
#if defined(_WIN32)
                    // Prefer ml64, fallback ml
                    if (tool_exists("ml64")) {
                        cmd = "ml64 /nologo /c /Fo \"" + obj + "\" \"" + inpath + "\"";
                    }
                    else {
                        cmd = "ml /nologo /c /Fo \"" + obj + "\" \"" + inpath + "\"";
                    }
                    rc = system(cmd.c_str());
#else
                    rc = 1; // MASM not available
#endif
                }
                else if (b.flavor == "gas") {
                    // Use current C compiler as assembler
                    cmd = cc + " -c \"" + inpath + "\" -o \"" + obj + "\"";
                    rc = system(cmd.c_str());
                }
                if (echo) std::cerr << "[asm] " << b.flavor << " " << b.name << " rc=" << rc << (cmd.empty() ? "" : " cmd=" + cmd) << "\n";
                if (rc == 0) { objs.push_back(obj); }
                else { rm_file(obj); }
                // Keep sources for debugging when show_c; otherwise they live in temp dir anyway.
            }
            return objs;
        }

        // Augment build command with extra object inputs (keeps baseline flags)
        static std::string build_cmd_with_objects(const Config& cfg,
            const std::string& cc,
            const std::string& cpath,
            const std::string& out,
            bool defineProfile,
            const std::vector<std::string>& extraObjs) {
            // Reuse existing build_cmd then append objects right before output/link section if possible
            std::string base = build_cmd(cfg, cc, cpath, out, defineProfile);
            if (extraObjs.empty()) return base;

            // Heuristic: append object paths just before final output flag if present; otherwise to the end.
            std::string cmd = base;
            for (const auto& o : extraObjs) {
                cmd += " \"" + o + "\"";
            }
            return cmd;
        }

        // Compile wasm (wat -> wasm) if wat2wasm exists, then embed bytes; always embeds even if tool missing.
        static std::string emit_wasm_embeds(const std::string& srcAll, bool echo = false) {
            auto ws = scan_wasm_blocks(srcAll);
            if (ws.empty()) return std::string();
            bool have_wat2wasm = tool_exists("wat2wasm");
            std::ostringstream o;
            o << "\n/* --- Embedded WASM blobs --- */\n";
            o << "typedef struct { const char* name; const unsigned char* data; unsigned int size; } CS_EmbeddedWasm;\n";
            for (auto& w : ws) {
                std::string inpath = write_temp("cscript_wasm_" + w.name + ".wat", w.wat);
                std::string wasmPath = write_temp("cscript_wasm_" + w.name + ".wasm", "");
                rm_file(wasmPath);
                int rc = 1;
                if (have_wat2wasm) {
                    std::string cmd = "wat2wasm \"" + inpath + "\" -o \"" + wasmPath + "\"";
                    rc = system(cmd.c_str());
                    if (echo) std::cerr << "[wasm] " << w.name << " rc=" << rc << "\n";
                }
                std::vector<unsigned char> bytes;
                try {
                    std::string bin = (have_wat2wasm && rc == 0) ? read_file(wasmPath) : w.wat;
                    bytes.assign(bin.begin(), bin.end());
                }
                catch (...) {
                    // fallback to wat text
                    bytes.assign(w.wat.begin(), w.wat.end());
                }
                // Emit as C array
                o << "static const unsigned char cs_wasm_" << w.name << "[] = {";
                for (size_t i = 0; i < bytes.size(); ++i) {
                    if (i % 16 == 0) o << "\n  ";
                    o << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                        << (int)(unsigned char)bytes[i] << std::nouppercase << std::dec;
                    if (i + 1 < bytes.size()) o << ",";
                }
                if (!bytes.empty()) o << "\n";
                o << "};\n";
                o << "static const unsigned int cs_wasm_" << w.name << "_len = (unsigned int)sizeof(cs_wasm_" << w.name << ");\n";
                rm_file(inpath); rm_file(wasmPath);
            }
            // Registry
            o << "static const CS_EmbeddedWasm cs_wasms[] = {\n";
            for (auto& w : ws) {
                o << "  { \"" << w.name << "\", cs_wasm_" << w.name << ", cs_wasm_" << w.name << "_len },\n";
            }
            o << "};\n";
            o << "static const unsigned int cs_wasms_count = (unsigned int)(sizeof(cs_wasms)/sizeof(cs_wasms[0]));\n";
            return o.str();
        }

    } // namespace cs_asmpack

// ============================ Ritual-Safe Build Pack (append-only) ============================
    namespace cs_ritual {

        struct ExtraCli {
            bool dump_ir = false;      // --dump-ir (embed-LLVM path preferred)
            bool emit_obj = false;     // --emit-obj
            bool relaxed_cleanup = false; // --relaxed
            bool show_c = false;       // --show-c (echo lowered C for each pass)
        };

        // ------------- CI TMPDIR normalization -------------
        static void ensure_tmpdir_for_ci() {
#if defined(_WIN32)
            auto setenv_kv = [](const char* k, const char* v) { SetEnvironmentVariableA(k, v); };
            char buf[MAX_PATH]; GetTempPathA(MAX_PATH, buf);
            const std::string sysTmp = buf;
#else
            auto setenv_kv = [](const char* k, const char* v) { setenv(k, v, 1); };
            const std::string sysTmp = "/tmp/";
#endif
            auto is_true = [](const char* v)->bool {
                if (!v) return false;
                std::string s(v); for (auto& c : s) c = (char)tolower((unsigned char)c);
                return s == "1" || s == "true" || s == "yes" || s == "on";
                };
#if defined(_WIN32)
            char* ci = nullptr; size_t len = 0;
            _dupenv_s(&ci, &len, "CI");
            const bool on = is_true(ci); if (ci) free(ci);
#else
            const bool on = is_true(std::getenv("CI")) || is_true(std::getenv("GITHUB_ACTIONS"));
#endif
            if (on) {
                setenv_kv("TMPDIR", sysTmp.c_str());
#if defined(_WIN32)
                // Align common variants
                setenv_kv("TEMP", sysTmp.c_str());
                setenv_kv("TMP", sysTmp.c_str());
#endif
            }
        }

        // ------------- @use imports and @unit manifest -------------
        static std::string trim(std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
            size_t b = s.find_last_not_of(" \t\r\n");  return s.substr(a, b - a + 1);
        }

        // Expand lines: @use "file.csc" -> inject file contents in-place (one level, conservative).
        static std::string apply_use_includes(const std::string& src, bool echo = false) {
            std::istringstream ss(src);
            std::ostringstream out;
            std::string line;
            while (std::getline(ss, line)) {
                std::string t = trim(line);
                if (t.rfind("@use", 0) == 0) {
                    std::istringstream ls(t.substr(4));
                    std::string path;
                    if (ls.peek() == '"' || ls.peek() == '\'') ls >> std::quoted(path); else ls >> path;
                    try {
                        out << read_file(path);
                        if (echo) std::cerr << "[ritual] used: " << path << "\n";
                    }
                    catch (...) {
                        std::cerr << "[ritual] warning: @use failed for " << path << "\n";
                    }
                    continue;
                }
                out << line << "\n";
            }
            return out.str();
        }

        static std::string unit_manifest_template() {
            return
                R"(@unit
# C-Script Ritual Manifest (template)
@hardline on
@softline on
@opt O2
@lto on
@profile auto
@time on
# Imports (shared config)
# @use "base.csc"

# Architecture / Vectorization (optional)
# @arch x64
# @vecwidth 256
# @fastmath off

# Bounds / Safety
@bounds on
# @warn relaxed

# Graphics/DB/etc. (optional)
# @graphics software
# @opencl off
)";
        }

        // ------------- Profile auto and warn relaxed scans -------------
        static bool scan_profile_auto(const std::string& src) {
            std::istringstream ss(src); std::string line;
            while (std::getline(ss, line)) {
                std::string t = trim(line);
                if (t.rfind("@profile", 0) == 0) {
                    std::istringstream ls(t.substr(9));
                    std::string v; ls >> v;
                    return v != "off"; // 'auto' or 'on' -> true
                }
            }
            return false;
        }
        static bool scan_warn_relaxed(const std::string& src) {
            std::istringstream ss(src); std::string line;
            while (std::getline(ss, line)) {
                std::string t = trim(line);
                if (t.rfind("@warn", 0) == 0) {
                    std::istringstream ls(t.substr(5));
                    std::string v; ls >> v;
                    return v == "relaxed";
                }
            }
            return false;
        }

        // ------------- Exhaustiveness relaxed prelude -------------
        static std::string prelude_exhaustive_relaxed() {
            return R"(
/* --- Exhaustiveness (relaxed) macros --- */
#ifndef CS_EXHAUSTIVE_RELAXED_INCLUDED
#define CS_EXHAUSTIVE_RELAXED_INCLUDED 1
#define CS_SWITCH_EXHAUSTIVE_RELAXED(T, expr) do { int __cs_hit=0; T __cs_v=(expr); switch(__cs_v){
#define CS_CASE_R(x) case x: __cs_hit=1
#define CS_SWITCH_RELAXED_END(T, expr) default: break; } do{ (void)__cs_hit; \
    if (!cs__enum_is_valid_##T((int)__cs_v)) { fprintf(stderr, "[warn] non-exhaustive switch (" #T ") value=%d at %s:%d\n", (int)__cs_v, __FILE__, __LINE__); } \
} while(0); } while(0)
#endif
)";
        }

        // ------------- Exhaustiveness checker that can warn instead of exit -------------
        static int check_exhaustiveness_relaxed(const std::string& src,
            const std::map<std::string, EnumInfo>& enums,
            bool relaxed) {
            // Reuse the existing pattern in check_exhaustiveness_or_die, but warn when relaxed.
            size_t i = 0, n = src.size();
            int errs = 0;
            while (true) {
                size_t a = src.find("CS_SWITCH_EXHAUSTIVE(", i);
                if (a == std::string::npos) break;
                size_t tstart = a + strlen("CS_SWITCH_EXHAUSTIVE(");
                size_t p = tstart; while (p < n && isspace((unsigned char)src[p])) ++p;
                size_t q = p; while (q < n && (isalnum((unsigned char)src[q]) || src[q] == '_')) ++q;
                std::string Type = src.substr(p, q - p);
                if (Type.empty()) { i = a + 1; continue; }
                std::string endKey = std::string("CS_SWITCH_END(") + Type;
                size_t b = src.find(endKey, q);
                if (b == std::string::npos) {
                    auto lc = line_col_at(src, a);
                    std::cerr << (relaxed ? "[warn]" : "error:") << " unmatched CS_SWITCH_EXHAUSTIVE for '" << Type
                        << "' at " << lc.first << ":" << lc.second << "\n";
                    if (!relaxed) return 1; else { i = a + 1; continue; }
                }
                std::string region = src.substr(a, b - a);
                std::regex caseRe(R"(CS_CASE\s*\(\s*([A-Za-z_]\w*)\s*\))");
                cs_regex_wrap::cmatch m; std::set<std::string> seen; size_t pos2 = 0;
                while (cs_regex_wrap::search_from(region, pos2, m, caseRe)) { seen.insert(m[1].str()); }
                auto itE = enums.find(Type);
                if (itE != enums.end()) {
                    const auto& universe = itE->second.members;
                    std::vector<std::string> missing; for (const auto& e : universe) if (!seen.count(e)) missing.push_back(e);
                    if (!missing.empty()) {
                        auto lc = line_col_at(src, a);
                        if (relaxed) {
                            std::cerr << "[warn] non-exhaustive switch for enum '" << Type << "' at "
                                << lc.first << ":" << lc.second << " missing:";
                            for (auto& mname : missing) std::cerr << " " << mname;
                            std::cerr << "\n";
                        }
                        else {
                            std::cerr << "error: non-exhaustive switch for enum '" << Type << "' at "
                                << lc.first << ":" << lc.second << " missing:";
                            for (auto& mname : missing) std::cerr << " " << mname;
                            std::cerr << "\n"; return 1;
                        }
                    }
                }
                i = b + 1;
            }
            return errs;
        }

        // ------------- CLI helpers (parse and build command variants) -------------
        static ExtraCli parse_extra_cli(const std::vector<std::string>& argv) {
            ExtraCli e{};
            for (size_t i = 0; i < argv.size(); ++i) {
                const std::string& a = argv[i];
                if (a == "--dump-ir") e.dump_ir = true;
                else if (a == "--emit-obj") e.emit_obj = true;
                else if (a == "--relaxed") e.relaxed_cleanup = true;
                else if (a == "--show-c") e.show_c = true;
            }
            return e;
        }

        static std::string build_cmd_emit_obj(const Config& cfg, const std::string& cc, const std::string& cpath, const std::string& objOut) {
            std::vector<std::string> cmd; cmd.push_back(cc);
            bool msvc = (cc == "cl" || cc == "clang-cl");
            auto add = [&](const std::string& s) { cmd.push_back(s); };
            if (msvc) {
                add("/nologo");
                if (cfg.opt == "O0") add("/Od"); else if (cfg.opt == "O1") add("/O1"); else add("/O2");
                if (cfg.lto) add("/GL");
                if (cfg.hardline || cfg.strict) { add("/Wall"); add("/WX"); }
                if (cfg.hardline) add("/DCS_HARDLINE=1");
                for (auto& d : cfg.defines) add("/D" + d);
                for (auto& p : cfg.incs)    add("/I" + p);
                add("/c");      // compile only
                add(cpath);
                add("/Fo:" + objOut);
            }
            else {
                add("-std=c11");
                if (cfg.opt == "O0") add("-O0");
                else if (cfg.opt == "O1") add("-O1");
                else if (cfg.opt == "O2") add("-O2");
                else if (cfg.opt == "O3") add("-O3");
                else if (cfg.opt == "size") add("-Os");
                if (cfg.lto) add("-flto");
                if (cfg.hardline) { add("-Wall"); add("-Wextra"); add("-Werror"); add("-Wconversion"); add("-Wsign-conversion"); add("-DCS_HARDLINE=1"); }
                for (auto& d : cfg.defines) add("-D" + d);
                for (auto& p : cfg.incs)    add("-I" + p);
                add("-c"); add(cpath);
                add("-o"); add(objOut);
            }
            std::string full;
            for (size_t i = 0; i < cmd.size(); ++i) { if (i) full.push_back(' '); bool q = cmd[i].find(' ') != std::string::npos; if (q) full.push_back('"'); full += cmd[i]; if (q) full.push_back('"'); }
            return full;
        }

        static void log_link_details_when_strict(const Config& cfg, const std::string& cc) {
            if (!cfg.strict && !cfg.hardline) return;
            std::cerr << "[link] cc=" << cc << "\n";
            if (!cfg.libpaths.empty()) {
                std::cerr << "[link] libpaths:";
                for (auto& p : cfg.libpaths) std::cerr << " " << p;
                std::cerr << "\n";
            }
            if (!cfg.links.empty()) {
                std::cerr << "[link] libs:";
                for (auto& l : cfg.links) std::cerr << " " << l;
                std::cerr << "\n";
            }
        }

        // ------------- Ritual two-pass builder (automates @profile auto) -------------
        struct RitualResult { int rc = 0; std::string exe; };

        static RitualResult build_two_pass_ritual(Config cfg,
            const std::string& cc,
            const std::string& srcAll,
            const ExtraCli& xcli) {
            RitualResult rr{};
            ensure_tmpdir_for_ci();

            // Expand @use includes first
            std::string source = apply_use_includes(srcAll, /*echo*/cfg.show_c);

            const bool wantProfile = scan_profile_auto(source) || cfg.profile;
            const bool warnRelaxed = scan_warn_relaxed(source);

            // Parse -> body (reuse host pipeline pieces)
            std::vector<std::string> bodyLines;
            parse_directives_and_collect(source, cfg, bodyLines);
            std::string body; for (auto& l : bodyLines) { body += l; body.push_back('\n'); }

            // enum! lower + exhaustiveness (warn or die)
            std::map<std::string, EnumInfo> enums;
            std::string enumLowered = lower_enum_bang_and_collect(body, enums);
            if (warnRelaxed) {
                (void)check_exhaustiveness_relaxed(body, enums, /*relaxed*/true);
            }
            else {
                check_exhaustiveness_or_die(body, enums);
            }

            // unsafe + match + softline setup (reuse host passes)
            std::string unsafeLowered = lower_unsafe_blocks(enumLowered);
            std::string matchLowered = lower_match_patterns(unsafeLowered);

            auto build_once_C = [&](const std::string& c_src, const std::string& out, bool defineProfile)->int {
                std::string cpath = write_temp("cscript_ritual.c", c_src);
                if (xcli.emit_obj) {
                    std::string objOut = out + ".obj";
#if !defined(_WIN32)
                    objOut = out + ".o";
#endif
                    std::string cmd = build_cmd_emit_obj(cfg, cc, cpath, objOut);
                    if (xcli.show_c) { std::cerr << "--- generated C ---\n" << c_src << "\n--- end ---\n"; }
                    int rc = run_cmd(cmd, /*echo*/true);
                    if (!xcli.relaxed_cleanup) rm_file(cpath);
                    return rc;
                }
                else {
                    std::string cmd = build_cmd(cfg, cc, cpath, out, defineProfile);
                    if (xcli.show_c) { std::cerr << "--- generated C ---\n" << c_src << "\n--- end ---\n"; }
                    log_link_details_when_strict(cfg, cc);
                    int rc = run_cmd(cmd, /*echo*/cfg.show_c);
                    if (!xcli.relaxed_cleanup) rm_file(cpath);
                    return rc;
                }
                };

            auto mk_csrc = [&](const std::string& lowered) {
                std::string csrc = prelude(cfg.hardline);
                if (warnRelaxed) csrc += prelude_exhaustive_relaxed();
                csrc += "\n"; csrc += lowered;
                return csrc;
                };

            // Pass 1 (instrumented) + run
            std::set<std::string> hotFns;
            if (wantProfile) {
                std::string inst = softline_lower(matchLowered, cfg.softline, /*hot*/{}, /*instrument*/true);
                std::string csrc1 = mk_csrc(inst);

                std::string tempExe;
#if defined(_WIN32)
                tempExe = write_temp("cscript_ritual_prof.exe", "");
#else
                tempExe = write_temp("cscript_ritual_prof.out", "");
#endif
                rm_file(tempExe);
                int rc1 = build_once_C(csrc1, tempExe, /*defineProfile*/true);
                if (rc1 != 0) { rr.rc = rc1; return rr; }

                // Run once and read counts
                std::string profPath = write_temp("cscript_profile.txt", ""); rm_file(profPath);
                int rcRun = run_exe_with_env(tempExe, "CS_PROFILE_OUT", profPath);
                if (rcRun != 0) std::cerr << "[ritual] instrumented run rc=" << rcRun << "\n";
                auto counts = read_profile_counts(profPath);
                hotFns = select_hot_functions(counts, 16);
                if (!xcli.relaxed_cleanup) { rm_file(profPath); rm_file(tempExe); }
            }

            // Pass 2 (optimized)
            std::string lowered2 = softline_lower(matchLowered, cfg.softline, hotFns, /*instrument*/false);
            std::string csrc2 = mk_csrc(lowered2);

            rr.exe = cfg.out;
            rr.rc = build_once_C(csrc2, rr.exe, /*defineProfile*/false);
            return rr;
        }

    } // namespace cs_ritual

	// ============================ Public API ============================

    int build_cscript_exe(const Config& cfg,
        const std::string& cc,
        const std::string& srcAll,
        const std::vector<std::string>& extraCli,
        std::string* outExe) {
        using namespace cs_asmpack;
        using namespace cs_ritual;
        ExtraCli xcli = parse_extra_cli(extraCli);
        if (cfg.ritual) {
            auto rr = build_two_pass_ritual(cfg, cc, srcAll, xcli);
            if (outExe) *outExe = rr.exe;
            return rr.rc;
        }
        ensure_tmpdir_for_ci();
        // Apply source lowerings
        std::string lowered = apply_lowerings(srcAll);
        if (xcli.show_c) { std::cerr << "--- lowered source ---\n" << lowered << "\n--- end ---\n"; }
        // Scan and assemble external asm blocks
        auto asmObjs = assemble_all(lowered, cc, cfg, /*echo*/cfg.show_c);
        // Emit wasm embeds if any
        std::string wasmEmbeds = emit_wasm_embeds(lowered, /*echo*/cfg.show_c);
        if (!wasmEmbeds.empty()) {
            lowered += "\n/* --- WASM EMBEDS --- */\n";
            lowered += wasmEmbeds;
            lowered += "\n/* --- END WASM EMBEDS --- */\n";
        }
        // Final C source
        std::string csrc = prelude(cfg.hardline);
        csrc += "\n"; csrc += lowered;
        // Write to temp C file
        std::string cpath = write_temp("cscript_final.c", csrc);
        // Build command
		std::string outPath = cfg.out;
		if (outPath.empty()) {
            #if defined(_WIN32)
			outPath = "cscript_out.exe";
#else
			outPath = "cscript_out.out";
#endif
            }
        std::string cmd = build_cmd_with_objects(cfg, cc, cpath, outPath, /*defineProfile*/false, asmObjs);
        if (xcli.show_c) { std::cerr << "--- final C ---\n" << csrc << "\n--- end ---\n"; }
        log_link_details_when_strict(cfg, cc);
        int rc = run_cmd(cmd, /*echo*/cfg.show_c);
        if (!xcli.relaxed_cleanup) rm_file(cpath);
        if (outExe) *outExe = (rc == 0) ? outPath : std::string();
        return rc;
	}
	} // namespace cscript
	} // extern "C"
#else
#error "C-Script source-to-source compiler requires C++11 or later"
#endif
// End of Source2.cpp
// End of Source2.cpp
// Source2.cpp - C-Script source-to-source compiler (C++11)
// Part of C-Script:
//
//
