#include "ccr.hpp"
#include <blake3.h>
#include <cstdio>

namespace axon {

namespace {

std::string sql_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out;
}

std::string blake3_hex(const std::string& in) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, in.data(), in.size());
    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    char hex[BLAKE3_OUT_LEN * 2 + 1];
    for (size_t i = 0; i < BLAKE3_OUT_LEN; i++)
        snprintf(hex + i * 2, 3, "%02x", out[i]);
    return std::string(hex, BLAKE3_OUT_LEN * 2);
}

} // namespace

std::string ccr_artifact_id(const std::string& kind,
                            const std::string& source_ref,
                            const std::string& content) {
    return "ccr_" + blake3_hex(kind + "\n" + source_ref + "\n" + content);
}

std::string ccr_marker(const std::string& artifact_id,
                       int64_t original_tokens) {
    return "/* axon:ccr artifact_id=" + artifact_id +
           " original_tokens=" + std::to_string(original_tokens) + " */\n";
}

std::string ccr_store_artifact(Database& db,
                               const std::string& kind,
                               const std::string& source_ref,
                               const std::string& content,
                               int64_t token_estimate) {
    std::string id = ccr_artifact_id(kind, source_ref, content);
    auto del = db.conn().Query(
        "DELETE FROM ccr_artifacts WHERE artifact_id = '" + sql_quote(id) + "'");
    if (del->HasError()) return "";
    auto ins = db.conn().Query(
        "INSERT INTO ccr_artifacts "
        "(artifact_id, kind, source_ref, content, token_estimate, created_at) VALUES ('" +
        sql_quote(id) + "', '" + sql_quote(kind) + "', '" + sql_quote(source_ref) + "', '" +
        sql_quote(content) + "', " + std::to_string(token_estimate) + ", now())");
    if (ins->HasError()) return "";
    return id;
}

std::optional<CcrArtifact> ccr_retrieve_artifact(Database& db,
                                                 const std::string& artifact_id) {
    auto res = db.conn().Query(
        "SELECT artifact_id, kind, source_ref, content, token_estimate "
        "FROM ccr_artifacts WHERE artifact_id = '" + sql_quote(artifact_id) + "' LIMIT 1");
    if (res->HasError() || res->RowCount() == 0) return std::nullopt;

    CcrArtifact artifact;
    artifact.artifact_id = res->GetValue(0, 0).ToString();
    artifact.kind = res->GetValue(1, 0).ToString();
    artifact.source_ref = res->GetValue(2, 0).ToString();
    artifact.content = res->GetValue(3, 0).ToString();
    artifact.token_estimate = res->GetValue<int64_t>(4, 0);
    return artifact;
}

CcrRecoverableOutput ccr_make_recoverable_output(Database& db,
                                                 const std::string& kind,
                                                 const std::string& source_ref,
                                                 const std::string& original,
                                                 const std::string& lossy_output,
                                                 int64_t original_tokens) {
    auto estimate_tokens = [](const std::string& s) -> int64_t {
        return static_cast<int64_t>((s.size() + 3) / 4);
    };

    CcrRecoverableOutput result;
    result.input_tokens = original_tokens > 0 ? original_tokens : estimate_tokens(original);
    result.output = original;
    result.output_tokens = result.input_tokens;

    int64_t lossy_tokens = estimate_tokens(lossy_output);
    if (lossy_output.empty() || lossy_tokens >= result.input_tokens) return result;

    std::string id = ccr_store_artifact(db, kind, source_ref, original, result.input_tokens);
    if (id.empty()) return result;

    std::string recoverable = ccr_marker(id, result.input_tokens) + lossy_output;
    int64_t recoverable_tokens = estimate_tokens(recoverable);
    if (recoverable_tokens >= result.input_tokens) return result;

    result.recoverable = true;
    result.output = std::move(recoverable);
    result.artifact_id = std::move(id);
    result.output_tokens = recoverable_tokens;
    result.tokens_saved = result.input_tokens - result.output_tokens;
    return result;
}

} // namespace axon
