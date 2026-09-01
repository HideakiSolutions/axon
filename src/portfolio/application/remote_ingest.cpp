#include "remote_ingest.hpp"

#include <cctype>
#include <stdexcept>

namespace axon::portfolio {
namespace {

bool canonical_uuid(const std::string& value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

} // namespace

RemoteIngestService::RemoteIngestService(PortfolioStore& store, std::string target_marker)
    : store_(store), target_marker_(std::move(target_marker)) {
    if (target_marker_.size() < 16 || target_marker_.size() > 256)
        throw std::invalid_argument("remote target marker is invalid");
}

void RemoteIngestService::validate_binding(const RemotePublisherBinding& binding) {
    if (!canonical_uuid(binding.binding_id) || !canonical_uuid(binding.stream.repository_id) ||
        !canonical_uuid(binding.stream.index_stream_id) || binding.principal_id.size() < 4 ||
        binding.principal_id.size() > 1024 || binding.identity_epoch.size() < 16 ||
        binding.identity_epoch.size() > 128 || binding.root_fingerprint.size() < 16 ||
        binding.root_fingerprint.size() > 128 ||
        (!binding.repository_contract_digest.empty() &&
         (binding.repository_contract_digest.size() < 16 ||
          binding.repository_contract_digest.size() > 128)))
        throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                  "remote publisher binding is invalid");
}

void RemoteIngestService::register_binding(const RemotePublisherBinding& binding) {
    validate_binding(binding);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto by_id = bindings_.find(binding.binding_id);
    if (by_id != bindings_.end()) {
        if (by_id->second.binding_id == binding.binding_id &&
            by_id->second.stream == binding.stream &&
            by_id->second.principal_id == binding.principal_id &&
            by_id->second.identity_epoch == binding.identity_epoch &&
            by_id->second.root_fingerprint == binding.root_fingerprint &&
            by_id->second.repository_contract_digest == binding.repository_contract_digest)
            return;
        throw PortfolioStoreError(PortfolioStoreErrorCode::IdempotencyConflict,
                                  "remote binding id is already bound differently");
    }
    const auto physical = binding_by_stream_.find(binding.stream.index_stream_id);
    if (physical != binding_by_stream_.end() && physical->second != binding.binding_id)
        throw PortfolioStoreError(PortfolioStoreErrorCode::IdempotencyConflict,
                                  "physical stream is already bound to another publisher");
    bindings_.emplace(binding.binding_id, binding);
    binding_by_stream_.emplace(binding.stream.index_stream_id, binding.binding_id);
}

void RemoteIngestService::register_pending_reidentification_binding(
    const std::string& old_binding_id, const RemotePublisherBinding& binding) {
    validate_binding(binding);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto old = bindings_.find(old_binding_id);
    if (old == bindings_.end() ||
        old->second.stream.index_stream_id != binding.stream.index_stream_id ||
        old->second.stream.repository_id == binding.stream.repository_id ||
        old->second.principal_id != binding.principal_id ||
        bindings_.count(binding.binding_id) != 0)
        throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                  "pending reidentification binding is invalid");
    bindings_.emplace(binding.binding_id, binding);
}

void RemoteIngestService::register_reidentification_grant(const std::string& old_binding_id,
                                                          const std::string& new_binding_id,
                                                          const std::string& approval_reference) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (approval_reference.empty() || approval_reference.size() > 512 ||
        bindings_.count(old_binding_id) == 0 || bindings_.count(new_binding_id) == 0 ||
        old_binding_id == new_binding_id)
        throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                  "remote reidentification grant is invalid");
    const auto value = std::make_pair(new_binding_id, approval_reference);
    const auto existing = reidentification_grants_.find(old_binding_id);
    if (existing != reidentification_grants_.end() && existing->second != value)
        throw PortfolioStoreError(PortfolioStoreErrorCode::IdempotencyConflict,
                                  "remote reidentification grant conflicts");
    reidentification_grants_[old_binding_id] = value;
}

void RemoteIngestService::require_registered(const AuthenticatedPrincipal& principal,
                                             const RemotePublisherBinding& binding) const {
    validate_binding(binding);
    if (principal.principal_id().size() < 4 || principal.audience().empty())
        throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                  "authenticated principal is incomplete");
    const auto found = bindings_.find(binding.binding_id);
    if (found == bindings_.end() || found->second.stream != binding.stream ||
        found->second.principal_id != binding.principal_id ||
        found->second.identity_epoch != binding.identity_epoch ||
        found->second.root_fingerprint != binding.root_fingerprint ||
        found->second.repository_contract_digest != binding.repository_contract_digest ||
        principal.principal_id() != found->second.principal_id)
        throw PortfolioStoreError(
            PortfolioStoreErrorCode::IdempotencyConflict,
            "remote publisher binding does not match authenticated principal");
}

ApplyResult RemoteIngestService::ingest(const AuthenticatedPrincipal& principal,
                                        const RemoteEventBatch& batch) {
    if (batch.target_marker != target_marker_ ||
        ((batch.events.empty() && !batch.reidentification) || batch.events.size() > 500))
        throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                  "remote event batch is invalid or targets another server");
    std::lock_guard<std::mutex> lock(mutex_);
    require_registered(principal, batch.binding);
    if (batch.reidentification) {
        if (!batch.events.empty() ||
            batch.reidentification->previous_stream != batch.binding.stream ||
            batch.reidentification->old_binding_id != batch.binding.binding_id)
            throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                      "remote reidentification batch is invalid");
        const auto grant = reidentification_grants_.find(batch.binding.binding_id);
        if (grant == reidentification_grants_.end() ||
            grant->second.first != batch.reidentification->new_binding_id ||
            grant->second.second != batch.reidentification->approval_reference)
            throw PortfolioStoreError(PortfolioStoreErrorCode::IdempotencyConflict,
                                      "remote reidentification has no approved binding grant");
        const auto new_binding = bindings_.find(batch.reidentification->new_binding_id);
        if (new_binding == bindings_.end() ||
            new_binding->second.stream != batch.reidentification->current_stream)
            throw PortfolioStoreError(PortfolioStoreErrorCode::IdempotencyConflict,
                                      "remote reidentification target binding is invalid");
        return store_.reidentify_repository_stream(*batch.reidentification, batch.expected_cursor);
    }
    for (const auto& event : batch.events)
        if (event.stream != batch.binding.stream)
            throw PortfolioStoreError(PortfolioStoreErrorCode::InvalidInput,
                                      "remote event does not match publisher binding");
    return store_.apply(batch.binding.stream, batch.expected_cursor, batch.events);
}

CursorEpochManifest RemoteIngestService::cursor_probe(const AuthenticatedPrincipal& principal,
                                                      const RemotePublisherBinding& binding) const {
    std::lock_guard<std::mutex> lock(mutex_);
    require_registered(principal, binding);
    return store_.stream_state(binding.stream);
}

} // namespace axon::portfolio
