#pragma once

#include "portfolio_store.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace axon::portfolio {

class KeycloakOidcAuthenticator;

// This value is created only by an authentication adapter.  It intentionally has no constructor
// from an HTTP header or request body: callers cannot nominate their own publisher identity.
class AuthenticatedPrincipal {
public:
    std::string principal_id() const { return issuer + "|" + subject; }
    const std::string& audience() const { return audience_; }

private:
    AuthenticatedPrincipal(std::string issuer, std::string subject, std::string audience)
        : issuer(std::move(issuer)), subject(std::move(subject)), audience_(std::move(audience)) {}
    std::string issuer;
    std::string subject;
    std::string audience_;
    friend class KeycloakOidcAuthenticator;
};

struct RemotePublisherBinding {
    std::string binding_id;
    RepositoryStreamKey stream;
    std::string principal_id;
    std::string identity_epoch;
    std::string root_fingerprint;
    std::string repository_contract_digest;
};

struct RemoteEventBatch {
    std::string target_marker;
    RemotePublisherBinding binding;
    std::uint64_t expected_cursor = 0;
    std::vector<ProjectionEvent> events;
    std::optional<RepositoryReidentification> reidentification;
};

class RemoteIngestService {
public:
    explicit RemoteIngestService(PortfolioStore& store, std::string target_marker);

    // Registration is deliberately a server-side operation. HTTP request data cannot create or
    // replace a binding, and reusing a physical stream under another binding fails closed.
    void register_binding(const RemotePublisherBinding& binding);
    // Server-side owner-approved preparation of the pending identity binding. It is the only path
    // that can associate another logical repository with an existing physical stream.
    void register_pending_reidentification_binding(const std::string& old_binding_id,
                                                   const RemotePublisherBinding& binding);
    void register_reidentification_grant(const std::string& old_binding_id,
                                         const std::string& new_binding_id,
                                         const std::string& approval_reference);
    ApplyResult ingest(const AuthenticatedPrincipal& principal, const RemoteEventBatch& batch);
    CursorEpochManifest cursor_probe(const AuthenticatedPrincipal& principal,
                                     const RemotePublisherBinding& binding) const;

private:
    static void validate_binding(const RemotePublisherBinding& binding);
    void require_registered(const AuthenticatedPrincipal& principal,
                            const RemotePublisherBinding& binding) const;

    PortfolioStore& store_;
    std::string target_marker_;
    std::map<std::string, RemotePublisherBinding> bindings_;
    std::map<std::string, std::string> binding_by_stream_;
    std::map<std::string, std::pair<std::string, std::string>> reidentification_grants_;
    mutable std::mutex mutex_;
};

} // namespace axon::portfolio
