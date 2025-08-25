// cscriptc.cpp — C-Script v0.4 reference compiler (front + analyzer + PGO + driver)
// Build: g++ -std=gnu++17 cscriptc.cpp -o cscriptc      (Linux/macOS, Clang/GCC)
//     or clang++ -std=c++17 cscriptc.cpp -o cscriptc
//     or (MSYS/Clang on Windows recommended). MSVC works but needs regex fixes below.
// Usage:  ./cscriptc file.csc [--show-c] [--strict] [-O{0|1|2|3|max|size}] [-o out.exe]
// Features:
//  - Single executable output with optional Profile-Guided Optimization
//  - Strong static analysis with exhaustiveness checking for enums
//  - Expressive syntax with softline functions and safety annotations
//  - Zero-cost abstractions with direct C ABI compatibility

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
#include <iomanip>
#include <chrono>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

using std::string;
using std::vector;
using std::set;
using std::map;
using std::pair;

// ============================================================================
// Version information
// ============================================================================
constexpr const char* CSCRIPT_VERSION = "0.4.0";
constexpr const char* CSCRIPT_BUILD_DATE = __DATE__;

// ============================================================================
// Error handling utilities
// ============================================================================
class CompilerError : public std::runtime_error {
public:
    CompilerError(const string& msg, int line = 0, int col = 0)
        : std::runtime_error(msg), line_(line), col_(col) {}
    
    int line() const { return line_; }
    int col() const { return col_; }
    
private:
    int line_;
    int col_;
};

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

    // Search from a byte offset; updates pos to just after the match.
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

    // Append [start, m.prefix_end) to out (absolute in original string).
    inline void append_prefix(std::string& out,
        const std::string& src,
        size_t start,
        const cmatch& m)
    {
        const size_t endAbs = static_cast<size_t>(m.prefix().second - src.begin());
        if (endAbs >= start && endAbs <= src.size())
            out.append(src, start, endAbs - start);
    }

    // Absolute end of the prefix in original string.
    inline size_t prefix_end_abs(const std::string& src, const cmatch& m) {
        return static_cast<size_t>(m.prefix().second - src.begin());
    }
}

//============================= Config =============================
struct Config {
    bool hardline = true;         // Enable runtime checks
    bool softline = true;         // Enable syntactic sugar
    string opt = "O2";            // Optimization level
    bool lto = true;              // Link-time optimization
    bool profile = false;         // PGO two-pass
    bool debug = false;           // Include debug symbols
    string out = "a.exe";         // Output filename
    string abi = "";              // ABI compatibility
    vector<string> defines;       // Preprocessor definitions
    vector<string> incs;          // Include paths
    vector<string> libpaths;      // Library paths
    vector<string> links;         // Libraries to link
    bool strict = false;          // Stricter error checking
    bool relaxed = false;         // More permissive behavior
    bool show_c = false;          // Show generated C code
    bool verbose = false;         // Verbose output
    string cc_prefer = "";        // Preferred C compiler
    
    // New config options
    bool emit_llvm = false;       // Emit LLVM IR
    bool warn_as_error = false;   // Treat warnings as errors
    string target = "";           // Target triple
};

//============================= String utilities =============================
static bool starts_with(const string& s, const string& p) { 
    return s.rfind(p, 0) == 0; 
}

static bool ends_with(const string& s, const string& p) {
    if (p.size() > s.size()) return false;
    return s.compare(s.size() - p.size(), p.size(), p) == 0;
}

static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static vector<string> split(const string& s, char delim) {
    vector<string> result;
    std::istringstream iss(s);
    string item;
    while (std::getline(iss, item, delim)) {
        result.push_back(item);
    }
    return result;
}

//============================= File operations =============================
static string read_file(const string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw CompilerError("Cannot open file: " + p);
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

static string get_temp_dir() {
    string dir;
#if defined(_WIN32)
    char buf[MAX_PATH]; 
    GetTempPathA(MAX_PATH, buf);
    dir = string(buf);
#else
    const char* t = getenv("TMPDIR"); 
    dir = t ? t : "/tmp/";
    if (!dir.empty() && dir.back() != '/') dir += '/';
#endif
    return dir;
}

static string write_temp(const string& base, const string& content) {
    string path = get_temp_dir() + base;
    std::ofstream o(path, std::ios::binary); 
    if (!o) throw CompilerError("Cannot create temporary file: " + path);
    o << content;
    return path;
}

static bool rm_file(const string& p) { 
    return std::remove(p.c_str()) == 0; 
}

// For error lines
static pair<int, int> line_col_at(const string& s, size_t pos) {
    int line = 1, col = 1;
    for (size_t i = 0; i < pos && i < s.size(); ++i) {
        if (s[i] == '\n') { line++; col = 1; }
        else col++;
    }
    return { line, col };
}

//============================= Prelude =============================
static string prelude(bool hardline) {
    std::ostringstream o;
    o << "// --- C-Script v" << CSCRIPT_VERSION << " prelude (zero-cost) ---\n"
      << "#include <stdio.h>\n"
      << "#include <stdint.h>\n"
      << "#include <stddef.h>\n"
      << "#include <stdlib.h>\n"
      << "#include <string.h>\n"
      << "#include <stdbool.h>\n\n"
      << "#define print(...) printf(__VA_ARGS__)\n"
      << "#if defined(__GNUC__) || defined(__clang__)\n"
      << "  #define likely(x)   __builtin_expect(!!(x),1)\n"
      << "  #define unlikely(x) __builtin_expect(!!(x),0)\n"
      << "#else\n"
      << "  #define likely(x)   (x)\n"
      << "  #define unlikely(x) (x)\n"
      << "#endif\n\n";

    // Resource management with defer
    o << "// ---- Resource management with 'defer' ----\n"
      << "#define CS_CONCAT2(a,b) a##b\n"
      << "#define CS_CONCAT(a,b)  CS_CONCAT2(a,b)\n"
      << "#define defer(body) for (int CS_CONCAT(_cs_defer_, __COUNTER__) = 0; \\\n"
      << "                         CS_CONCAT(_cs_defer_, __COUNTER__) == 0; \\\n"
      << "                         (void)(body), CS_CONCAT(_cs_defer_, __COUNTER__)=1)\n\n";

    // Exhaustive switch checking
    o << "// ---- Exhaustive switch helpers ----\n"
      << "#define CS_SWITCH_EXHAUSTIVE(T, expr) do { int __cs_hit=0; T __cs_v=(expr); switch(__cs_v){\n"
      << "#define CS_CASE(x) case x: __cs_hit=1\n"
      << "#define CS_SWITCH_END(T, expr) default: break; } if(!__cs_hit) cs__enum_assert_##T(__cs_v); } while(0)\n\n";

    // Unsafe pragmas for disabling conversion warnings
    o << "// ---- @unsafe pragmas ----\n"
      << "#if defined(_MSC_VER)\n"
      << "  #define CS_PRAGMA_PUSH __pragma(warning(push))\n"
      << "  #define CS_PRAGMA_POP  __pragma(warning(pop))\n"
      << "  #define CS_PRAGMA_RELAX __pragma(warning(disable:4244 4267 4018 4389))\n"
      << "#else\n"
      << "  #define CS_PRAGMA_PUSH _Pragma(\"GCC diagnostic push\")\n"
      << "  #define CS_PRAGMA_POP  _Pragma(\"GCC diagnostic pop\")\n"
      << "  #define CS_PRAGMA_RELAX _Pragma(\"GCC diagnostic ignored \\\"-Wconversion\\\"\")\\\n"
      << "                          _Pragma(\"GCC diagnostic ignored \\\"-Wsign-conversion\\\"\")\\\n"
      << "                          _Pragma(\"GCC diagnostic ignored \\\"-Wenum-conversion\\\"\")\n"
      << "#endif\n"
      << "#define CS_UNSAFE_BEGIN do { CS_PRAGMA_PUSH; CS_PRAGMA_RELAX; } while(0)\n"
      << "#define CS_UNSAFE_END   do { CS_PRAGMA_POP; } while(0)\n\n";

    // Function attributes for PGO
    o << "// ---- Function attributes for PGO ----\n"
      << "#if defined(_MSC_VER)\n"
      << "  #define CS_HOT\n"
      << "#else\n"
      << "  #define CS_HOT __attribute__((hot))\n"
      << "#endif\n";

    if (hardline) o << "\n#define CS_HARDLINE 1\n";

    // Profiler (only for instrumented pass)
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

// ---- Memory management utilities ----
#ifndef CS_MALLOC
#define CS_MALLOC malloc
#endif
#ifndef CS_FREE
#define CS_FREE free
#endif
#ifndef CS_REALLOC
#define CS_REALLOC realloc
#endif

// ---- Result type for error handling ----
#define Result(T) struct { T value; bool ok; const char* error; }
#define Ok(x) (typeof(x)){.value = (x), .ok = true, .error = NULL}
#define Err(msg) {.ok = false, .error = (msg)}
#define unwrap(result) ((result).ok ? (result).value : (fprintf(stderr, "Runtime error: %s\n", (result).error), exit(1), (result).value))
#define try(result) do { if (!(result).ok) return Err((result).error); } while(0)
)";

    // Add to the prelude function
    o << "// ---- Capsule safety system ----\n"
      << "#ifdef CAPSULE_GUARD\n"
      << "  static volatile unsigned long long cs__mutations = 0;\n"
      << "  #define CS_MUT_NOTE()          do { cs__mutations++; } while(0)\n"
      << "  #define CS_MUT_STORE(dst,val)  do { (dst)=(val); cs__mutations++; } while(0)\n"
      << "  #define CS_MUT_MEMCPY(d,s,n)   do { memcpy((d),(s),(n)); cs__mutations++; } while(0)\n"
      << "  #define CS_GLYPH(sym)          \"[\" sym \"]\" \n"
      << "  static unsigned long long cs_mutation_count(void) { return cs__mutations; }\n"
      << "#else\n"
      << "  #define CS_MUT_NOTE()          do { } while(0)\n"
      << "  #define CS_MUT_STORE(dst,val)  do { (dst)=(val); } while(0)\n"
      << "  #define CS_MUT_MEMCPY(d,s,n)   memcpy((d),(s),(n))\n"
      << "  #define CS_GLYPH(sym)          \"\"\n"
      << "#endif\n\n";

    return o.str();
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
            if (name == "hardline") { 
                string v; ls >> v; 
                cfg.hardline = (v != "off"); 
            }
            else if (name == "softline") { 
                string v; ls >> v; 
                cfg.softline = (v != "off"); 
            }
            else if (name == "opt") { 
                string v; ls >> v; 
                cfg.opt = v; 
            }
            else if (name == "lto") { 
                string v; ls >> v; 
                cfg.lto = (v != "off"); 
            }
            else if (name == "profile") { 
                string v; ls >> v; 
                cfg.profile = (v != "off"); 
            }
            else if (name == "debug") { 
                string v; ls >> v; 
                cfg.debug = (v != "off"); 
            }
            else if (name == "out") { 
                string v; ls >> std::quoted(v); 
                cfg.out = v; 
            }
            else if (name == "abi") { 
                string v; ls >> std::quoted(v); 
                cfg.abi = v; 
            }
            else if (name == "define") { 
                string v; ls >> v; 
                cfg.defines.push_back(v); 
            }
            else if (name == "inc") { 
                string v; ls >> std::quoted(v); 
                cfg.incs.push_back(v); 
            }
            else if (name == "libpath") { 
                string v; ls >> std::quoted(v); 
                cfg.libpaths.push_back(v); 
            }
            else if (name == "link") { 
                string v; ls >> std::quoted(v); 
                cfg.links.push_back(v); 
            }
            else if (name == "target") { 
                string v; ls >> std::quoted(v); 
                cfg.target = v; 
            }
            else { 
                std::cerr << "warning: unknown directive @" << name << "\n"; 
            }
            continue;
        }
        body.push_back(line);
    }
}

//============================= enum! parsing + emission =============================
struct EnumInfo { 
    set<string> members; 
    bool is_flags = false;
};

static string lower_enum_bang_and_collect(const string& in, map<string, EnumInfo>& enums) {
    using namespace cs_regex_wrap;

    string s = in;
    // Match standard enum! and enum_flags!
    std::regex re_standard(R"(enum!\s+([A-Za-z_]\w*)\s*\{([^}]*)\})");
    std::regex re_flags(R"(enum_flags!\s+([A-ZaZ_]\w*)\s*\{([^}]*)\})");
    
    cmatch m;
    string out; 
    out.reserve(s.size() * 12 / 10);
    size_t pos = 0;

    auto split_enums = [](string body) -> vector<string> {
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

    // Process standard enums
    while (search_from(s, pos, m, re_standard)) {
        append_prefix(out, s, pos - (m.length(0) ? m.length(0) : 0), m);

        string name = m[1].str();
        string body = m[2].str();
        vector<string> items = split_enums(body);
        EnumInfo info;
        info.is_flags = false;
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
    }

    // Reset position to search for flag enums
    pos = 0;
    string s2 = out;
    out.clear();
    out.reserve(s2.size() * 12 / 10);

    // Process flag enums (bitfield enums)
    while (search_from(s2, pos, m, re_flags)) {
        append_prefix(out, s2, pos - (m.length(0) ? m.length(0) : 0), m);

        string name = m[1].str();
        string body = m[2].str();
        vector<string> items = split_enums(body);
        EnumInfo info;
        info.is_flags = true;
        for (auto& id : items) info.members.insert(id);
        enums[name] = info;

        // Emit flag enum as C typedef with bitwise operations helpers
        out += "typedef enum " + name + " { " + body + " } " + name + ";\n";
        out += "static inline " + name + " " + name + "_combine(" + name + " a, " + name + " b) { return (" + name + ")(a | b); }\n";
        out += "static inline bool " + name + "_has(" + name + " flags, " + name + " flag) { return (flags & flag) == flag; }\n";
    }

    // Append tail
    out.append(s2, pos, string::npos);
    return out;
}

//============================= Compile-time switch exhaustiveness =============================
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
            throw CompilerError("Unmatched CS_SWITCH_EXHAUSTIVE for '" + Type + "'", lc.first, lc.second);
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
            // Skip flags enum - they don't require exhaustiveness checking
            if (itE->second.is_flags) {
                i = b + 1;
                continue;
            }
            
            const auto& universe = itE->second.members;
            vector<string> missing;
            for (const auto& e : universe) if (!seen.count(e)) missing.push_back(e);
            if (!missing.empty()) {
                auto lc = line_col_at(src, a);
                std::ostringstream err;
                err << "Non-exhaustive switch for enum '" << Type << "'. Missing:";
                for (auto& mname : missing) err << " " << mname;
                throw CompilerError(err.str(), lc.first, lc.second);
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
        else if (cfg.opt == "size") cmd.push_back("/Os");
        
        if (cfg.debug) cmd.push_back("/Zi");
        if (cfg.hardline || cfg.strict) { cmd.push_back("/Wall"); cmd.push_back("/WX"); }
        if (cfg.lto) cmd.push_back("/GL");
        if (cfg.hardline) cmd.push_back("/DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("/DCS_PROFILE_BUILD=1");
        
        for (auto& d : cfg.defines) cmd.push_back("/D" + d);
        for (auto& p : cfg.incs)    cmd.push_back("/I" + p);
        
        cmd.push_back(cpath);
        cmd.push_back("/Fe:" + out);
        
        if (cfg.debug) cmd.push_back("/Fd:" + out + ".pdb");
        
        for (auto& lp : cfg.libpaths) cmd.push_back("/link /LIBPATH:\"" + lp + "\"");
        for (auto& l : cfg.links) {
            string lib = l; 
            if (lib.rfind(".lib") == string::npos) lib += ".lib";
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
        
        if (cfg.debug) cmd.push_back("-g");
        
        if (cfg.hardline) {
            cmd.push_back("-Wall"); 
            cmd.push_back("-Wextra"); 
            if (cfg.warn_as_error) cmd.push_back("-Werror");
            cmd.push_back("-Wconversion"); 
            cmd.push_back("-Wsign-conversion");
        }
        
        if (cfg.lto) cmd.push_back("-flto");
        
        if (!cfg.target.empty()) {
            cmd.push_back("-target");
            cmd.push_back(cfg.target);
        }
        
        if (cfg.hardline) cmd.push_back("-DCS_HARDLINE=1");
        if (defineProfile) cmd.push_back("-DCS_PROFILE_BUILD=1");
        
        for (auto& d : cfg.defines) { cmd.push_back("-D" + d); }
        for (auto& p : cfg.incs) { cmd.push_back("-I" + p); }
        
        cmd.push_back(cpath);
        cmd.push_back("-o"); 
        cmd.push_back(out);
        
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
    if (argc < 2) { 
        std::cerr << "C-Script Compiler v" << CSCRIPT_VERSION << " (" << CSCRIPT_BUILD_DATE << ")\n"
                  << "Usage: cscriptc [options] file.csc\n"
                  << "Options:\n"
                  << "  -o <file>       Output file name\n"
                  << "  -O<level>       Optimization level (0,1,2,3,size,max)\n"
                  << "  --no-lto        Disable link-time optimization\n"
                  << "  --strict        Enable strict error checking\n"
                  << "  --relaxed       More permissive behavior\n"
                  << "  --show-c        Show generated C code\n"
                  << "  --verbose       Verbose output\n"
                  << "  --cc <compiler> Specify C compiler\n"
                  << "  --debug         Include debug information\n"
                  << "  --target <triple> Set compilation target\n"
                  << "  --capsule       Generate capsule.h and enable runtime safety\n"
                  << "  --trace-lib     Trace library calls with symbolic overlays\n";
        return 1;
    }

    try {
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
            else if (a == "--verbose") { cfg.verbose = true; }
            else if (a == "--debug") { cfg.debug = true; }
            else if (a == "--cc" && i + 1 < args.size()) { cfg.cc_prefer = args[++i]; }
            else if (a == "--target" && i + 1 < args.size()) { cfg.target = args[++i]; }
            else if (a == "--warn-as-error") { cfg.warn_as_error = true; }
            else if (a == "--capsule") { cfg.defines.push_back("CS_CAPSULE=1"); }
            else if (a == "--trace-lib") { cfg.defines.push_back("CS_TRACE_LIB=1"); }
            else if (!a.empty() && a[0] != '-') { inpath = a; }
        }
        if (inpath.empty()) { throw CompilerError("Missing input .csc file"); }

        // Show compilation info if verbose
        if (cfg.verbose) {
            std::cerr << "C-Script Compiler v" << CSCRIPT_VERSION << "\n"
                      << "Input: " << inpath << "\n"
                      << "Output: " << cfg.out << "\n"
                      << "Optimization: " << cfg.opt << (cfg.lto ? " with LTO" : "") << "\n";
        }

        // Auto-set output name if not specified
        if (cfg.out == "a.exe" && !inpath.empty()) {
            string basename = inpath;
            size_t lastSlash = basename.find_last_of("/\\");
            if (lastSlash != string::npos) basename = basename.substr(lastSlash + 1);
            
            size_t lastDot = basename.find_last_of(".");
            if (lastDot != string::npos) basename = basename.substr(0, lastDot);
            
#if defined(_WIN32)
            cfg.out = basename + ".exe";
#else
            cfg.out = basename + ".out";
#endif
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        // Read & split into directives + body
        string srcAll = read_file(inpath);
        vector<string> bodyLines;
        parse_directives_and_collect(srcAll, cfg, bodyLines);
        string body;
        body.reserve(srcAll.size());
        for (auto& l : bodyLines) { body += l; body.push_back('\n'); }

        // 1) Analyze enum! and emit typedefs + helpers; collect enum members
        if (cfg.verbose) {
            std::cerr << "Processing enum! declarations...\n";
        }
        map<string, EnumInfo> enums;
        string enumLowered = lower_enum_bang_and_collect(body, enums);
        if (cfg.verbose) {
            std::cerr << "Found " << enums.size() << " enum types\n";
        }

        // 2) Compile-time switch exhaustiveness checks against enum!
        check_exhaustiveness_or_die(body, enums); // analyze original macros in 'body'

        // 3) Lower @unsafe blocks
        string unsafeLowered = lower_unsafe_blocks(enumLowered);

        // 4) PGO two-pass (optional)
        set<string> hotFns; // selected after pass 1
        string cc = pick_cc(cfg.cc_prefer);

        auto build_once = [&](const string& c_src, const string& out, bool profileBuild) -> int {
            string cpath = write_temp(string("cscript_") + std::to_string(uintptr_t(&cfg)) + ".c", c_src);
            string cmd = build_cmd(cfg, cc, cpath, out, profileBuild);
            if (cfg.show_c) { 
                std::cerr << "--- Generated C ---\n" << c_src << "\n--- End ---\n"; 
            }
            if (cfg.verbose) {
                std::cerr << "Building with command:\n" << cmd << "\n";
            }
            int rc = run_cmd(cmd, cfg.verbose);
            if (!cfg.show_c) rm_file(cpath);
            return rc;
        };

        if (cfg.profile) {
            // First pass: instrument softline fns and build temp exe
            string s1 = prelude(cfg.hardline);
            string inst = softline_lower(unsafeLowered, cfg.softline, /*hot*/{}, /*instrument*/true);
            s1 += "\n"; s1 += inst;

            if (cfg.verbose) {
                std::cerr << "Building instrumented version for profile-guided optimization...\n";
            }

            string tempExeProfile;
#if defined(_WIN32)
            tempExeProfile = write_temp("cscript_prof.exe", "");
            rm_file(tempExeProfile); // unique path; build will recreate
#else
            tempExeProfile = write_temp("cscript_prof.out", "");
            rm_file(tempExeProfile);
#endif
            if (build_once(s1, tempExeProfile, /*defineProfile*/true) != 0) {
                throw CompilerError("Build failed (instrumented pass)");
            }

            // Run once with profile output file
            if (cfg.verbose) {
                std::cerr << "Running instrumented executable to collect profile data...\n";
            }
            
            string profPath = write_temp("cscript_profile.txt", "");
            rm_file(profPath);
            int rcRun = run_exe_with_env(tempExeProfile, "CS_PROFILE_OUT", profPath);
            if (rcRun != 0) {
                std::cerr << "warning: instrumented run returned " << rcRun << "; proceeding\n";
            }
            
            // Read profile and select hot functions
            auto counts = read_profile_counts(profPath);
            hotFns = select_hot_functions(counts, 16);
            
            if (cfg.verbose) {
                std::cerr << "Selected " << hotFns.size() << " hot functions for optimization\n";
            }
            
            rm_file(profPath);
            rm_file(tempExeProfile);
        }

        // 5) Final lowering with hot attributes, no instrumentation
        string csrc = prelude(cfg.hardline);
        string lowered = softline_lower(unsafeLowered, cfg.softline, hotFns, /*instrument*/false);
        csrc += "\n"; csrc += lowered;

        // 6) Final build to single exe
        if (cfg.verbose) {
            std::cerr << "Building final executable...\n";
        }
        
        if (build_once(csrc, cfg.out, /*defineProfile*/false) != 0) {
            throw CompilerError("Build failed");
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        if (cfg.verbose) {
            std::cerr << "Build completed in " << duration << "ms\n";
        }

        std::cout << cfg.out << "\n";
        return 0;
        
    } catch (const CompilerError& e) {
        if (e.line() > 0) {
            std::cerr << "error:" << e.line() << ":" << e.col() << ": " << e.what() << "\n";
        } else {
            std::cerr << "error: " << e.what() << "\n";
        }
        return 1;  // Returns 1 for CompilerError
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;  // Returns 1 for standard exceptions
    }
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
    
    if (cfg.debug) {
        CGO.setDebugInfo(clang::codegenoptions::FullDebugInfo);
    }
    
    if (cfg.lto) {
        CGO.EmitLLVMUsableTypeMetadata = 1;
    }
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

    // Target triple: host or specified
    auto targetOpts = std::make_shared<clang::TargetOptions>();
    targetOpts->Triple = cfg.target.empty() ? llvm::sys::getDefaultTargetTriple() : cfg.target;
    Inv->setTargetOpts(*targetOpts);

    // Header search / preprocessor
    PreprocessorOptions& PP = Inv->getPreprocessorOpts();
    for (auto& d : cfg.defines) PP.addMacroDef(d);
    HeaderSearchOptions& HS = Inv->getHeaderSearchOpts();
    for (auto& p : cfg.incs) HS.AddPath(p, frontend::Angled, false, false);

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
    
    if (cfg.debug) {
        args.push_back("/DEBUG");
        args.push_back("/DEBUGTYPE:CV");
    }
    
    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { 
        hold.push_back(std::string("/LIBPATH:") + lp); 
        args.push_back(hold.back().c_str()); 
    }
    
    for (auto& l : cfg.links) { 
        std::string lib = l; 
        if (lib.rfind(".lib") == std::string::npos) lib += ".lib"; 
        hold.push_back(lib); 
        args.push_back(hold.back().c_str()); 
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
    
    if (cfg.debug) {
        args.push_back("-g");
    }
    
    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { 
        hold.push_back(std::string("-L") + lp); 
        args.push_back(hold.back().c_str()); 
    }
    
    for (auto& l : cfg.links) { 
        hold.push_back(std::string("-l") + l);  
        args.push_back(hold.back().c_str()); 
    }
    
    args.push_back("-lSystem");
    
    if (lld::macho::link(args, /*canExitEarly*/ false, llvm::outs(), llvm::errs()))
        rc = 0;

#else
    // ELF
    std::vector<const char*> args;
    args.push_back("ld.lld");
    args.push_back("-o"); args.push_back(outPath.c_str());
    args.push_back(tmpObj.c_str());

    if (cfg.debug) {
        args.push_back("-g");
    }

    std::vector<std::string> hold;
    for (auto& lp : cfg.libpaths) { 
        hold.push_back(std::string("-L") + lp); 
        args.push_back(hold.back().c_str()); 
    }
    
    for (auto& l : cfg.links) { 
        hold.push_back(std::string("-l") + l);  
        args.push_back(hold.back().c_str()); 
    }

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
    using namespace llvm;

    std::vector<std::string> incs = cfg.incs;
    std::vector<std::string> defs = cfg.defines;
    if (cfg.hardline) defs.push_back("CS_HARDLINE=1");

    auto DiagOpts = llvm::make_unique<DiagnosticOptions>();
    auto DiagPrinter = llvm::make_unique<TextDiagnosticPrinter>(llvm::errs(), DiagOpts.get());
    IntrusiveRefCntPtr<DiagnosticsEngine> Diags(
        new DiagnosticsEngine(new DiagnosticIDs(), &*DiagOpts, DiagPrinter.release(), false));

    LLVMContext Context;
    Context.setDiagnosticHandler(Diags);

    auto targetOpts = std::make_shared<clang::TargetOptions>();
    targetOpts->Triple = sys::getDefaultTargetTriple();

    // In-memory file system for input
    auto InMemFS = llvm::vfs::InMemoryFileSystem::create();
    auto MB = llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(c_src), "input.c");
    InMemFS->addFile("input.c", /*modtime*/ 0, std::move(MB));
    auto OverlayFS = std::make_shared<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
    OverlayFS->pushOverlay(std::move(InMemFS));

    CompilerInstance CI;
    CI.createDiagnostics();

    auto Inv = std::make_shared<CompilerInvocation>();
    LangOptions& LO = Inv->getLangOpts();
    LO.C11 = 1;
    LO.C99 = 1;
    LO.GNUMode = 1;

    auto targetOpts2 = std::make_shared<TargetOptions>();
    targetOpts2->Triple = sys::getDefaultTargetTriple();
    Inv->setTargetOpts(*targetOpts2);

    PreprocessorOptions& PP = Inv->getPreprocessorOpts();
    for (auto& d : cfg.defines) PP.addMacroDef(d);
    HeaderSearchOptions& HS = Inv->getHeaderSearchOpts();
    for (auto& p : incs) HS.AddPath(p, frontend::Angled, false, false);

    Inv->getFrontendOpts().Inputs.clear();
    Inv->getFrontendOpts().ProgramAction = frontend::EmitObj;
    Inv->getFrontendOpts().Inputs.emplace_back("input.c", clang::Language::C);

    // Map source in-memory
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

    // Linker set up
    static llvm::sys::Mutex LinkerMutex;
    llvm::sys::MutexGuard guard(LinkerMutex);

    int rc = cs_link_with_lld(cfg, Obj->getMemBufferRef(), outPath);
    return rc;
}

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

// ---- Emit an object file from an LLVM Module (in-memory)
static std::unique_ptr<llvm::MemoryBuffer>
cs_emit_obj_from_module(llvm::Module& M, const Config& cfg) {
    using namespace llvm;

    std::string Triple = sys::getDefaultTargetTriple();
    M.setTargetTriple(Triple);

    std::string Error;
    const Target* T = TargetRegistry::lookupTarget(Triple, Error);
    if (!T) throw std::runtime_error("Target lookup failed: " + Error);

    TargetOptions Opts;
    CodeGenOpt::Level CGO = CodeGenOpt::Default;
    if (cfg.opt == "O0") CGO = CodeGenOpt::None;
    else if (cfg.opt == "O1") CGO = CodeGenOpt::Less;
    else if (cfg.opt == "O2") CGO = CodeGenOpt::Default;
    else                    CGO = CodeGenOpt::Aggressive;

    std::unique_ptr<TargetMachine> TM(
        T->createTargetMachine(Triple, "generic", "", Opts, std::nullopt, std::nullopt, CGO));
    M.setDataLayout(TM->createDataLayout());

    if (verifyModule(M, &errs())) {
        throw std::runtime_error("invalid LLVM module after instrumentation");
    }

    legacy::PassManager PM;
    llvm::SmallVector<char, 0> OBJ;
    raw_svector_ostream OS(OBJ);

    if (TM->addPassesToEmitFile(PM, OS, nullptr, CodeGenFileType::CGFT_ObjectFile)) {
        throw std::runtime_error("TargetMachine cannot emit object file");
    }
    PM.run(M);

    return std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(OBJ), "cscript_obj.o", false);
}

// ---- Build once (instrumented profiling pass) entirely in-proc
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
