#include "embedding_state.hpp"

namespace axon {

std::string stored_embedding_model_id(Database& db) {
    auto result = db.conn().Query("SELECT model_id FROM embedding_state WHERE singleton = true");
    if (result->HasError() || result->RowCount() == 0) return "";
    return result->GetValue(0, 0).ToString();
}

bool any_stored_embedding_vectors(Database& db) {
    for (const char* sql : {"SELECT COUNT(*) FROM symbols WHERE embedding IS NOT NULL",
                            "SELECT COUNT(*) FROM turns WHERE embedding IS NOT NULL",
                            "SELECT COUNT(*) FROM observations WHERE embedding IS NOT NULL",
                            "SELECT COUNT(*) FROM sessions WHERE digest_embedding IS NOT NULL"}) {
        auto result = db.conn().Query(sql);
        if (!result->HasError() && result->GetValue<int64_t>(0, 0) > 0) return true;
    }
    return false;
}

std::string embedding_state_hint(Database& db) {
    const std::string stored = stored_embedding_model_id(db);
    if (!stored.empty()) return stored;
    return any_stored_embedding_vectors(db) ? "legacy" : "";
}

} // namespace axon
