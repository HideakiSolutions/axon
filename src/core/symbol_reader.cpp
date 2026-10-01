#include "symbol_reader.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace axon {

namespace {

int estimate(const std::string& text) {
    return static_cast<int>((text.size() + 3) / 4);
}

std::string quote(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

std::vector<std::string> read_lines(const fs::path& path, int first, int last) {
    std::vector<std::string> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::string line;
    for (int number = 1; std::getline(in, line) && number <= last; ++number) {
        if (number < first) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(to_valid_utf8(line));
    }
    return out;
}

} // namespace

std::string elide_body(const std::vector<std::string>& lines, int cap_tokens) {
    std::string whole;
    for (const auto& line : lines)
        whole += line + "\n";
    if (estimate(whole) <= cap_tokens) return whole;
    std::string head, tail;
    size_t head_end = 0, tail_begin = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (estimate(head) + estimate(lines[i]) > cap_tokens * 60 / 100) break;
        head += lines[i] + "\n";
        head_end = i + 1;
    }
    for (size_t i = lines.size(); i > head_end; --i) {
        if (estimate(tail) + estimate(lines[i - 1]) > cap_tokens * 30 / 100) break;
        tail = lines[i - 1] + "\n" + tail;
        tail_begin = i - 1;
    }
    const size_t elided = tail_begin > head_end ? tail_begin - head_end : 0;
    return head + "    // … " + std::to_string(elided) + " lines elided …\n" + tail;
}

std::vector<SymbolMatch> read_symbols(Database& db, const fs::path& project_root,
                                      const std::string& name, const std::string& file_filter,
                                      const std::string& kind_filter, int token_budget,
                                      int max_matches) {
    std::string simple = name;
    for (const char* separator : {"::", ".", "#"}) {
        const auto at = simple.rfind(separator);
        if (at != std::string::npos) simple = simple.substr(at + std::string(separator).size());
    }
    std::string sql =
        "SELECT s.id, s.name, s.kind, s.start_line, s.end_line, COALESCE(s.signature,''), "
        "COALESCE(s.docstring,''), f.path FROM symbols s JOIN files f ON f.id = s.file_id "
        "WHERE s.name = '" +
        quote(simple) + "'";
    if (!file_filter.empty()) sql += " AND f.path LIKE '%" + quote(file_filter) + "'";
    if (!kind_filter.empty()) sql += " AND s.kind = '" + quote(kind_filter) + "'";
    sql += " ORDER BY CASE WHEN s.kind IN ('function','async_function','method','constructor',"
           "'class','struct','interface','enum') THEN 0 ELSE 1 END, f.path, s.start_line LIMIT " +
           std::to_string(std::clamp(max_matches, 1, 50));
    auto res = db.conn().Query(sql);
    if (res->HasError()) throw std::runtime_error("symbol lookup failed: " + res->GetError());

    std::vector<SymbolMatch> out;
    for (duckdb::idx_t i = 0; i < res->RowCount(); ++i) {
        SymbolMatch m;
        m.name = res->GetValue(1, i).ToString();
        m.kind = res->GetValue(2, i).ToString();
        m.start_line = res->GetValue<int32_t>(3, i);
        m.end_line = res->GetValue<int32_t>(4, i);
        m.signature = res->GetValue(5, i).ToString();
        m.docstring = res->GetValue(6, i).ToString();
        m.path = res->GetValue(7, i).ToString();
        out.push_back(std::move(m));
    }
    // Bodies for the first matches, splitting the budget; the rest stay signature-only.
    const size_t with_body = std::min<size_t>(out.size(), 3);
    const int per_body =
        std::max(200, token_budget / static_cast<int>(std::max<size_t>(1, with_body)));
    for (size_t i = 0; i < with_body; ++i) {
        auto& m = out[i];
        const auto lines = read_lines(project_root / m.path, m.start_line, m.end_line);
        if (lines.empty()) continue;
        m.content = elide_body(lines, per_body);
        m.elided = m.content.find("lines elided") != std::string::npos;
        m.token_estimate = estimate(m.content);
    }
    return out;
}

} // namespace axon
