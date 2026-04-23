#include "skeleton.hpp"
#include <tree_sitter/api.h>
#include <cstring>
#include <sstream>
#include <vector>

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

static std::string node_text(TSNode node, const std::string& src) {
    uint32_t s = ts_node_start_byte(node);
    uint32_t e = ts_node_end_byte(node);
    if (s >= src.size() || e > src.size() || e <= s) return "";
    return src.substr(s, e - s);
}

static std::string first_line(TSNode node, const std::string& src) {
    auto t = node_text(node, src);
    auto pos = t.find('\n');
    return (pos == std::string::npos ? t : t.substr(0, pos)) + " { ... }";
}

// Returns true if this node type represents a "body" to be skipped
static bool is_body_kind(const char* kind) {
    return (
        strcmp(kind, "statement_block") == 0 ||   // TS/JS function body
        strcmp(kind, "block") == 0               ||   // Python/Go body
        strcmp(kind, "block_body") == 0          ||
        strcmp(kind, "field_declaration_list") == 0 || // TS class body (keep for now)
        strcmp(kind, "declaration_list") == 0         // Rust impl body
    );
}

static bool is_comment_kind(const char* kind) {
    return (strcmp(kind, "comment") == 0 ||
            strcmp(kind, "line_comment") == 0 ||
            strcmp(kind, "block_comment") == 0);
}

// Recursively build skeleton, skipping function bodies
static void build_skeleton(TSNode node, const std::string& src,
                            Language lang, int depth, std::ostringstream& out,
                            bool strip_comments)
{
    if (ts_node_is_null(node)) return;
    std::string kind = ts_node_type(node);

    // For function/method nodes: emit only first line (signature)
    bool is_func = (kind == "function_declaration" ||
                    kind == "function_definition"  ||
                    kind == "function_item"        ||
                    kind == "method_definition"    ||
                    kind == "method_declaration"   ||
                    kind == "constructor_declaration" ||  // C#/Java
                    kind == "function_signature"   ||     // Dart
                    kind == "generator_function_declaration");

    if (is_func) {
        out << first_line(node, src) << "\n";
        return;
    }

    // For class/impl: emit header, recurse into members (methods get skeletonized)
    bool is_class = (kind == "class_declaration"  ||
                     kind == "class_definition"   ||
                     kind == "impl_item"          ||
                     kind == "struct_item"        ||
                     kind == "interface_declaration" ||
                     kind == "enum_declaration");

    if (is_class && depth > 0) {
        // Emit first line of class and recurse into children
        out << first_line(node, src) << "\n";
        uint32_t cnt = ts_node_child_count(node);
        for (uint32_t i = 0; i < cnt; i++)
            build_skeleton(ts_node_child(node, i), src, lang, depth + 1, out, strip_comments);
        return;
    }

    // C++: namespace_definition and class/struct specifiers — recurse into body
    if (lang == Language::Cpp) {
        if (kind == "namespace_definition" || kind == "class_specifier" || kind == "struct_specifier") {
            if (kind != "namespace_definition") out << first_line(node, src) << "\n";
            uint32_t cnt2 = ts_node_child_count(node);
            for (uint32_t i = 0; i < cnt2; i++)
                build_skeleton(ts_node_child(node, i), src, lang, depth + 1, out, strip_comments);
            return;
        }
        // For C++, declaration_list is a namespace body — recurse into it, don't skip
        if (kind == "declaration_list" || kind == "field_declaration_list") {
            uint32_t cnt2 = ts_node_child_count(node);
            for (uint32_t i = 0; i < cnt2; i++)
                build_skeleton(ts_node_child(node, i), src, lang, depth, out, strip_comments);
            return;
        }
    }

    // Skip pure body nodes
    if (is_body_kind(kind.c_str())) return;

    // Comments: skip if strip_comments is on
    if (is_comment_kind(kind.c_str())) {
        if (!strip_comments) out << node_text(node, src) << "\n";
        return;
    }

    // Imports, type aliases: emit as-is
    bool emit_full = (kind == "import_statement"        ||
                      kind == "import_declaration"       ||
                      kind == "import_from_statement"    ||
                      kind == "use_declaration"          ||
                      kind == "using_directive"          ||  // C#
                      kind == "namespace_use_declaration"||  // PHP
                      kind == "import_or_export"         ||  // Dart
                      kind == "type_alias_declaration"   ||
                      kind == "interface_declaration");
    if (emit_full) {
        out << node_text(node, src) << "\n";
        return;
    }

    // Recurse into children
    uint32_t cnt = ts_node_child_count(node);
    if (cnt == 0) return;
    for (uint32_t i = 0; i < cnt; i++)
        build_skeleton(ts_node_child(node, i), src, lang, depth, out, strip_comments);
}

std::string skeletonize(const std::string& source, Language lang, bool strip_comments) {
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, get_ts_language(lang));
    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), (uint32_t)source.size());
    TSNode root  = ts_tree_root_node(tree);

    std::ostringstream out;

    if (lang == Language::Vue) {
        // Find script_element and sub-parse its raw_text with TS or JS parser
        uint32_t top = ts_node_child_count(root);
        for (uint32_t i = 0; i < top; i++) {
            TSNode child = ts_node_child(root, i);
            if (std::string(ts_node_type(child)) != "script_element") continue;

            bool is_typescript = false;
            uint32_t raw_start = 0, raw_end = 0;
            bool found_raw = false;
            uint32_t cc = ts_node_child_count(child);
            for (uint32_t j = 0; j < cc; j++) {
                TSNode c = ts_node_child(child, j);
                std::string ck = ts_node_type(c);
                if (ck == "start_tag") {
                    uint32_t acc = ts_node_child_count(c);
                    for (uint32_t k = 0; k < acc; k++) {
                        TSNode a = ts_node_child(c, k);
                        if (std::string(ts_node_type(a)) == "attribute") {
                            std::string atxt = node_text(a, source);
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
                    found_raw = true;
                }
            }
            if (!found_raw || raw_end <= raw_start) continue;

            std::string sub_src = source.substr(raw_start, raw_end - raw_start);
            TSParser* sub_parser = ts_parser_new();
            Language sub_lang = is_typescript ? Language::TypeScript : Language::JavaScript;
            ts_parser_set_language(sub_parser, get_ts_language(sub_lang));
            TSTree* sub_tree = ts_parser_parse_string(sub_parser, nullptr,
                sub_src.c_str(), (uint32_t)sub_src.size());
            TSNode sub_root = ts_tree_root_node(sub_tree);

            uint32_t sub_cnt = ts_node_child_count(sub_root);
            for (uint32_t j = 0; j < sub_cnt; j++)
                build_skeleton(ts_node_child(sub_root, j), sub_src, sub_lang, 0, out, strip_comments);

            ts_tree_delete(sub_tree);
            ts_parser_delete(sub_parser);
        }
    } else {
        uint32_t cnt = ts_node_child_count(root);
        for (uint32_t i = 0; i < cnt; i++)
            build_skeleton(ts_node_child(root, i), source, lang, 0, out, strip_comments);
    }

    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return out.str();
}

} // namespace axon
