#include "portfolio/application/reference_portfolio_store.hpp"
#include "portfolio/application/remote_ingest.hpp"
#include "portfolio/infrastructure/http/keycloak_oidc_auth.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <vector>

namespace axon::portfolio {
namespace {
using json = nlohmann::json;
constexpr char kIssuer[] = "https://keycloak.hideakiservicos.net/kc/realms/axon";
constexpr char kAudience[] = "axon-portfolio-api";
constexpr char kRepository[] = "11111111-1111-4111-8111-111111111111";
constexpr char kStream[] = "22222222-2222-4222-8222-222222222222";
constexpr char kBinding[] = "33333333-3333-4333-8333-333333333333";
constexpr char kEpoch[] = "0123456789abcdef0123456789abcdef";
constexpr char kFingerprint[] = "abcdef0123456789abcdef0123456789";
constexpr char kMarker[] = "axon/portfolio-sync/v1:test-target";

std::string base64url(const unsigned char* data, std::size_t size) {
    std::string encoded(((size + 2) / 3) * 4, '\0');
    const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), data,
                                         static_cast<int>(size));
    encoded.resize(written);
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    for (char& ch : encoded) { if (ch == '+') ch = '-'; else if (ch == '/') ch = '_'; }
    return encoded;
}

struct SigningFixture {
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa{RSA_new(), RSA_free};

    explicit SigningFixture(int bits = 2048, unsigned long public_exponent = RSA_F4) {
        std::unique_ptr<BIGNUM, decltype(&BN_free)> exponent(BN_new(), BN_free);
        if (!exponent || BN_set_word(exponent.get(), public_exponent) != 1 ||
            RSA_generate_key_ex(rsa.get(), bits, exponent.get(), nullptr) != 1)
            throw std::runtime_error("failed to create signing fixture");
    }

    std::string jwks(json metadata = json::object()) const {
        const BIGNUM *n = nullptr, *e = nullptr;
        RSA_get0_key(rsa.get(), &n, &e, nullptr);
        std::vector<unsigned char> nb(BN_num_bytes(n)), eb(BN_num_bytes(e));
        BN_bn2bin(n, nb.data()); BN_bn2bin(e, eb.data());
        metadata["kty"] = "RSA"; metadata["kid"] = "test-kid"; metadata["use"] = "sig";
        if (!metadata.contains("n")) metadata["n"] = base64url(nb.data(), nb.size());
        if (!metadata.contains("e")) metadata["e"] = base64url(eb.data(), eb.size());
        return json{{"keys", json::array({metadata})}}.dump();
    }

    std::string token(const json& claims, const std::string& alg = "RS256",
                      json extra_header = json::object()) const {
        extra_header["alg"] = alg;
        extra_header["kid"] = "test-kid";
        const auto header = extra_header.dump();
        const auto payload = claims.dump();
        const auto signing = base64url(reinterpret_cast<const unsigned char*>(header.data()), header.size()) + "." +
                             base64url(reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_new(), EVP_PKEY_free);
        if (!key || EVP_PKEY_set1_RSA(key.get(), rsa.get()) != 1)
            throw std::runtime_error("failed to load signing key");
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
            EVP_DigestSignUpdate(context.get(), signing.data(), signing.size()) != 1)
            throw std::runtime_error("failed to initialize signature");
        std::size_t signature_size = 0;
        if (EVP_DigestSignFinal(context.get(), nullptr, &signature_size) != 1)
            throw std::runtime_error("failed to size signature");
        std::vector<unsigned char> signature(signature_size);
        if (EVP_DigestSignFinal(context.get(), signature.data(), &signature_size) != 1)
            throw std::runtime_error("failed to create signature");
        signature.resize(signature_size);
        return signing + "." + base64url(signature.data(), signature.size());
    }
};

RemotePublisherBinding binding() {
    return {kBinding, {kRepository, kStream}, std::string(kIssuer) + "|publisher-1", kEpoch,
            kFingerprint, ""};
}

ProjectionEvent event(std::uint64_t sequence = 1) {
    return {{kRepository, kStream}, sequence, "event-id-0123456789", kEpoch,
            std::string("fedcba9876543210fedcba9876543210"),
            {{"repository", kRepository, ProjectionOperation::Upsert, std::nullopt}}};
}

AuthenticatedPrincipal verified_principal(const SigningFixture& fixture, const std::string& subject) {
    KeycloakOidcAuthenticator auth({kIssuer, kAudience, fixture.jwks(), std::chrono::seconds{0}});
    const auto now = std::chrono::system_clock::now();
    const auto expiration = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + 60;
    return auth.authenticate_bearer("Bearer " + fixture.token(
        {{"iss", kIssuer}, {"aud", kAudience}, {"sub", subject}, {"exp", expiration}}), now);
}

TEST(PortfolioRemoteIngest, RejectsUnregisteredOrMismatchedPublisherBeforeStoreWrite) {
    ReferencePortfolioStore store;
    RemoteIngestService ingest(store, kMarker);
    SigningFixture fixture;
    const auto principal = verified_principal(fixture, "publisher-1");
    RemoteEventBatch batch{kMarker, binding(), 0, {event()}};
    EXPECT_THROW(ingest.ingest(principal, batch), PortfolioStoreError);
    ingest.register_binding(binding());
    const auto wrong = verified_principal(fixture, "publisher-2");
    EXPECT_THROW(ingest.ingest(wrong, batch), PortfolioStoreError);
    EXPECT_EQ(store.stream_state({kRepository, kStream}).cursor, 0u);
    EXPECT_EQ(ingest.ingest(principal, batch).state.cursor, 1u);
    EXPECT_EQ(ingest.ingest(principal, batch).disposition, ApplyDisposition::Duplicate);
}

TEST(PortfolioRemoteIngest, RejectsTargetAndStreamConfusionBeforeStoreWrite) {
    ReferencePortfolioStore store;
    RemoteIngestService ingest(store, kMarker);
    SigningFixture fixture;
    const auto principal = verified_principal(fixture, "publisher-1");
    ingest.register_binding(binding());
    auto batch = RemoteEventBatch{kMarker + std::string("x"), binding(), 0, {event()}};
    EXPECT_THROW(ingest.ingest(principal, batch), PortfolioStoreError);
    batch.target_marker = kMarker;
    batch.events.front().stream.repository_id = "44444444-4444-4444-8444-444444444444";
    EXPECT_THROW(ingest.ingest(principal, batch), PortfolioStoreError);
    EXPECT_EQ(store.stream_state({kRepository, kStream}).cursor, 0u);
}

TEST(PortfolioRemoteIngest, RequiresServerSideGrantForRemoteReidentification) {
    ReferencePortfolioStore store;
    RemoteIngestService ingest(store, kMarker);
    SigningFixture fixture;
    const auto principal = verified_principal(fixture, "publisher-1");
    const auto old_binding = binding();
    auto new_binding = old_binding;
    new_binding.binding_id = "55555555-5555-4555-8555-555555555555";
    new_binding.stream.repository_id = "44444444-4444-4444-8444-444444444444";
    new_binding.identity_epoch = "11111111111111111111111111111111";
    ingest.register_binding(old_binding);
    ingest.register_pending_reidentification_binding(old_binding.binding_id, new_binding);
    ASSERT_EQ(ingest.ingest(principal, {kMarker, old_binding, 0, {event()}}).state.cursor, 1u);
    RepositoryReidentification reid{{kRepository, kStream}, new_binding.stream, 2,
        "reidentify-id-0123456789", "22222222222222222222222222222222", std::nullopt,
        old_binding.binding_id, new_binding.binding_id, "owner-approved:G7", "owner-approved"};
    EXPECT_THROW(ingest.ingest(principal, {kMarker, old_binding, 1, {}, reid}), PortfolioStoreError);
    ingest.register_reidentification_grant(old_binding.binding_id, new_binding.binding_id,
                                           "owner-approved:G7");
    EXPECT_EQ(ingest.ingest(principal, {kMarker, old_binding, 1, {}, reid}).state.cursor, 2u);
    EXPECT_EQ(store.stream_state(new_binding.stream).cursor, 2u);
}

TEST(PortfolioRemoteIngest, KeycloakRs256AuthenticationFailsClosed) {
    SigningFixture fixture;
    KeycloakOidcAuthenticator auth({kIssuer, kAudience, fixture.jwks(), std::chrono::seconds{0}});
    const auto now = std::chrono::system_clock::now();
    const auto expiration = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + 60;
    const json claims{{"iss", kIssuer}, {"aud", json::array({kAudience})}, {"sub", "publisher-1"}, {"exp", expiration}};
    const auto principal = auth.authenticate_bearer("Bearer " + fixture.token(claims), now);
    EXPECT_EQ(principal.principal_id(), std::string(kIssuer) + "|publisher-1");
    auto tampered = fixture.token(claims);
    const auto signature_start = tampered.rfind('.') + 1;
    tampered.at(signature_start + 1) = tampered.at(signature_start + 1) == 'A' ? 'B' : 'A';
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + tampered, now), std::runtime_error);
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(claims, "none"), now), std::runtime_error);
    auto wrong_audience = claims; wrong_audience["aud"] = "another-api";
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(wrong_audience), now), std::runtime_error);
    auto malformed_audience = claims; malformed_audience["aud"] = json::array({kAudience, 42});
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(malformed_audience), now), std::runtime_error);
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(claims, "RS256",
        {{"crit", json::array({"unrecognized"})}, {"unrecognized", true}}), now), std::runtime_error);
    KeycloakOidcAuthenticator wrong_key_algorithm({kIssuer, kAudience, fixture.jwks({{"alg", "RS512"}}), std::chrono::seconds{0}});
    EXPECT_THROW(wrong_key_algorithm.authenticate_bearer("Bearer " + fixture.token(claims), now), std::runtime_error);
    KeycloakOidcAuthenticator signing_only_key({kIssuer, kAudience, fixture.jwks({{"key_ops", json::array({"sign"})}}), std::chrono::seconds{0}});
    EXPECT_THROW(signing_only_key.authenticate_bearer("Bearer " + fixture.token(claims), now), std::runtime_error);
    auto wrong_issuer = claims; wrong_issuer["iss"] = "https://untrusted.example/realm";
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(wrong_issuer), now), std::runtime_error);
    auto expired = claims; expired["exp"] = expiration - 120;
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(expired), now), std::runtime_error);
    auto future = claims; future["nbf"] = expiration + 3600;
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(future), now), std::runtime_error);
    auto malformed_nbf = claims; malformed_nbf["nbf"] = "tomorrow";
    EXPECT_THROW(auth.authenticate_bearer("Bearer " + fixture.token(malformed_nbf), now), std::runtime_error);
}

TEST(PortfolioRemoteIngest, RejectsWeakOrUnsafeJwkRsaParameters) {
    SigningFixture weak(512);
    const auto now = std::chrono::system_clock::now();
    const auto expiration = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + 60;
    const json claims{{"iss", kIssuer}, {"aud", kAudience}, {"sub", "publisher-1"}, {"exp", expiration}};
    KeycloakOidcAuthenticator weak_auth({kIssuer, kAudience, weak.jwks(), std::chrono::seconds{0}});
    EXPECT_THROW(weak_auth.authenticate_bearer("Bearer " + weak.token(claims), now), std::runtime_error);
    SigningFixture regular;
    KeycloakOidcAuthenticator even_exponent({kIssuer, kAudience,
        regular.jwks({{"e", "Ag"}}), std::chrono::seconds{0}});
    EXPECT_THROW(even_exponent.authenticate_bearer("Bearer " + regular.token(claims), now), std::runtime_error);
}

} // namespace
} // namespace axon::portfolio
