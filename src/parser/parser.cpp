#include "parser.hpp"
#include <tree_sitter/api.h>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <climits>
#include <blake3.h>

// Grammar declarations (C linkage)
extern "C" {
    TSLanguage* tree_sitter_typescript();
    TSLanguage* tree_sitter_javascript();
    TSLanguage* tree_sitter_python();
    TSLanguage* tree_sitter_rust();
    TSLanguage* tree_sitter_go();
    TSLanguage* tree_sitter_c_sharp();
    TSLanguage* tree_sitter_php();
    TSLanguage* tree_sitter_dart();
    TSLanguage* tree_sitter_java();
    TSLanguage* tree_sitter_bash();
    TSLanguage* tree_sitter_cpp();
    TSLanguage* tree_sitter_kotlin();
    TSLanguage* tree_sitter_vue();
}

namespace axon {

std::optional<Language> language_from_extension(const std::string& ext) {
    if (ext == "ts" || ext == "tsx") return Language::TypeScript;
    if (ext == "js" || ext == "jsx" || ext == "mjs" || ext == "cjs") return Language::JavaScript;
    if (ext == "py") return Language::Python;
    if (ext == "rs") return Language::Rust;
    if (ext == "go") return Language::Go;
    if (ext == "cs") return Language::CSharp;
    if (ext == "php") return Language::PHP;
    if (ext == "dart") return Language::Dart;
    if (ext == "java") return Language::Java;
    if (ext == "sh" || ext == "bash") return Language::Bash;
    if (ext == "cpp" || ext == "cxx" || ext == "cc" || ext == "hpp" || ext == "hxx" || ext == "h") return Language::Cpp;
    if (ext == "kt" || ext == "kts") return Language::Kotlin;
    if (ext == "vue") return Language::Vue;
    return std::nullopt;
}

std::string language_name(Language lang) {
    switch (lang) {
        case Language::TypeScript:  return "typescript";
        case Language::JavaScript:  return "javascript";
        case Language::Python:      return "python";
        case Language::Rust:        return "rust";
        case Language::Go:          return "go";
        case Language::CSharp:      return "csharp";
        case Language::PHP:         return "php";
        case Language::Dart:        return "dart";
        case Language::Java:        return "java";
        case Language::Bash:        return "bash";
        case Language::Cpp:         return "cpp";
        case Language::Kotlin:      return "kotlin";
        case Language::Vue:         return "vue";
    }
    return "unknown";
}

static std::string compute_blake3(const std::string& content) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, content.data(), content.size());
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, out, BLAKE3_OUT_LEN);
    char hex[BLAKE3_OUT_LEN * 2 + 1];
    for (size_t i = 0; i < BLAKE3_OUT_LEN; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    return std::string(hex, BLAKE3_OUT_LEN * 2);
}

static TSLanguage* get_ts_language(Language lang) {
    switch (lang) {
        case Language::TypeScript:  return tree_sitter_typescript();
        case Language::JavaScript:  return tree_sitter_javascript();
        case Language::Python:      return tree_sitter_python();
        case Language::Rust:        return tree_sitter_rust();
        case Language::Go:          return tree_sitter_go();
        case Language::CSharp:      return tree_sitter_c_sharp();
        case Language::PHP:         return tree_sitter_php();
        case Language::Dart:        return tree_sitter_dart();
        case Language::Java:        return tree_sitter_java();
        case Language::Bash:        return tree_sitter_bash();
        case Language::Cpp:         return tree_sitter_cpp();
        case Language::Kotlin:      return tree_sitter_kotlin();
        case Language::Vue:         return tree_sitter_vue();
    }
    return nullptr;
}

// Extract text of a node from source
static std::string node_text(TSNode node, const std::string& src) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (start >= src.size() || end > src.size() || end <= start) return "";
    return src.substr(start, end - start);
}

// Get first line of node text (for signature)
static std::string first_line(TSNode node, const std::string& src) {
    auto text = node_text(node, src);
    auto pos = text.find('\n');
    return pos == std::string::npos ? text : text.substr(0, pos);
}

static bool is_doc_kind(const std::string& kind) {
    return kind == "comment" || kind == "block_comment" ||
           kind == "line_comment" || kind == "string" ||
           kind == "expression_statement";  // Python docstring wrapped in expr
}

// Try to extract docstring/leading comment before a node
// Searches up to 3 preceding siblings and concatenates contiguous comments.
static std::optional<std::string> extract_docstring(
    TSNode node, const std::string& src, TSNode root)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) return std::nullopt;

    uint32_t child_count = ts_node_child_count(parent);
    // Find index of this node among siblings
    uint32_t node_idx = child_count;
    for (uint32_t i = 0; i < child_count; i++) {
        if (ts_node_eq(ts_node_child(parent, i), node)) { node_idx = i; break; }
    }
    if (node_idx == 0) return std::nullopt;

    // Collect up to 3 preceding doc siblings (contiguous comments)
    std::vector<std::string> parts;
    for (int back = 1; back <= 3 && (int)node_idx - back >= 0; back++) {
        TSNode prev = ts_node_child(parent, node_idx - back);
        std::string kind = ts_node_type(prev);
        if (!is_doc_kind(kind)) break;

        std::string text = node_text(prev, src);
        // For expression_statement, only keep if it looks like a string literal (Python docstring)
        if (kind == "expression_statement") {
            if (text.empty() || (text[0] != '"' && text[0] != '\'')) break;
        }
        parts.insert(parts.begin(), text);
    }

    if (parts.empty()) return std::nullopt;
    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) result += '\n';
        result += parts[i];
    }
    return result;
}

struct ParseContext {
    const std::string& src;
    std::vector<Symbol>& symbols;
    std::vector<ImportEdge>& imports;
    std::vector<CallSite>& calls;
    Language lang;
};

// Cross-language list of AST node kinds that represent function/method calls.
static bool is_call_kind(const std::string& kind) {
    static const std::unordered_set<std::string> kinds = {
        "call_expression",          // TS/JS/Rust/Go/C++/Kotlin/Dart
        "call",                     // Python
        "invocation_expression",    // C#
        "method_invocation",        // Java
        "function_call_expression", // PHP
        "member_call_expression",   // PHP
        "scoped_call_expression",   // PHP
        "macro_invocation",         // Rust
    };
    return kinds.count(kind) > 0;
}

// Extract the called identifier from a call-kind node.
// Walks the first chain of children looking for the leaf identifier.
// Examples:
//   foo(...)            → "foo"
//   obj.method(...)     → "method"
//   ns::path::fn(...)   → "fn"
//   self.foo(...)       → "foo"
static std::string extract_callee_name(TSNode call_node, const std::string& src) {
    // Try field "function" first (TS/JS/Rust/Go/C++/etc)
    TSNode fn = ts_node_child_by_field_name(call_node, "function", 8);
    if (ts_node_is_null(fn)) {
        // Fallback: first named child
        uint32_t n = ts_node_named_child_count(call_node);
        if (n == 0) return "";
        fn = ts_node_named_child(call_node, 0);
    }
    if (ts_node_is_null(fn)) return "";

    // Walk down picking the rightmost identifier-like leaf.
    // Handles member_expression, scoped_identifier, field_expression, etc.
    while (!ts_node_is_null(fn)) {
        std::string k = ts_node_type(fn);
        if (k == "identifier" || k == "field_identifier" || k == "property_identifier" ||
            k == "simple_identifier" || k == "shorthand_property_identifier") {
            return node_text(fn, src);
        }
        // Common wrappers that hide the name:
        //   member_expression: object.PROPERTY  → "property" field
        //   field_expression : object.FIELD     → "field"   field
        //   scoped_identifier: ns::NAME         → "name"    field
        TSNode prop = ts_node_child_by_field_name(fn, "property", 8);
        if (ts_node_is_null(prop)) prop = ts_node_child_by_field_name(fn, "field", 5);
        if (ts_node_is_null(prop)) prop = ts_node_child_by_field_name(fn, "name", 4);
        if (!ts_node_is_null(prop)) { fn = prop; continue; }

        // Last resort: pick last named child (closest to leaf)
        uint32_t n = ts_node_named_child_count(fn);
        if (n == 0) return "";
        fn = ts_node_named_child(fn, n - 1);
    }
    return "";
}

// Common identifiers that look like calls but are control flow / built-ins.
// Filtering these prevents noise in the call graph.
static bool is_callee_noise(const std::string& name) {
    static const std::unordered_set<std::string> stop = {
        // C/C++ keywords disguised as calls
        "if","while","for","switch","return","sizeof","throw","catch",
        // Common built-ins / generic placeholders
        "assert","print","println","printf","fprintf","cout","cerr","cin",
        "log","trace","debug","info","warn","error","panic",
        // Pointer-like in C/C++
        "static_cast","dynamic_cast","reinterpret_cast","const_cast",
        // JS/Python noise
        "Object","Array","String","Number","Boolean","JSON","Math",
        "len","str","int","float","bool","list","dict","set","tuple",
        // Common noise
        "main","new","delete",
    };
    if (name.size() < 2) return true;
    return stop.count(name) > 0;
}

// Extract the real module / path specifier from an import node.
// Walks children DFS looking for the first path-like or string-literal descendant,
// so the caller can hand a raw import node (e.g. `import_from_statement`,
// `use_declaration`) without needing language-specific field names.
// Returns the cleaned specifier (quotes stripped, no surrounding whitespace).
static std::string extract_import_specifier(TSNode node, const std::string& src) {
    std::string kind = ts_node_type(node);

    // String literal kinds — strip surrounding quotes
    if (kind == "string_literal" || kind == "interpreted_string_literal" ||
        kind == "raw_string_literal" || kind == "string") {
        auto s = node_text(node, src);
        // Peel quote/backtick pairs (handles "foo", 'foo', `foo`)
        while (s.size() >= 2 &&
               (s.front() == '"' || s.front() == '\'' || s.front() == '`') &&
               s.front() == s.back()) {
            s = s.substr(1, s.size() - 2);
        }
        return s;
    }

    // Path-like identifiers used across tree-sitter grammars
    static const std::unordered_set<std::string> path_kinds = {
        "dotted_name",             // Python
        "relative_import",         // Python (from . import ...)
        "scoped_identifier",       // Rust, Java
        "scoped_use_list",         // Rust (use foo::{a,b})
        "use_list",                // Rust fallback
        "qualified_name",          // C#, PHP
        "namespace_name",          // PHP
        "qualified_identifier",    // C++, Kotlin
        "identifier",              // generic fallback
        "simple_identifier",       // Kotlin
        "package_identifier",      // Go
    };
    if (path_kinds.count(kind)) {
        return node_text(node, src);
    }

    // Recurse into children
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        std::string nested = extract_import_specifier(ts_node_child(node, i), src);
        if (!nested.empty()) return nested;
    }
    return "";
}

// Helper for languages where we have a raw import node and want a single edge.
static inline void push_import_edge(TSNode node, const std::string& src,
                                    std::vector<ImportEdge>& imports) {
    auto spec = extract_import_specifier(node, src);
    if (spec.empty()) return;
    // Trim leading/trailing whitespace
    size_t a = spec.find_first_not_of(" \t\n\r");
    size_t b = spec.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return;
    spec = spec.substr(a, b - a + 1);
    if (spec.empty()) return;
    imports.push_back({"", spec, "imports"});
}

static void visit_node(TSNode node, ParseContext& ctx, int depth = 0) {
    if (ts_node_is_null(node)) return;

    std::string kind = ts_node_type(node);
    Symbol sym;
    bool is_symbol = false;

    // TypeScript / JavaScript
    if (ctx.lang == Language::TypeScript || ctx.lang == Language::JavaScript) {
        if (kind == "function_declaration" || kind == "generator_function_declaration") {
            sym.kind = "function";
            // name = first named child "identifier"
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_definition") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "interface_declaration") {
            sym.kind = "interface";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "type_alias_declaration") {
            sym.kind = "type";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_statement" || kind == "import_declaration") {
            // Extract import specifier
            TSNode src_node = ts_node_child_by_field_name(node, "source", 6);
            if (!ts_node_is_null(src_node)) {
                auto spec = node_text(src_node, ctx.src);
                // Strip quotes
                if (spec.size() >= 2 && (spec.front() == '"' || spec.front() == '\''))
                    spec = spec.substr(1, spec.size() - 2);
                ctx.imports.push_back({"", spec, "imports"});
            }
        }
    }

    // Python
    if (ctx.lang == Language::Python) {
        if (kind == "function_definition") {
            sym.kind = "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_definition") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_statement" || kind == "import_from_statement") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Rust
    if (ctx.lang == Language::Rust) {
        if (kind == "function_item") {
            sym.kind = "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "struct_item") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "impl_item") {
            sym.kind = "class";
            TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
            if (!ts_node_is_null(type_node)) sym.name = "impl " + node_text(type_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "use_declaration") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // C#
    if (ctx.lang == Language::CSharp) {
        if (kind == "method_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "interface_declaration") {
            sym.kind = "interface";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "constructor_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "using_directive") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // PHP
    if (ctx.lang == Language::PHP) {
        if (kind == "function_definition") {
            sym.kind = "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "namespace_use_clause") {
            // Only capture the leaf clause to avoid duplicate edges from the
            // enclosing namespace_use_declaration (tree walk visits both).
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Dart
    if (ctx.lang == Language::Dart) {
        if (kind == "function_signature" || kind == "function_declaration") {
            sym.kind = "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_definition") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_signature" || kind == "function_body") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_or_export") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Java
    if (ctx.lang == Language::Java) {
        if (kind == "method_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "interface_declaration") {
            sym.kind = "interface";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "constructor_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_declaration") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Vue SFC — sub-parse <script> block with TS or JS parser
    if (ctx.lang == Language::Vue) {
        if (kind == "script_element") {
            bool is_typescript = false;
            uint32_t raw_start = 0, raw_end = 0;
            uint32_t script_start_row = 0;
            bool found_raw = false;
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ck = ts_node_type(c);
                if (ck == "start_tag") {
                    uint32_t acc = ts_node_child_count(c);
                    for (uint32_t j = 0; j < acc; j++) {
                        TSNode a = ts_node_child(c, j);
                        if (std::string(ts_node_type(a)) == "attribute") {
                            std::string atxt = node_text(a, ctx.src);
                            if (atxt.find("lang=\"ts\"") != std::string::npos ||
                                atxt.find("lang='ts'") != std::string::npos ||
                                atxt.find("lang=\"typescript\"") != std::string::npos) {
                                is_typescript = true;
                            }
                        }
                    }
                } else if (ck == "raw_text" || ck == "text") {
                    raw_start = ts_node_start_byte(c);
                    raw_end   = ts_node_end_byte(c);
                    script_start_row = ts_node_start_point(c).row;
                    found_raw = true;
                }
            }
            if (!found_raw || raw_end <= raw_start) return;

            std::string sub_src = ctx.src.substr(raw_start, raw_end - raw_start);
            TSParser* sub_parser = ts_parser_new();
            ts_parser_set_language(sub_parser,
                is_typescript ? tree_sitter_typescript() : tree_sitter_javascript());
            TSTree* sub_tree = ts_parser_parse_string(sub_parser, nullptr,
                sub_src.c_str(), (uint32_t)sub_src.size());
            TSNode sub_root = ts_tree_root_node(sub_tree);

            std::vector<Symbol> sub_syms;
            std::vector<ImportEdge> sub_imports;
            std::vector<CallSite> sub_calls;
            ParseContext sub_ctx{sub_src, sub_syms, sub_imports, sub_calls,
                                 is_typescript ? Language::TypeScript : Language::JavaScript};
            visit_node(sub_root, sub_ctx);

            for (auto& s : sub_syms) {
                s.start_line += (int)script_start_row;
                s.end_line   += (int)script_start_row;
                s.start_byte += (int)raw_start;
                s.end_byte   += (int)raw_start;
                ctx.symbols.push_back(std::move(s));
            }
            for (auto& e : sub_imports) {
                ctx.imports.push_back(std::move(e));
            }
            for (auto& c : sub_calls) {
                c.line += (int)script_start_row;
                ctx.calls.push_back(std::move(c));
            }

            ts_tree_delete(sub_tree);
            ts_parser_delete(sub_parser);
            return;
        }
        // template_element, style_element and other Vue nodes: recurse normally
    }

    // Go
    if (ctx.lang == Language::Go) {
        if (kind == "function_declaration" || kind == "method_declaration") {
            sym.kind = kind == "method_declaration" ? "method" : "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "type_declaration") {
            sym.kind = "type";
            sym.name = first_line(node, ctx.src).substr(0, 40);
            sym.signature = first_line(node, ctx.src);
            is_symbol = true;
        } else if (kind == "import_spec") {
            // Capture per-spec; the surrounding import_declaration is walked
            // and enters each spec as a child — so only the leaf is needed.
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Bash
    if (ctx.lang == Language::Bash) {
        if (kind == "function_definition") {
            sym.kind = "function";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "command") {
            uint32_t cc = ts_node_child_count(node);
            if (cc >= 2) {
                TSNode cmd = ts_node_child(node, 0);
                std::string cmd_text = node_text(cmd, ctx.src);
                if (cmd_text == "source" || cmd_text == ".") {
                    TSNode arg = ts_node_child(node, 1);
                    auto spec = node_text(arg, ctx.src);
                    if (spec.size() >= 2 && (spec.front() == '"' || spec.front() == '\''))
                        spec = spec.substr(1, spec.size() - 2);
                    ctx.imports.push_back({"", spec, "imports"});
                }
            }
        }
    }

    // C++
    if (ctx.lang == Language::Cpp) {
        if (kind == "function_definition") {
            sym.kind = "function";
            TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
            while (!ts_node_is_null(decl)) {
                std::string dk = ts_node_type(decl);
                if (dk == "identifier" || dk == "field_identifier" || dk == "qualified_identifier") {
                    sym.name = node_text(decl, ctx.src);
                    break;
                }
                TSNode inner = ts_node_child_by_field_name(decl, "declarator", 10);
                if (ts_node_is_null(inner)) break;
                decl = inner;
            }
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_specifier" || kind == "struct_specifier") {
            sym.kind = (kind == "class_specifier") ? "class" : "struct";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "namespace_definition") {
            sym.kind = "namespace";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "preproc_include") {
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ck = ts_node_type(c);
                if (ck == "system_lib_string" || ck == "string_literal") {
                    auto spec = node_text(c, ctx.src);
                    if (spec.size() >= 2) spec = spec.substr(1, spec.size() - 2);
                    ctx.imports.push_back({"", spec, "imports"});
                    break;
                }
            }
        }
    }

    // Kotlin
    if (ctx.lang == Language::Kotlin) {
        if (kind == "function_declaration") {
            sym.kind = "function";
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                if (std::string(ts_node_type(c)) == "simple_identifier") {
                    sym.name = node_text(c, ctx.src);
                    break;
                }
            }
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration" || kind == "object_declaration") {
            sym.kind = "class";
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ct = ts_node_type(c);
                if (ct == "simple_identifier" || ct == "type_identifier") {
                    sym.name = node_text(c, ctx.src);
                    break;
                }
            }
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_header") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    if (is_symbol) {
        sym.start_line = (int)ts_node_start_point(node).row + 1;
        sym.end_line   = (int)ts_node_end_point(node).row + 1;
        sym.start_byte = (int)ts_node_start_byte(node);
        sym.end_byte   = (int)ts_node_end_byte(node);
        if (!sym.docstring) {
            // Extract leading comment/docstring (searches up to 3 siblings back)
            TSNode root_dummy = {}; // root param unused in new impl
            sym.docstring = extract_docstring(node, ctx.src, root_dummy);
        }
        ctx.symbols.push_back(std::move(sym));
    }

    // Collect call sites (only when the node is a call kind).
    // Caller resolution happens in a post-pass once all symbols are known.
    if (is_call_kind(kind)) {
        std::string callee = extract_callee_name(node, ctx.src);
        if (!callee.empty() && !is_callee_noise(callee)) {
            CallSite cs;
            cs.caller_name = "";  // resolved post-walk via byte range
            cs.callee_name = std::move(callee);
            cs.line        = (int)ts_node_start_point(node).row + 1;
            // Stash byte position in line slot's high bits — no, keep separate.
            // We need start_byte to find enclosing symbol; piggyback in caller_name temporarily
            // and resolve below. Simpler: record byte and resolve after visit.
            ctx.calls.push_back(std::move(cs));
            // Append byte position alongside in a parallel vector? Simpler:
            // we'll re-derive via line in post-pass against symbols' line ranges.
        }
    }

    // Recurse into children
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; i++) {
        visit_node(ts_node_child(node, i), ctx, depth + 1);
    }
}

std::optional<ParsedFile> parse_file(
    const std::filesystem::path& abs_path,
    const std::filesystem::path& project_root)
{
    auto ext = abs_path.extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);

    auto lang_opt = language_from_extension(ext);
    if (!lang_opt) return std::nullopt;
    Language lang = *lang_opt;

    std::ifstream file(abs_path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string src((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());

    // Compute hash
    std::string hash = compute_blake3(src);

    // Parse with tree-sitter
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, get_ts_language(lang));
    TSTree* tree = ts_parser_parse_string(parser, nullptr, src.c_str(), (uint32_t)src.size());
    TSNode root  = ts_tree_root_node(tree);

    ParsedFile result;
    result.path     = std::filesystem::relative(abs_path, project_root).string();
    result.language = lang;
    result.hash     = hash;

    ParseContext ctx{src, result.symbols, result.imports, result.calls, lang};
    visit_node(root, ctx);

    ts_tree_delete(tree);
    ts_parser_delete(parser);

    // Post-pass: resolve caller_name for each call site by finding the smallest
    // enclosing symbol whose [start_line, end_line] contains the call's line.
    // Calls outside any symbol (e.g. module-level) are dropped — they have no
    // function-level caller to attribute to.
    {
        std::vector<CallSite> resolved;
        resolved.reserve(result.calls.size());
        for (auto& c : result.calls) {
            const Symbol* best = nullptr;
            int best_span = INT_MAX;
            for (const auto& s : result.symbols) {
                if (c.line < s.start_line || c.line > s.end_line) continue;
                int span = s.end_line - s.start_line;
                if (span < best_span) { best_span = span; best = &s; }
            }
            if (!best) continue;                       // module-level call → drop
            if (best->name == c.callee_name) continue; // self-recursion → drop
            c.caller_name = best->name;
            resolved.push_back(std::move(c));
        }
        result.calls = std::move(resolved);
    }

    return result;
}

} // namespace axon
