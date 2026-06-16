#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// Data structures

struct FieldEntry {
    std::string name;
    std::string type;
    uint32_t    offset{};
    bool        is_static{};
};

struct MethodEntry {
    std::string name;
    std::string return_type;
    std::string params;
    uint64_t    rva{};
    uint64_t    offset{};
};

struct ClassEntry {
    std::string              ns;
    std::string              name;
    std::string              parent;
    std::vector<FieldEntry>  fields;
    std::vector<MethodEntry> methods;

    [[nodiscard]] std::string full_name() const {
        return ns.empty() ? name : ns + "." + name;
    }
};

// Reserved C/C++ macro and keyword names that must not be used as identifiers

static const std::unordered_set<std::string> g_reserved = {
    // C stdio macros
    "stdin", "stdout", "stderr",
    // Common C macros
    "NULL", "EOF", "SEEK_SET", "SEEK_CUR", "SEEK_END",
    "EXIT_SUCCESS", "EXIT_FAILURE", "RAND_MAX",
    "INT_MIN", "INT_MAX", "UINT_MAX", "LONG_MIN", "LONG_MAX",
    "FLT_MAX", "DBL_MAX", "TRUE", "FALSE",
    // errno macros
    "errno",
    // assert
    "assert",
    // C++ keywords
    "alignas", "alignof", "and", "and_eq", "asm", "auto",
    "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "char8_t", "char16_t", "char32_t", "class", "compl",
    "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield",
    "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int",
    "long", "mutable", "namespace", "new", "noexcept", "not",
    "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "register", "reinterpret_cast", "requires",
    "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
    "while", "xor", "xor_eq",
    // Windows / MSVC common macros
    "BOOL", "BYTE", "WORD", "DWORD", "HANDLE", "HWND", "HMODULE",
    "WINAPI", "CALLBACK", "min", "max",

    // MSVC extension keywords
    "near", "far", "huge", "interface", "sealed",
    "cdecl", "pascal",

    // MSVC included types
    "_int8", "_int16", "_int32", "_int64",
    "__int8", "__int16", "__int32", "__int64",

    // MSVC calling conventions
    "__cdecl", "__stdcall", "__fastcall", "__thiscall", "__vectorcall",
    "__forceinline", "__declspec",
};

// Identifier helpers

static bool is_obfuscated(std::string_view s) {
    for (unsigned char c : s)
        if (c > 0x7E && c != '_') return true;
    return false;
}

// Convert arbitrary string → valid C++ identifier.
// Rules:
//   - non-alnum/non-underscore → '_'
//   - leading digit → prefix '_'
//   - empty result → "unnamed"
//   - reserved keyword/macro → prefix "f_"
static std::string safe_ident(std::string_view s, bool upper = false) {
    if (is_obfuscated(s)) {
        size_t h = 0;
        for (unsigned char c : s) h = h * 31 + c;
        return std::format("obf_{:08x}", static_cast<uint32_t>(h));
    }

    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        ? (upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c)
        : '_';

    if (out.empty())
        return "unnamed";

    // Leading digit → prefix with underscore
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;

    // Reserved keyword or macro → prefix with f_
    if (g_reserved.count(out))
        out = "f_" + out;

    return out;
}

// Make a namespace-level identifier unique within a set of already-used names.
// Returns the (possibly suffixed) unique name and inserts it into `used`.
static std::string unique_ident(const std::string& base, std::unordered_set<std::string>& used) {
    if (!used.count(base)) {
        used.insert(base);
        return base;
    }
    for (int i = 2; ; ++i) {
        std::string candidate = base + "_" + std::to_string(i);
        if (!used.count(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
}

// Utilities

static std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1'000'000;
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(6) << std::setfill('0') << us.count() << " UTC";
    return ss.str();
}

static std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

// Merging: collapse duplicate full_name classes into one, dedup fields

static std::vector<ClassEntry> merge_classes(std::vector<ClassEntry> raw) {
    // Preserve insertion order
    std::vector<std::string> order;
    std::unordered_map<std::string, ClassEntry> merged;

    for (auto& cls : raw) {
        const std::string key = cls.full_name();

        if (!merged.count(key)) {
            order.push_back(key);
            merged[key] = std::move(cls);
            continue;
        }

        auto& dst = merged[key];

        // Collect already-known field identifiers (after safe_ident transform)
        std::unordered_set<std::string> seen_fields;
        for (auto& f : dst.fields)
            seen_fields.insert(safe_ident(f.name) + (f.is_static ? "_static" : ""));

        for (auto& f : cls.fields) {
            std::string id = safe_ident(f.name) + (f.is_static ? "_static" : "");
            if (!seen_fields.count(id)) {
                seen_fields.insert(id);
                dst.fields.push_back(f);
            }
        }

        // Collect already-known method signatures
        std::unordered_set<std::string> seen_methods;
        for (auto& m : dst.methods)
            seen_methods.insert(m.name + m.params);

        for (auto& m : cls.methods) {
            if (!seen_methods.count(m.name + m.params)) {
                seen_methods.insert(m.name + m.params);
                dst.methods.push_back(m);
            }
        }
    }

    std::vector<ClassEntry> result;
    result.reserve(order.size());
    for (auto& k : order)
        result.push_back(std::move(merged[k]));
    return result;
}

// Parser

std::vector<ClassEntry> parse_dump_cs(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << std::format("[-] Cannot open: {}\n", path.string());
        return {};
    }

    static const std::regex re_namespace(R"(^//\s*Namespace:\s*(.*?)\s*$)");
    static const std::regex re_class_decl(R"(\bclass\s+(\S+?)(?:<[^>]*>)?(?:\s*:\s*(\S+?))?\s*(?://|$|\{))");
    static const std::regex re_field_inline(R"(^\s*((?:(?:public|private|protected|internal|static|readonly|new|volatile)\s+)+).+;\s*//\s*0x([0-9A-Fa-f]+)\s*$)");
    static const std::regex re_method_meta(R"(^\s*//\s*RVA:\s*0x([0-9A-Fa-f]+)\s+Offset:\s*0x([0-9A-Fa-f]+))");
    static const std::regex re_method_decl(
        R"(^\s*((?:(?:public|private|protected|internal|static|virtual|override|abstract|new|extern)\s+)*)([\w\.<>\[\], \*]+?)\s+(\w+)\s*\(([^)]*)\)\s*[{;])");

    std::vector<ClassEntry>   classes;
    std::optional<ClassEntry> current;

    std::string pending_ns;
    bool        next_is_method = false;
    uint64_t    pending_rva = 0;
    uint64_t    pending_moff = 0;

    auto flush = [&] {
        if (current && (!current->fields.empty() || !current->methods.empty()))
            classes.push_back(std::move(*current));
        current.reset();
        };

    std::string line;
    while (std::getline(file, line)) {
        std::smatch m;

        if (std::regex_search(line, m, re_namespace)) {
            pending_ns = trim(m[1].str());
            next_is_method = false;
            continue;
        }

        if (std::regex_search(line, m, re_class_decl)) {
            flush();
            ClassEntry cls;
            cls.ns = pending_ns;
            cls.name = m[1].str();
            cls.parent = trim(m[2].str());

            auto strip_generic = [](std::string& s) {
                auto p = s.find('<');
                if (p != std::string::npos) s = s.substr(0, p);
                };
            strip_generic(cls.name);
            strip_generic(cls.parent);

            current = std::move(cls);
            next_is_method = false;
            continue;
        }

        if (!current) continue;

        // Field
        {
            std::smatch fi_m;
            if (std::regex_search(line, fi_m, re_field_inline)) {
                std::string rest = line.substr(fi_m[1].length());
                auto semi = rest.find(';');
                if (semi != std::string::npos) {
                    rest = rest.substr(0, semi);
                    auto e = rest.find_last_not_of(" \t");
                    if (e != std::string::npos) rest = rest.substr(0, e + 1);
                    auto sp = rest.find_last_of(" \t");
                    if (sp != std::string::npos) {
                        FieldEntry f;
                        f.name = rest.substr(sp + 1);
                        f.type = trim(rest.substr(0, sp));
                        f.offset = static_cast<uint32_t>(std::stoul(fi_m[2].str(), nullptr, 16));
                        f.is_static = fi_m[1].str().find("static") != std::string::npos;
                        current->fields.push_back(std::move(f));
                    }
                }
                next_is_method = false;
                continue;
            }
        }

        // Method RVA/offset comment
        if (std::regex_search(line, m, re_method_meta)) {
            next_is_method = true;
            pending_rva = std::stoull(m[1].str(), nullptr, 16);
            pending_moff = std::stoull(m[2].str(), nullptr, 16);
            continue;
        }

        // Method declaration (line following RVA comment)
        if (next_is_method) {
            next_is_method = false;
            if (std::regex_search(line, m, re_method_decl)) {
                MethodEntry me;
                me.return_type = trim(m[2].str());
                me.name = trim(m[3].str());
                me.params = trim(m[4].str());
                me.rva = pending_rva;
                me.offset = pending_moff;
                current->methods.push_back(std::move(me));
            }
            continue;
        }
    }
    flush();
    return classes;
}

// Shared helper: build a safe namespace name for a class
// Returns empty string if the class should be skipped entirely.

static std::string class_ns_ident(const ClassEntry& cls) {
    std::string full = cls.full_name();
    std::string ns = safe_ident(full);

    // After safe_ident, an empty or pure-underscore result means the original
    // name was something like "<PrivateImplementationDetails>" that collapsed
    // entirely into underscores — give it a hash-based name instead.
    bool all_under = std::all_of(ns.begin(), ns.end(), [](char c) { return c == '_'; });
    if (ns.empty() || all_under) {
        size_t h = std::hash<std::string>{}(full);
        ns = std::format("private_impl_{:08x}", static_cast<uint32_t>(h));
    }
    return ns;
}

// Writers

void write_hpp(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format(
        "// Generated using https://github.com/Romuilesik/Airport-Security-Sucks-Offsets-Dumper\n"
        "// {}\n\n#pragma once\n#include <cstdint>\n\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;

        std::string ns = class_ns_ident(cls);
        f << std::format("namespace {} {{\n", ns);

        if (!cls.parent.empty())
            f << std::format("    // parent: {}\n", cls.parent);

        if (!cls.fields.empty()) {
            f << "    // Fields\n";
            std::unordered_set<std::string> used_idents;
            for (auto& fld : cls.fields) {
                std::string base = safe_ident(fld.name) + (fld.is_static ? "_static" : "");
                std::string ident = unique_ident(base, used_idents);
                std::string obf = is_obfuscated(fld.name)
                    ? std::format(" [obf: {}]", fld.name) : "";
                f << std::format("    constexpr std::ptrdiff_t {} = {:#x}; // {}{}\n",
                    ident, fld.offset, fld.type, obf);
            }
        }

        if (!cls.methods.empty()) {
            f << "    // Methods\n";
            for (auto& me : cls.methods) {
                std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
                f << std::format("    // [RVA {:#010x}] {}({}){}\n",
                    me.rva, me.name, me.params, ret);
            }
        }

        f << "}\n\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

void write_json(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);

    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else r += c;
        }
        return r;
        };

    f << "{\n";
    f << "  \"generator\": \"https://github.com/Romuilesik/Airport-Security-Sucks-Offsets-Dumper\",\n";
    f << std::format("  \"generated_at\": \"{}\",\n", ts);
    f << "  \"classes\": [\n";

    bool first_cls = true;
    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        if (!first_cls) f << ",\n";
        first_cls = false;

        f << "    {\n";
        f << std::format("      \"namespace\": \"{}\",\n", esc(cls.ns));
        f << std::format("      \"name\": \"{}\",\n", esc(cls.name));
        f << std::format("      \"full_name\": \"{}\",\n", esc(cls.full_name()));
        f << std::format("      \"parent\": \"{}\",\n", esc(cls.parent));
        f << "      \"fields\": [\n";
        for (size_t i = 0; i < cls.fields.size(); ++i) {
            auto& fld = cls.fields[i];
            f << std::format(
                "        {{ \"name\": \"{}\", \"type\": \"{}\","
                " \"offset\": {}, \"offset_hex\": \"{:#x}\", \"static\": {} }}",
                esc(fld.name), esc(fld.type),
                fld.offset, fld.offset,
                fld.is_static ? "true" : "false");
            if (i + 1 < cls.fields.size()) f << ",";
            f << "\n";
        }
        f << "      ],\n";
        f << "      \"methods\": [\n";
        for (size_t i = 0; i < cls.methods.size(); ++i) {
            auto& me = cls.methods[i];
            f << std::format(
                "        {{ \"name\": \"{}\", \"return_type\": \"{}\", \"params\": \"{}\","
                " \"rva\": {}, \"rva_hex\": \"{:#x}\", \"offset\": {}, \"offset_hex\": \"{:#x}\" }}",
                esc(me.name), esc(me.return_type), esc(me.params),
                me.rva, me.rva, me.offset, me.offset);
            if (i + 1 < cls.methods.size()) f << ",";
            f << "\n";
        }
        f << "      ]\n";
        f << "    }";
    }
    f << "\n  ]\n}\n";
    std::cout << std::format("[+] {}\n", out.string());
}

void write_txt(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format(
        "// Generated using https://github.com/Romuilesik/Airport-Security-Sucks-Offsets-Dumper\n"
        "// {}\n\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string header = cls.full_name();
        if (!cls.parent.empty()) header += " : " + cls.parent;
        f << std::format("Class: {}\n", header);

        for (auto& fld : cls.fields) {
            std::string tag = fld.is_static ? "[static] " : "";
            f << std::format("  [{:#06x}] {}{} : {}\n",
                fld.offset, tag, fld.name, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("  [RVA {:#010x}] {}({}){}\n",
                me.rva, me.name, me.params, ret);
        }
        f << "\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

void write_cs(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format(
        "// Generated using https://github.com/Romuilesik/Airport-Security-Sucks-Offsets-Dumper\n"
        "// {}\n\nnamespace AirportSecurityOffsets\n{{\n", ts);

    // C# reserved keywords — prefix with @ if needed (we use f_ for simplicity)
    static const std::unordered_set<std::string> cs_reserved = {
        "abstract","as","base","bool","break","byte","case","catch","char",
        "checked","class","const","continue","decimal","default","delegate",
        "do","double","else","enum","event","explicit","extern","false",
        "finally","fixed","float","for","foreach","goto","if","implicit",
        "in","int","interface","internal","is","lock","long","namespace",
        "new","null","object","operator","out","override","params","private",
        "protected","public","readonly","ref","return","sbyte","sealed",
        "short","sizeof","stackalloc","static","string","struct","switch",
        "this","throw","true","try","typeof","uint","ulong","unchecked",
        "unsafe","ushort","using","virtual","void","volatile","while",
    };

    auto cs_ident = [&](std::string_view s) -> std::string {
        std::string id = safe_ident(s);
        if (cs_reserved.count(id)) id = "f_" + id;
        return id;
        };

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string ident = cs_ident(cls.full_name());
        f << std::format("    public static class {}\n    {{\n", ident);

        if (!cls.parent.empty())
            f << std::format("        // parent: {}\n", cls.parent);

        std::unordered_set<std::string> used_idents;
        for (auto& fld : cls.fields) {
            std::string base = cs_ident(fld.name) + (fld.is_static ? "_Static" : "");
            std::string fname = unique_ident(base, used_idents);
            f << std::format("        public const int {} = {:#x}; // {}\n",
                fname, fld.offset, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("        // [RVA {:#x}] {}({}){}\n",
                me.rva, me.name, me.params, ret);
        }
        f << "    }\n\n";
    }
    f << "}\n";
    std::cout << std::format("[+] {}\n", out.string());
}

void write_rs(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format(
        "// Generated using https://github.com/Romuilesik/Airport-Security-Sucks-Offsets-Dumper\n"
        "// {}\n\n", ts);

    // Rust reserved keywords
    static const std::unordered_set<std::string> rs_reserved = {
        "as","async","await","break","const","continue","crate","dyn",
        "else","enum","extern","false","fn","for","if","impl","in",
        "let","loop","match","mod","move","mut","pub","ref","return",
        "self","Self","static","struct","super","trait","true","type",
        "union","unsafe","use","where","while","abstract","become","box",
        "do","final","macro","override","priv","try","typeof","unsized",
        "virtual","yield",
    };

    auto rs_mod_ident = [&](const std::string& full) -> std::string {
        std::string id = safe_ident(full);
        // lowercase for mod names
        std::ranges::transform(id, id.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (rs_reserved.count(id)) id = "m_" + id;
        return id;
        };

    auto rs_const_ident = [&](std::string_view name, bool is_static) -> std::string {
        std::string id = safe_ident(name, /*upper=*/true) + (is_static ? "_STATIC" : "");
        if (rs_reserved.count(id)) id = "F_" + id;
        return id;
        };

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;

        std::string mod = rs_mod_ident(cls.full_name());
        f << "#[allow(dead_code, non_upper_case_globals)]\n";
        f << std::format("pub mod {} {{\n", mod);

        if (!cls.parent.empty())
            f << std::format("    // parent: {}\n", cls.parent);

        std::unordered_set<std::string> used_idents;
        for (auto& fld : cls.fields) {
            std::string base = rs_const_ident(fld.name, fld.is_static);
            std::string fname = unique_ident(base, used_idents);
            f << std::format("    pub const {}: usize = {:#x}; // {}\n",
                fname, fld.offset, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("    // [RVA {:#x}] {}({}){}\n",
                me.rva, me.name, me.params, ret);
        }
        f << "}\n\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

// Entry point

int main(int argc, char* argv[]) {
    fs::path input_dir = ".";
    fs::path output_dir = "./offsets";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if ((arg == "--input" || arg == "-i") && i + 1 < argc)
            input_dir = argv[++i];
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc)
            output_dir = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: parse [--input <dir>] [--output <dir>]\n"
                "  --input  / -i   folder with dump.cs (default: .)\n"
                "  --output / -o   output folder       (default: ./offsets)\n";
            return 0;
        }
    }

    fs::path dump_cs = input_dir / "dump.cs";
    if (!fs::exists(dump_cs)) {
        std::cerr << std::format("[-] dump.cs not found in: {}\n", input_dir.string());
        std::cerr << "    Run Il2CppDumper first: https://github.com/Perfare/Il2CppDumper\n";
        return 1;
    }

    std::cout << std::format("[*] Parsing {} ...\n", dump_cs.string());
    auto raw_classes = parse_dump_cs(dump_cs);

    if (raw_classes.empty()) {
        std::cerr << "[-] No classes parsed. Check that dump.cs is valid Il2CppDumper output.\n";
        return 1;
    }

    // Merge duplicate namespaces and deduplicate fields
    auto classes = merge_classes(std::move(raw_classes));

    size_t total_fields = 0;
    size_t total_methods = 0;
    for (auto& c : classes) {
        total_fields += c.fields.size();
        total_methods += c.methods.size();
    }
    std::cout << std::format("[*] Found {} classes, {} fields, {} methods\n",
        classes.size(), total_fields, total_methods);

    fs::create_directories(output_dir);
    std::string ts = utc_timestamp();

    write_hpp(classes, output_dir / "ass_offsets.hpp", ts);
    write_json(classes, output_dir / "ass_offsets.json", ts);
    write_txt(classes, output_dir / "ass_dump.txt", ts);
    write_cs(classes, output_dir / "assOffsets.cs", ts);
    write_rs(classes, output_dir / "ass_offsets.rs", ts);

    std::cout << std::format("\n[+] Done. Output: {}\n", fs::absolute(output_dir).string());
    return 0;
}