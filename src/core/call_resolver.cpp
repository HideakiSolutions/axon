#include "call_resolver.hpp"
#include "db.hpp"
#include "portfolio/domain/index_journal.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace axon {
namespace {

std::string sql_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 4);
    for (char ch : value) {
        if (ch == '\'') escaped += '\'';
        escaped += ch;
    }
    return escaped;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool is_owner_kind(const std::string& kind) {
    static const std::vector<std::string> owners = {
        "class", "partial_class", "sealed_class", "data_class", "struct",   "interface",
        "trait", "impl",          "record",       "module",     "namespace"};
    return std::find(owners.begin(), owners.end(), kind) != owners.end();
}

std::optional<std::string> enclosing_owner(duckdb::Connection& conn, int64_t file_id,
                                           int start_line, int end_line, int64_t symbol_id) {
    auto result = conn.Query("SELECT id, name, kind, start_line, end_line FROM symbols WHERE "
                             "file_id = " +
                             std::to_string(file_id) + " AND id <> " + std::to_string(symbol_id) +
                             " AND start_line <= " + std::to_string(start_line) +
                             " AND end_line >= " + std::to_string(end_line) +
                             " ORDER BY (end_line - start_line), id");
    if (result->HasError()) return std::nullopt;
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
        if (is_owner_kind(result->GetValue(2, row).ToString()))
            return result->GetValue(1, row).ToString();
    }
    return std::nullopt;
}

int signature_arity(const std::string& signature, const std::string& symbol_name) {
    size_t search_from = signature.find(symbol_name);
    if (search_from == std::string::npos) search_from = 0;
    size_t open = signature.find('(', search_from + symbol_name.size());
    if (open == std::string::npos) return -1;

    int round = 0, square = 0, curly = 0, angle = 0;
    bool single_quote = false, double_quote = false, escaped = false;
    int count = 1;
    bool has_token = false;
    std::string first_parameter;
    for (size_t i = open + 1; i < signature.size(); ++i) {
        const char ch = signature[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && (single_quote || double_quote)) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !double_quote) {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote) {
            double_quote = !double_quote;
            continue;
        }
        if (single_quote || double_quote) continue;

        if (ch == ')' && round == 0 && square == 0 && curly == 0 && angle == 0) {
            if (!has_token) return 0;
            const std::string first = lower(first_parameter);
            if (first == "self" || first.rfind("self:", 0) == 0 || first == "cls" ||
                first.rfind("cls:", 0) == 0 || first.rfind("&self", 0) == 0 ||
                first.rfind("&mutself", 0) == 0)
                --count;
            return std::max(count, 0);
        }
        const bool top_level_comma =
            ch == ',' && round == 0 && square == 0 && curly == 0 && angle == 0;
        if (!std::isspace(static_cast<unsigned char>(ch)) && !top_level_comma) {
            has_token = true;
            if (count == 1) first_parameter += ch;
        }
        if (ch == '(')
            ++round;
        else if (ch == ')' && round > 0)
            --round;
        else if (ch == '[')
            ++square;
        else if (ch == ']' && square > 0)
            --square;
        else if (ch == '{')
            ++curly;
        else if (ch == '}' && curly > 0)
            --curly;
        else if (ch == '<')
            ++angle;
        else if (ch == '>' && angle > 0)
            --angle;
        else if (top_level_comma)
            ++count;
    }
    return -1;
}

bool looks_like_test_path(const std::string& path) {
    const std::string value = lower(path);
    return value.find("/test") != std::string::npos || value.find("tests/") == 0 ||
           value.find("_test.") != std::string::npos || value.find(".test.") != std::string::npos;
}

struct Candidate {
    int64_t id = 0;
    int64_t file_id = 0;
    std::string signature;
    std::string path;
    std::optional<std::string> owner;
    int score = std::numeric_limits<int>::min();
};

} // namespace

int resolve_call_edges(duckdb::Connection& conn, int64_t from_file_id,
                       const std::vector<CallSite>& calls,
                       portfolio::Transaction& transaction) {
    int inserted_edges = 0;
    for (const auto& call : calls) {
        if (call.caller_name.empty() || call.callee_name.empty()) continue;

        auto caller_result = conn.Query(
            "SELECT id, start_line, end_line FROM symbols WHERE file_id = " +
            std::to_string(from_file_id) + " AND name = '" + sql_escape(call.caller_name) +
            "' AND start_line <= " + std::to_string(call.line) + " AND end_line >= " +
            std::to_string(call.line) + " ORDER BY (end_line - start_line), id LIMIT 1");
        require_ok(caller_result, "resolve caller symbol");
        if (caller_result->RowCount() == 0) continue;
        const int64_t caller_id = caller_result->GetValue<int64_t>(0, 0);
        const int caller_start = caller_result->GetValue<int32_t>(1, 0);
        const int caller_end = caller_result->GetValue<int32_t>(2, 0);
        const auto caller_owner =
            enclosing_owner(conn, from_file_id, caller_start, caller_end, caller_id);

        auto candidates_result =
            conn.Query("SELECT s.id, s.file_id, COALESCE(s.signature, ''), f.path, "
                       "s.start_line, s.end_line FROM symbols s JOIN files f ON f.id = s.file_id "
                       "WHERE s.name = '" +
                       sql_escape(call.callee_name) + "' ORDER BY s.id");
        require_ok(candidates_result, "resolve callee candidates");

        std::vector<Candidate> candidates;
        for (duckdb::idx_t row = 0; row < candidates_result->RowCount(); ++row) {
            Candidate candidate;
            candidate.id = candidates_result->GetValue<int64_t>(0, row);
            candidate.file_id = candidates_result->GetValue<int64_t>(1, row);
            candidate.signature = candidates_result->GetValue(2, row).ToString();
            candidate.path = candidates_result->GetValue(3, row).ToString();
            const int start_line = candidates_result->GetValue<int32_t>(4, row);
            const int end_line = candidates_result->GetValue<int32_t>(5, row);
            candidate.owner =
                enclosing_owner(conn, candidate.file_id, start_line, end_line, candidate.id);

            int score = candidate.file_id == from_file_id ? 20 : 0;
            if (!looks_like_test_path(candidate.path)) score += 2;

            std::string expected_owner = call.qualifier;
            const std::string normalized_qualifier = lower(expected_owner);
            if ((normalized_qualifier == "this" || normalized_qualifier == "self") && caller_owner)
                expected_owner = *caller_owner;
            if (!expected_owner.empty() && candidate.owner) {
                if (lower(*candidate.owner) == lower(expected_owner))
                    score += 100;
                else if (std::isupper(static_cast<unsigned char>(expected_owner.front())))
                    score -= 100;
            } else if (call.qualifier.empty() && caller_owner && candidate.owner &&
                       lower(*caller_owner) == lower(*candidate.owner)) {
                score += 40;
            }

            const int arity = signature_arity(candidate.signature, call.callee_name);
            if (call.argument_count >= 0 && arity >= 0)
                score += arity == call.argument_count ? 30 : -10;

            candidate.score = score;
            candidates.push_back(std::move(candidate));
        }
        if (candidates.empty()) continue;

        const auto best = std::max_element(candidates.begin(), candidates.end(),
                                           [](const Candidate& lhs, const Candidate& rhs) {
                                               if (lhs.score != rhs.score)
                                                   return lhs.score < rhs.score;
                                               return lhs.id > rhs.id;
                                           });
        if (best->id == caller_id) continue;

        auto inserted = conn.Query(
            "INSERT INTO edges (id, from_file, to_file, kind, from_symbol, to_symbol) VALUES (" +
            std::string("nextval('seq_id'), ") + std::to_string(from_file_id) + ", " +
            std::to_string(best->file_id) + ", 'calls', " + std::to_string(caller_id) + ", " +
            std::to_string(best->id) + ")");
        require_success(std::move(inserted), "insert call edge");
        transaction.mark_index_mutation();
        ++inserted_edges;
    }
    return inserted_edges;
}

int resolve_call_edges(duckdb::Connection& conn, int64_t from_file_id,
                       const std::vector<CallSite>& calls) {
    portfolio::Transaction transaction(conn);
    const int inserted = resolve_call_edges(conn, from_file_id, calls, transaction);
    if (inserted > 0) {
        const std::string manifest = portfolio::compute_manifest_hash(conn);
        std::vector<portfolio::AffectedEntity> affected = {
            {"dependency", "call-edges-from-file:" + std::to_string(from_file_id), "upsert",
             std::nullopt}};
        portfolio::append_index_event(transaction, conn, "IndexSymbolsUpdated", affected,
                                      manifest);
    }
    transaction.commit();
    return inserted;
}

} // namespace axon
