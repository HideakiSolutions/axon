#include "parser.hpp"
#include <tree_sitter/api.h>
#include <cstdio>
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
    TSLanguage* tree_sitter_nix();
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
    if (ext == "nix") return Language::Nix;
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
        case Language::Nix:         return "nix";
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
        case Language::Nix:         return tree_sitter_nix();
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
        "apply_expression",         // Nix
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
        // TS decorators have two emission shapes depending on grammar version:
        //   1. Direct children of the declaration (plain `@Foo class X {}`)
        //   2. Siblings under `export_statement` for `@Foo export class X {}`
        // We walk both so capsule queries surface @Component/@Injectable/@Get
        // regardless of whether the class is exported.
        auto collect_decorators_ts = [&](TSNode def_node) -> std::optional<std::string> {
            std::string out;
            auto scan = [&](TSNode container) {
                uint32_t cc = ts_node_child_count(container);
                for (uint32_t i = 0; i < cc; i++) {
                    TSNode c = ts_node_child(container, i);
                    if (std::string(ts_node_type(c)) == "decorator") {
                        if (!out.empty()) out += '\n';
                        out += node_text(c, ctx.src);
                    }
                }
            };
            scan(def_node);
            TSNode parent = ts_node_parent(def_node);
            if (!ts_node_is_null(parent) &&
                std::string(ts_node_type(parent)) == "export_statement") {
                scan(parent);
            }
            return out.empty() ? std::nullopt : std::optional<std::string>(out);
        };

        if (kind == "function_declaration" || kind == "generator_function_declaration") {
            // Detect `async function` via first unnamed child.
            sym.kind = "function";
            if (ts_node_child_count(node) > 0) {
                std::string fc = ts_node_type(ts_node_child(node, 0));
                if (fc == "async") sym.kind = "async_function";
            }
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            sym.docstring = collect_decorators_ts(node);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_definition") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            sym.docstring = collect_decorators_ts(node);
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
        } else if (kind == "enum_declaration") {
            sym.kind = "enum";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "internal_module" || kind == "module") {
            // tree-sitter-typescript: `namespace Foo {}` -> internal_module;
            // `module "foo" {}` (ambient) -> module.
            sym.kind = "namespace";
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
        // When the parent is a `decorated_definition`, gather the @decorator lines
        // and stash them in docstring so capsule consumers see framework usage
        // (e.g. @router.get, @app.task, @dataclass) without re-parsing source.
        auto collect_decorators = [&](TSNode def_node) -> std::optional<std::string> {
            TSNode parent = ts_node_parent(def_node);
            if (ts_node_is_null(parent)) return std::nullopt;
            if (std::string(ts_node_type(parent)) != "decorated_definition") return std::nullopt;
            std::string out;
            uint32_t pcc = ts_node_child_count(parent);
            for (uint32_t i = 0; i < pcc; i++) {
                TSNode pc = ts_node_child(parent, i);
                if (std::string(ts_node_type(pc)) == "decorator") {
                    if (!out.empty()) out += '\n';
                    out += node_text(pc, ctx.src);
                }
            }
            return out.empty() ? std::nullopt : std::optional<std::string>(out);
        };

        if (kind == "function_definition") {
            // Detect `async def`: first unnamed child is the "async" keyword.
            sym.kind = "function";
            if (ts_node_child_count(node) > 0) {
                std::string fc = ts_node_type(ts_node_child(node, 0));
                if (fc == "async") sym.kind = "async_function";
            }
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            sym.docstring = collect_decorators(node);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_definition") {
            sym.kind = "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            sym.docstring = collect_decorators(node);
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
            sym.kind = "class";  // kept "class" for back-compat with existing edge queries
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "enum_item" || kind == "union_item") {
            sym.kind = (kind == "enum_item") ? "enum" : "union";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "trait_item") {
            sym.kind = "trait";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "impl_item") {
            // Distinguish `impl Trait for Type` (trait impl) from `impl Type` (inherent impl).
            // Without this, both collapsed onto kind="class" name="impl Type", losing the
            // trait↔implementor relationship in the symbol graph.
            TSNode trait_node = ts_node_child_by_field_name(node, "trait", 5);
            TSNode type_node  = ts_node_child_by_field_name(node, "type", 4);
            std::string type_name = ts_node_is_null(type_node) ? "" : node_text(type_node, ctx.src);
            sym.kind = "impl";
            if (!ts_node_is_null(trait_node)) {
                sym.name = node_text(trait_node, ctx.src) + " for " + type_name;
            } else {
                sym.name = "impl " + type_name;
            }
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "mod_item") {
            sym.kind = "module";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "macro_definition") {
            // `macro_rules! name { ... }` — the macro itself is a defined symbol.
            sym.kind = "macro";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "use_declaration") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // C#
    if (ctx.lang == Language::CSharp) {
        // C# attributes ([Serializable], [HttpGet("/users")]) are emitted as
        // `attribute_list` siblings of the declaration. They land in docstring
        // alongside detected modifier flags so capsule queries surface the
        // ASP.NET / Serializer wiring without re-grepping.
        auto collect_attrs_cs = [&](TSNode def_node, bool& out_async, bool& out_partial) {
            out_async = false;
            out_partial = false;
            std::string out;
            uint32_t cc = ts_node_child_count(def_node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(def_node, i);
                std::string ck = ts_node_type(c);
                if (ck == "attribute_list") {
                    if (!out.empty()) out += '\n';
                    out += node_text(c, ctx.src);
                } else if (ck == "modifier") {
                    std::string mt = node_text(c, ctx.src);
                    if (mt == "async") out_async = true;
                    else if (mt == "partial") out_partial = true;
                }
            }
            return out.empty() ? std::optional<std::string>{} : std::optional<std::string>(out);
        };

        if (kind == "method_declaration") {
            bool is_async = false, is_partial = false;
            sym.docstring = collect_attrs_cs(node, is_async, is_partial);
            sym.kind = is_async ? "async_method" : "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "property_declaration") {
            sym.kind = "property";
            bool dummy_a = false, dummy_p = false;
            sym.docstring = collect_attrs_cs(node, dummy_a, dummy_p);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            bool dummy_a = false, is_partial = false;
            sym.docstring = collect_attrs_cs(node, dummy_a, is_partial);
            sym.kind = is_partial ? "partial_class" : "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "interface_declaration") {
            sym.kind = "interface";
            bool dummy_a = false, dummy_p = false;
            sym.docstring = collect_attrs_cs(node, dummy_a, dummy_p);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "record_declaration") {
            // C# 9+ records — first-class for DTOs / immutable values.
            sym.kind = "record";
            bool dummy_a = false, dummy_p = false;
            sym.docstring = collect_attrs_cs(node, dummy_a, dummy_p);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "enum_declaration") {
            sym.kind = "enum";
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
        } else if (kind == "namespace_declaration") {
            sym.kind = "namespace";
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
        // PHP 8 introduced #[Attribute] syntax; tree-sitter-php emits these as
        // attribute_list children of declarations. Same shape as C# attributes.
        auto collect_attrs_php = [&](TSNode def_node) -> std::optional<std::string> {
            std::string out;
            uint32_t cc = ts_node_child_count(def_node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(def_node, i);
                if (std::string(ts_node_type(c)) == "attribute_list") {
                    if (!out.empty()) out += '\n';
                    out += node_text(c, ctx.src);
                }
            }
            return out.empty() ? std::nullopt : std::optional<std::string>(out);
        };

        if (kind == "function_definition") {
            sym.kind = "function";
            sym.docstring = collect_attrs_php(node);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_declaration") {
            sym.kind = "method";
            sym.docstring = collect_attrs_php(node);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            sym.kind = "class";
            sym.docstring = collect_attrs_php(node);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "trait_declaration") {
            sym.kind = "trait";
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
        } else if (kind == "enum_declaration") {
            sym.kind = "enum";
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
        } else if (kind == "namespace_use_clause") {
            // Only capture the leaf clause to avoid duplicate edges from the
            // enclosing namespace_use_declaration (tree walk visits both).
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Dart
    if (ctx.lang == Language::Dart) {
        // Detect `async` / `async*` body modifier on functions/methods. In
        // tree-sitter-dart the modifier token appears before the function_body
        // child, so we scan for an "async" or "async*" leaf in node's subtree
        // shallow-enough to be cheap (first 6 children).
        auto is_async_dart = [&](TSNode def_node) {
            uint32_t cc = ts_node_child_count(def_node);
            uint32_t scan = cc < 6 ? cc : 6;
            for (uint32_t i = 0; i < scan; i++) {
                std::string ck = ts_node_type(ts_node_child(def_node, i));
                if (ck == "async" || ck == "async_marker" || ck == "async*") return true;
            }
            return false;
        };

        if (kind == "function_signature" || kind == "function_declaration") {
            sym.kind = is_async_dart(node) ? "async_function" : "function";
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
        } else if (kind == "mixin_declaration") {
            sym.kind = "mixin";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "extension_declaration") {
            // `extension X on Type { ... }` — Dart's open-class mechanism.
            sym.kind = "extension";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "enum_declaration") {
            sym.kind = "enum";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "factory_constructor_signature" ||
                   kind == "constructor_signature") {
            // factory + named/redirecting constructors — both important for
            // capsule rendering of DI/factory patterns common in Flutter.
            sym.kind = (kind == "factory_constructor_signature") ? "factory" : "constructor";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "method_signature" || kind == "function_body") {
            sym.kind = is_async_dart(node) ? "async_method" : "method";
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
        // Walk a `modifiers` child of a declaration and pull out @-annotations
        // and the `sealed`/`non-sealed` keywords (Java 15+) for capsule rendering.
        auto collect_modifiers = [&](TSNode decl_node, bool& out_is_sealed) -> std::optional<std::string> {
            out_is_sealed = false;
            TSNode mods = ts_node_child_by_field_name(decl_node, "modifiers", 9);
            // Fallback: tree-sitter-java sometimes emits modifiers as the first
            // unnamed child of kind "modifiers" rather than via a field.
            if (ts_node_is_null(mods)) {
                uint32_t cc = ts_node_child_count(decl_node);
                for (uint32_t i = 0; i < cc; i++) {
                    TSNode c = ts_node_child(decl_node, i);
                    if (std::string(ts_node_type(c)) == "modifiers") { mods = c; break; }
                }
            }
            if (ts_node_is_null(mods)) return std::nullopt;
            std::string out;
            uint32_t cc = ts_node_child_count(mods);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(mods, i);
                std::string ck = ts_node_type(c);
                if (ck == "marker_annotation" || ck == "annotation" ||
                    ck == "single_element_annotation") {
                    if (!out.empty()) out += '\n';
                    out += node_text(c, ctx.src);
                } else if (ck == "sealed" || ck == "non-sealed") {
                    out_is_sealed = true;
                }
            }
            return out.empty() ? std::nullopt : std::optional<std::string>(out);
        };

        if (kind == "method_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            bool dummy = false;
            sym.docstring = collect_modifiers(node, dummy);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_declaration") {
            bool sealed = false;
            sym.docstring = collect_modifiers(node, sealed);
            sym.kind = sealed ? "sealed_class" : "class";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "interface_declaration") {
            bool sealed = false;
            sym.docstring = collect_modifiers(node, sealed);
            sym.kind = sealed ? "sealed_interface" : "interface";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "record_declaration") {
            // Java 14+ `record Point(int x, int y) { }` — first-class data carrier.
            sym.kind = "record";
            bool dummy = false;
            sym.docstring = collect_modifiers(node, dummy);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "enum_declaration") {
            sym.kind = "enum";
            bool dummy = false;
            sym.docstring = collect_modifiers(node, dummy);
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "annotation_type_declaration") {
            sym.kind = "annotation_type";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "constructor_declaration") {
            sym.kind = "method";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            bool dummy = false;
            sym.docstring = collect_modifiers(node, dummy);
            is_symbol = !sym.name.empty();
        } else if (kind == "import_declaration") {
            push_import_edge(node, ctx.src, ctx.imports);
        }
    }

    // Nix
    if (ctx.lang == Language::Nix) {
        // `foo.bar = expr;` and `foo = expr;` — emit attrpath as symbol. Kind
        // depends on RHS: `function_expression` → "function", `attrset_expression`
        // → "attrset", anything else → "binding". Lets capsule queries surface
        // option definitions, NixOS modules, and helper lambdas without dragging
        // in entire `let` blocks.
        if (kind == "binding") {
            TSNode attrpath_node = ts_node_child_by_field_name(node, "attrpath", 8);
            TSNode expr_node     = ts_node_child_by_field_name(node, "expression", 10);
            if (!ts_node_is_null(attrpath_node)) {
                sym.name = node_text(attrpath_node, ctx.src);
                sym.kind = "binding";
                if (!ts_node_is_null(expr_node)) {
                    std::string ek = ts_node_type(expr_node);
                    if (ek == "function_expression")          sym.kind = "function";
                    else if (ek == "attrset_expression" ||
                             ek == "rec_attrset_expression")  sym.kind = "attrset";
                }
                sym.signature = first_line(node, ctx.src);
                is_symbol = !sym.name.empty();
            }
        } else if (kind == "inherit" || kind == "inherit_from") {
            // `inherit a b c;` — re-binds each attr from the enclosing scope.
            // `inherit (src) a b;` — re-binds from `src`; also emits import edge.
            TSNode attrs = ts_node_child_by_field_name(node, "attrs", 5);
            if (!ts_node_is_null(attrs)) {
                uint32_t cc = ts_node_named_child_count(attrs);
                for (uint32_t i = 0; i < cc; i++) {
                    TSNode a = ts_node_named_child(attrs, i);
                    if (std::string(ts_node_type(a)) != "identifier") continue;
                    Symbol s;
                    s.kind = "variable";
                    s.name = node_text(a, ctx.src);
                    s.signature = first_line(node, ctx.src);
                    s.start_line = (int)ts_node_start_point(a).row + 1;
                    s.end_line   = (int)ts_node_end_point(a).row + 1;
                    s.start_byte = (int)ts_node_start_byte(a);
                    s.end_byte   = (int)ts_node_end_byte(a);
                    ctx.symbols.push_back(std::move(s));
                }
            }
            if (kind == "inherit_from") {
                TSNode src_expr = ts_node_child_by_field_name(node, "expression", 10);
                if (!ts_node_is_null(src_expr)) {
                    auto spec = node_text(src_expr, ctx.src);
                    if (!spec.empty()) ctx.imports.push_back({"", spec, "imports"});
                }
            }
        } else if (kind == "apply_expression") {
            // Detect `import <path>` / `import ./foo.nix` and record as import.
            // Anything else falls through to the generic call-site collector.
            TSNode fn = ts_node_child_by_field_name(node, "function", 8);
            if (!ts_node_is_null(fn)) {
                std::string fn_kind = ts_node_type(fn);
                std::string fn_text;
                if (fn_kind == "variable_expression" &&
                    ts_node_named_child_count(fn) > 0) {
                    fn_text = node_text(ts_node_named_child(fn, 0), ctx.src);
                }
                if (fn_text == "import") {
                    TSNode arg = ts_node_child_by_field_name(node, "argument", 8);
                    if (!ts_node_is_null(arg)) {
                        std::string ak = ts_node_type(arg);
                        if (ak == "path_expression" || ak == "spath_expression" ||
                            ak == "hpath_expression" || ak == "string_expression") {
                            auto spec = node_text(arg, ctx.src);
                            // Strip surrounding `<…>` or quotes
                            if (spec.size() >= 2) {
                                if (spec.front() == '<' && spec.back() == '>')
                                    spec = spec.substr(1, spec.size() - 2);
                                else if ((spec.front() == '"' || spec.front() == '\'') &&
                                         spec.front() == spec.back())
                                    spec = spec.substr(1, spec.size() - 2);
                            }
                            if (!spec.empty())
                                ctx.imports.push_back({"", spec, "imports"});
                        }
                    }
                }
            }
        }
    }

    // Vue SFC — sub-parse <script> block with TS or JS parser
    if (ctx.lang == Language::Vue) {
        if (kind == "script_element") {
            bool is_typescript = false;
            bool saw_lang_attr = false;
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
                            if (atxt.find("lang=") != std::string::npos) saw_lang_attr = true;
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
            // Vue 3 SFCs that omit `lang` default to JavaScript per the
            // single-file-component spec. We honor the spec but emit a
            // one-line stderr warning so users grepping for missing types
            // notice — the same TS code parsed as JS will silently lose
            // generic / decorator coverage.
            if (!saw_lang_attr) {
                static thread_local bool warned_once = false;
                if (!warned_once) {
                    std::fprintf(stderr,
                        "[warn] Vue SFC <script> without `lang` attribute — "
                        "parsing as JavaScript. Add `lang=\"ts\"` for TypeScript.\n");
                    warned_once = true;
                }
            }

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
            // Inspect the inner type_spec / type_alias to classify the kind.
            // v0.5.0 collapsed everything under kind="type" with a name derived
            // from first_line — interfaces and structs were indistinguishable.
            sym.kind = "type";
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode spec = ts_node_child(node, i);
                std::string sk = ts_node_type(spec);
                if (sk != "type_spec" && sk != "type_alias") continue;
                TSNode name_n = ts_node_child_by_field_name(spec, "name", 4);
                if (ts_node_is_null(name_n)) continue;
                sym.name = node_text(name_n, ctx.src);
                TSNode type_n = ts_node_child_by_field_name(spec, "type", 4);
                std::string tk = ts_node_is_null(type_n) ? "" : ts_node_type(type_n);
                if (tk == "interface_type") sym.kind = "interface";
                else if (tk == "struct_type") sym.kind = "struct";
                else if (sk == "type_alias") sym.kind = "type_alias";
                sym.signature = first_line(node, ctx.src);
                is_symbol = true;
                // Multi-spec blocks (`type ( A int; B string )`) only emit the
                // first spec — matches v0.5.0 behavior; precise multi-emit lands
                // when the visit_node emitter supports it.
                break;
            }
            if (!is_symbol) {
                sym.name = first_line(node, ctx.src).substr(0, 40);
                sym.signature = first_line(node, ctx.src);
                is_symbol = true;
            }
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
        } else if (kind == "declaration_command") {
            // `export FOO=bar`, `readonly X=1`, `declare -r BASE=...` — emit the
            // assigned name as a symbol so capsule queries about config knobs
            // can locate the export site without re-grepping. Heredocs and
            // bare-variable assignments without `export`/`readonly`/`declare`
            // are intentionally skipped (high noise, low semantic value).
            uint32_t cc = ts_node_child_count(node);
            std::string keyword;
            if (cc > 0) keyword = node_text(ts_node_child(node, 0), ctx.src);
            if (keyword == "export" || keyword == "readonly" ||
                keyword == "declare" || keyword == "typeset") {
                for (uint32_t i = 1; i < cc; i++) {
                    TSNode c = ts_node_child(node, i);
                    if (std::string(ts_node_type(c)) == "variable_assignment") {
                        TSNode nm = ts_node_child_by_field_name(c, "name", 4);
                        if (!ts_node_is_null(nm)) {
                            sym.kind = "variable";
                            sym.name = node_text(nm, ctx.src);
                            sym.signature = first_line(node, ctx.src);
                            is_symbol = true;
                            break;
                        }
                    }
                }
            }
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
        // template_declaration wraps a function_definition / class_specifier /
        // struct_specifier child. We let the inner declaration emit its symbol
        // normally (visit_node recurses) but capture the template parameters
        // line in docstring so capsule rendering keeps `template<typename T>`
        // visible alongside the templated entity. The lambda is only invoked
        // for inner nodes whose parent is a template_declaration.
        auto template_params_for = [&](TSNode inner) -> std::optional<std::string> {
            TSNode parent = ts_node_parent(inner);
            if (ts_node_is_null(parent)) return std::nullopt;
            if (std::string(ts_node_type(parent)) != "template_declaration") return std::nullopt;
            TSNode params = ts_node_child_by_field_name(parent, "parameters", 10);
            if (ts_node_is_null(params)) return std::nullopt;
            return node_text(params, ctx.src);
        };

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
            sym.docstring = template_params_for(node);
            is_symbol = !sym.name.empty();
        } else if (kind == "class_specifier" || kind == "struct_specifier") {
            sym.kind = (kind == "class_specifier") ? "class" : "struct";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            sym.docstring = template_params_for(node);
            is_symbol = !sym.name.empty();
        } else if (kind == "union_specifier") {
            sym.kind = "union";
            TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name_node)) sym.name = node_text(name_node, ctx.src);
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "enum_specifier") {
            sym.kind = "enum";
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
        } else if (kind == "friend_declaration") {
            // friend declarations cross encapsulation boundaries — capsule
            // queries about access control should surface them.
            sym.kind = "friend";
            sym.name = first_line(node, ctx.src).substr(0, 60);
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
        // Walk the modifiers child list (when present) for sealed/suspend/data
        // markers. tree-sitter-kotlin emits these as either modifier_list /
        // modifiers children or as scattered keyword tokens — we cover both.
        auto kotlin_flags = [&](TSNode def_node, bool& sealed, bool& data,
                                bool& suspend, bool& enum_class) {
            sealed = data = suspend = enum_class = false;
            uint32_t cc = ts_node_child_count(def_node);
            uint32_t scan = cc < 8 ? cc : 8;
            for (uint32_t i = 0; i < scan; i++) {
                TSNode c = ts_node_child(def_node, i);
                std::string ck = ts_node_type(c);
                if (ck == "modifier_list" || ck == "modifiers") {
                    uint32_t mc = ts_node_child_count(c);
                    for (uint32_t j = 0; j < mc; j++) {
                        std::string mk = node_text(ts_node_child(c, j), ctx.src);
                        if (mk == "sealed") sealed = true;
                        else if (mk == "data") data = true;
                        else if (mk == "suspend") suspend = true;
                        else if (mk == "enum") enum_class = true;
                    }
                } else {
                    std::string txt = node_text(c, ctx.src);
                    if (txt == "sealed") sealed = true;
                    else if (txt == "data") data = true;
                    else if (txt == "suspend") suspend = true;
                    else if (txt == "enum") enum_class = true;
                }
            }
        };

        if (kind == "function_declaration") {
            bool sealed = false, data = false, suspend = false, ec = false;
            kotlin_flags(node, sealed, data, suspend, ec);
            // Detect extension functions: a `receiver_type` (or
            // `user_type`/`type_reference`) appears before the function name.
            bool is_extension = false;
            uint32_t cc = ts_node_child_count(node);
            int seen_ident = -1;
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ct = ts_node_type(c);
                if (ct == "simple_identifier") { seen_ident = (int)i; break; }
                if (ct == "user_type" || ct == "receiver_type" || ct == "type_reference") {
                    is_extension = true;
                }
            }
            sym.kind = is_extension ? "extension_function"
                       : suspend     ? "suspend_function"
                                     : "function";
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                if (std::string(ts_node_type(c)) == "simple_identifier") {
                    sym.name = node_text(c, ctx.src);
                    break;
                }
            }
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
            (void)seen_ident;
        } else if (kind == "class_declaration") {
            bool sealed = false, data = false, suspend = false, enum_class = false;
            kotlin_flags(node, sealed, data, suspend, enum_class);
            if      (sealed)     sym.kind = "sealed_class";
            else if (data)       sym.kind = "data_class";
            else if (enum_class) sym.kind = "enum_class";
            else                 sym.kind = "class";
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
        } else if (kind == "object_declaration") {
            // Plain object vs companion object: companion objects appear
            // wrapped in a companion_object node OR carry a "companion"
            // modifier; both surface as kind="companion_object".
            bool is_companion = false;
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                std::string txt = node_text(ts_node_child(node, i), ctx.src);
                if (txt == "companion") { is_companion = true; break; }
            }
            sym.kind = is_companion ? "companion_object" : "object";
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ct = ts_node_type(c);
                if (ct == "simple_identifier" || ct == "type_identifier") {
                    sym.name = node_text(c, ctx.src);
                    break;
                }
            }
            if (sym.name.empty() && is_companion) sym.name = "Companion";
            sym.signature = first_line(node, ctx.src);
            is_symbol = !sym.name.empty();
        } else if (kind == "companion_object") {
            sym.kind = "companion_object";
            sym.name = "Companion";
            sym.signature = first_line(node, ctx.src);
            is_symbol = true;
        } else if (kind == "type_alias") {
            sym.kind = "type_alias";
            uint32_t cc = ts_node_child_count(node);
            for (uint32_t i = 0; i < cc; i++) {
                TSNode c = ts_node_child(node, i);
                std::string ct = ts_node_type(c);
                if (ct == "type_identifier" || ct == "simple_identifier") {
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
