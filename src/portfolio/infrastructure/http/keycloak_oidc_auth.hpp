#pragma once

#include "portfolio/application/remote_ingest.hpp"

#include <chrono>
#include <string>

namespace axon::portfolio {

struct KeycloakOidcConfig {
    std::string issuer;
    std::string audience;
    std::string jwks_json;
    std::chrono::seconds clock_skew{60};
};

// Validates only asymmetric RS256 access tokens issued by the configured Keycloak realm. The JWKS
// document is supplied by a controlled fetcher/configuration adapter (normally the internal shared
// Keycloak certs endpoint); no request may supply it.
class KeycloakOidcAuthenticator {
public:
    explicit KeycloakOidcAuthenticator(KeycloakOidcConfig config);
    AuthenticatedPrincipal authenticate_bearer(const std::string& authorization,
                                               std::chrono::system_clock::time_point now =
                                                   std::chrono::system_clock::now()) const;

private:
    KeycloakOidcConfig config_;
};

} // namespace axon::portfolio
