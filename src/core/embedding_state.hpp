#pragma once
#include "db.hpp"
#include <string>

namespace axon {

// Model identity recorded in the index (empty when none was recorded).
std::string stored_embedding_model_id(Database& db);

// True when any symbol, turn, observation or session digest already has a vector.
bool any_stored_embedding_vectors(Database& db);

// Identity of the model behind the vectors already stored: its recorded id, "legacy" for an
// index that predates model tracking (nomic vectors), or "" when there are no vectors yet.
std::string embedding_state_hint(Database& db);

} // namespace axon
