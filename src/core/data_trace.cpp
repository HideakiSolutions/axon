#include "data_trace.hpp"

#include <tree_sitter/api.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

extern "C" {
TSLanguage* tree_sitter_typescript();
TSLanguage* tree_sitter_javascript();
}

namespace axon {
namespace {

std::string type(TSNode n) { return ts_node_is_null(n) ? "" : ts_node_type(n); }
std::string slice(TSNode n, const std::string& src) {
    if (ts_node_is_null(n) || ts_node_end_byte(n) > src.size()) return {};
    return src.substr(ts_node_start_byte(n), ts_node_end_byte(n) - ts_node_start_byte(n));
}
TSNode field(TSNode n, const char* name) { return ts_node_child_by_field_name(n, name, std::char_traits<char>::length(name)); }
int line(TSNode n) { return static_cast<int>(ts_node_start_point(n).row) + 1; }
bool is_function(const std::string& t) {
    return t == "function_declaration" || t == "method_definition" ||
           t == "function_expression" || t == "arrow_function" || t == "generator_function_declaration";
}
bool is_control(const std::string& t) {
    return t == "if_statement" || t == "for_statement" || t == "for_in_statement" ||
           t == "while_statement" || t == "do_statement" || t == "switch_statement" ||
           t == "try_statement" || t == "catch_clause" || t == "conditional_expression";
}
bool source_expr(const std::string& s) {
    static const std::regex re(R"(^(req|request)\.(body|params|query|headers)(\.|\[|$))");
    return std::regex_search(s, re);
}
bool dynamic_expr(const std::string& s) { return s.find('[') != std::string::npos || s.find("?.") != std::string::npos; }
std::string sink_kind(const std::string& name) {
    static const std::regex db(R"(^(db|database|repository|repo|prisma|model|collection)\.(save|create|insert|update|upsert|write|put|set|delete|insertOne|updateOne)$)");
    static const std::regex http(R"(^(fetch|axios(\.(get|post|put|patch|delete|request))?|http(\.(get|post|put|patch|delete|request))?)$)");
    if (std::regex_match(name, db)) return "persistence";
    if (std::regex_match(name, http)) return "http";
    return {};
}

struct Flow {
    std::string source;
    std::vector<DataTraceStep> steps;
    bool unknown = false;
    std::string reason;
};
using Env = std::unordered_map<std::string, Flow>;

Flow eval(TSNode n, const std::string& src, const Env& env) {
    Flow out;
    if (ts_node_is_null(n)) return out;
    auto t = type(n);
    auto s = slice(n, src);
    if ((t == "member_expression" || t == "subscript_expression" || t == "optional_chain") && source_expr(s)) {
        out.source = s;
        out.unknown = dynamic_expr(s);
        if (out.unknown) out.reason = "propriedade de request dinâmica";
        out.steps.push_back({"source", s, line(n), out.unknown ? "unknown" : "observed"});
        return out;
    }
    if (t == "identifier" && env.count(s)) return env.at(s);
    if (t == "call_expression") {
        auto callee = slice(field(n, "function"), src);
        if (callee.empty()) callee = slice(ts_node_named_child(n, 0), src);
        auto args = field(n, "arguments");
        Flow merged;
        for (uint32_t i = 0; i < ts_node_named_child_count(args); ++i) {
            auto part = eval(ts_node_named_child(args, i), src, env);
            if (part.source.empty()) continue;
            if (!merged.source.empty() && merged.source != part.source) {
                merged.unknown = true;
                merged.reason = "múltiplas origens combinadas";
            }
            if (merged.source.empty()) merged.source = part.source;
            merged.steps.insert(merged.steps.end(), part.steps.begin(), part.steps.end());
            merged.unknown = merged.unknown || part.unknown;
            if (merged.reason.empty()) merged.reason = part.reason;
        }
        // Chained string transforms receive data through the callee object.
        if (type(field(n, "function")) == "member_expression") {
            auto receiver = field(field(n, "function"), "object");
            auto part = eval(receiver, src, env);
            if (merged.source.empty() && !part.source.empty()) merged = part;
        }
        if (!merged.source.empty()) {
            const bool known = callee.ends_with(".trim") || callee.ends_with(".toLowerCase") ||
                               callee.ends_with(".toUpperCase") || callee == "String" ||
                               callee == "JSON.stringify";
            merged.steps.push_back({"transform", s, line(n), known ? "inferred" : "unknown"});
            if (!known) {
                merged.unknown = true;
                merged.reason = "retorno de chamada entre funções não resolvido";
            }
        }
        return merged;
    }
    // Expressions such as object literals and string concatenation combine their
    // named children; avoid inventing a unique path when origins differ.
    for (uint32_t i = 0; i < ts_node_named_child_count(n); ++i) {
        auto child = eval(ts_node_named_child(n, i), src, env);
        if (child.source.empty()) continue;
        if (!out.source.empty() && out.source != child.source) {
            out.unknown = true;
            out.reason = "múltiplas origens combinadas";
        }
        if (out.source.empty()) out.source = child.source;
        out.steps.insert(out.steps.end(), child.steps.begin(), child.steps.end());
        out.unknown = out.unknown || child.unknown;
        if (out.reason.empty()) out.reason = child.reason;
    }
    return out;
}

void collect_sinks(TSNode n, const std::string& src, const Env& env,
                   const std::string& function, bool uncertain, DataTraceResult& result) {
    if (ts_node_is_null(n) || is_function(type(n))) return;
    if (type(n) == "call_expression") {
        auto callee = slice(field(n, "function"), src);
        if (callee.empty()) callee = slice(ts_node_named_child(n, 0), src);
        auto kind = sink_kind(callee);
        if (!kind.empty()) {
            ++result.sinks_seen;
            Flow flow;
            auto args = field(n, "arguments");
            for (uint32_t i = 0; i < ts_node_named_child_count(args); ++i) {
                auto candidate = eval(ts_node_named_child(args, i), src, env);
                if (candidate.source.empty()) continue;
                if (flow.source.empty()) flow = candidate;
                else if (flow.source != candidate.source) {
                    flow.unknown = true;
                    flow.reason = "múltiplas origens combinadas";
                }
            }
            if (!flow.source.empty()) {
                DataTracePath path;
                path.function = function;
                path.source = flow.source;
                path.sink = callee;
                path.sink_kind = kind;
                path.status = uncertain || flow.unknown ? "unknown" : "confirmed";
                path.reason = uncertain ? "fluxo de controle ou atribuição condicional" : flow.reason;
                path.steps = std::move(flow.steps);
                path.steps.push_back({"sink", slice(n, src), line(n), path.status == "confirmed" ? "observed" : "unknown"});
                if (path.status == "unknown") ++result.unknown_sinks;
                result.paths.push_back(std::move(path));
            }
        }
    }
    for (uint32_t i = 0; i < ts_node_named_child_count(n); ++i)
        collect_sinks(ts_node_named_child(n, i), src, env, function,
                      uncertain || is_control(type(n)), result);
}

void process_statement(TSNode n, const std::string& src, Env& env,
                       const std::string& function, DataTraceResult& result,
                       bool uncertain = false) {
    auto t = type(n);
    if (t == "statement_block") {
        for (uint32_t i = 0; i < ts_node_named_child_count(n); ++i)
            process_statement(ts_node_named_child(n, i), src, env, function, result, uncertain);
        return;
    }
    if (is_control(t)) {
        if (t == "if_statement") {
            // Each branch starts with the same incoming definitions. A value
            // defined on just one branch is only a possible dependency after
            // the merge, including the implicit fallthrough when no else exists.
            Env before = env, branch = env;
            auto consequence = field(n, "consequence");
            auto alternative = field(n, "alternative");
            collect_sinks(field(n, "condition"), src, env, function, true, result);
            if (!ts_node_is_null(consequence))
                process_statement(consequence, src, branch, function, result, true);
            Env other = before;
            if (type(alternative) == "else_clause")
                alternative = ts_node_named_child(alternative, 0);
            if (!ts_node_is_null(alternative))
                process_statement(alternative, src, other, function, result, true);
            std::unordered_set<std::string> names;
            for (const auto& [name, _] : branch) names.insert(name);
            for (const auto& [name, _] : other) names.insert(name);
            Env merged;
            for (const auto& name : names) {
                const auto a = branch.find(name), b = other.find(name);
                if (a == branch.end() && b == other.end()) continue;
                Flow candidate = a != branch.end() ? a->second : b->second;
                if (candidate.source.empty()) continue;
                candidate.unknown = true;
                candidate.reason = "definição em ramo condicional";
                merged[name] = std::move(candidate);
            }
            env = std::move(merged);
        } else {
            collect_sinks(n, src, env, function, true, result);
        }
        return;
    }
    collect_sinks(n, src, env, function, uncertain, result);
    if (t == "lexical_declaration" || t == "variable_declaration") {
        for (uint32_t i = 0; i < ts_node_named_child_count(n); ++i) {
            auto decl = ts_node_named_child(n, i);
            if (type(decl) != "variable_declarator") continue;
            auto lhs = field(decl, "name");
            auto rhs = field(decl, "value");
            if (type(lhs) != "identifier") continue;
            auto name = slice(lhs, src);
            auto flow = eval(rhs, src, env);
            if (flow.source.empty()) { env.erase(name); continue; }
            flow.steps.push_back({"assignment", name + " = " + slice(rhs, src), line(decl), "observed"});
            env[name] = std::move(flow);
        }
    } else if (t == "expression_statement") {
        auto expr = ts_node_named_child(n, 0);
        if (type(expr) != "assignment_expression") return;
        auto lhs = field(expr, "left");
        auto rhs = field(expr, "right");
        if (type(lhs) != "identifier") return;
        auto name = slice(lhs, src);
        auto flow = eval(rhs, src, env);
        if (flow.source.empty()) { env.erase(name); return; }
        flow.steps.push_back({"assignment", name + " = " + slice(rhs, src), line(expr), "observed"});
        env[name] = std::move(flow);
    }
}

void scan_functions(TSNode n, const std::string& src, const std::optional<std::string>& filter,
                    DataTraceResult& result) {
    if (ts_node_is_null(n)) return;
    if (is_function(type(n))) {
        auto name = slice(field(n, "name"), src);
        if (name.empty()) name = "<anonymous@" + std::to_string(line(n)) + ">";
        if (!filter || *filter == name) {
            auto body = field(n, "body");
            if (!ts_node_is_null(body)) {
                ++result.functions_scanned;
                Env env;
                for (uint32_t i = 0; i < ts_node_named_child_count(body); ++i)
                    process_statement(ts_node_named_child(body, i), src, env, name, result);
            }
        }
        // Nested functions are independent scopes and must never inherit env.
    }
    for (uint32_t i = 0; i < ts_node_named_child_count(n); ++i)
        scan_functions(ts_node_named_child(n, i), src, filter, result);
}

} // namespace

DataTraceResult trace_data_flow(const std::filesystem::path& file,
                                const std::optional<std::string>& symbol_filter) {
    const auto started = std::chrono::steady_clock::now();
    DataTraceResult result;
    result.file = file.string();
    result.limitations = {"somente TS/JS dentro de uma função", "sem propagação entre funções",
                          "ramificações e propriedades dinâmicas são desconhecidas",
                          "somente sinks estáticos de persistência e HTTP declarados"};
    auto ext = file.extension().string();
    const bool ts = ext == ".ts" || ext == ".tsx";
    const bool js = ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs";
    if (!ts && !js) return result;
    std::ifstream in(file, std::ios::binary);
    if (!in) return result;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto src = buf.str();
    result.source_bytes = src.size();
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, ts ? tree_sitter_typescript() : tree_sitter_javascript());
    TSTree* tree = ts_parser_parse_string(parser, nullptr, src.c_str(), static_cast<uint32_t>(src.size()));
    if (tree) {
        result.supported = true;
        scan_functions(ts_tree_root_node(tree), src, symbol_filter, result);
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    for (const auto& path : result.paths) {
        result.trace_bytes += sizeof(path) + path.function.size() + path.source.size() + path.sink.size() + path.reason.size();
        for (const auto& step : path.steps) result.trace_bytes += sizeof(step) + step.expression.size();
    }
    result.elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace axon
