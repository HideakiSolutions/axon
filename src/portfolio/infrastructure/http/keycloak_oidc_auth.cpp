#include "keycloak_oidc_auth.hpp"

#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

namespace axon::portfolio {
namespace {
using json = nlohmann::json;

[[noreturn]] void denied(const char* message) {
    throw std::runtime_error(message);
}

std::vector<unsigned char> base64url_decode(const std::string& input) {
    if (input.empty() || input.size() > 16384) denied("JWT segment is invalid");
    std::string b64 = input;
    for (char& ch : b64) {
        if (ch == '-')
            ch = '+';
        else if (ch == '_')
            ch = '/';
        else if (!std::isalnum(static_cast<unsigned char>(ch)))
            denied("JWT base64url is invalid");
    }
    while (b64.size() % 4 != 0)
        b64.push_back('=');
    std::vector<unsigned char> result((b64.size() / 4) * 3);
    const auto decoded =
        EVP_DecodeBlock(result.data(), reinterpret_cast<const unsigned char*>(b64.data()),
                        static_cast<int>(b64.size()));
    if (decoded < 0) denied("JWT base64url decode failed");
    std::size_t size = static_cast<std::size_t>(decoded);
    if (!b64.empty() && b64.back() == '=') --size;
    if (b64.size() > 1 && b64[b64.size() - 2] == '=') --size;
    result.resize(size);
    return result;
}

json decode_json(const std::string& segment) {
    const auto bytes = base64url_decode(segment);
    try {
        return json::parse(bytes.begin(), bytes.end());
    } catch (const json::exception&) {
        denied("JWT JSON is invalid");
    }
}

bool audience_matches(const json& audience, const std::string& expected) {
    if (audience.is_string()) return audience.get<std::string>() == expected;
    if (!audience.is_array()) return false;
    bool matched = false;
    for (const auto& entry : audience) {
        if (!entry.is_string()) return false;
        matched = matched || entry.get<std::string>() == expected;
    }
    return matched;
}

bool key_permits_rs256_verification(const json& jwk) {
    if (!jwk.is_object() || (jwk.contains("alg") && (!jwk.at("alg").is_string() ||
                                                     jwk.at("alg").get<std::string>() != "RS256")))
        return false;
    if (!jwk.contains("key_ops")) return true;
    if (!jwk.at("key_ops").is_array()) return false;
    bool permits_verify = false;
    for (const auto& operation : jwk.at("key_ops")) {
        if (!operation.is_string()) return false;
        permits_verify = permits_verify || operation.get<std::string>() == "verify";
    }
    return permits_verify;
}

void verify_rs256(const std::string& signing_input, const std::vector<unsigned char>& signature,
                  const json& jwk) {
    if (!jwk.is_object() || jwk.value("kty", "") != "RSA" || !jwk.contains("n") ||
        !jwk.contains("e") || !jwk.at("n").is_string() || !jwk.at("e").is_string())
        denied("JWKS key is invalid");
    const auto modulus = base64url_decode(jwk.at("n").get<std::string>());
    const auto exponent = base64url_decode(jwk.at("e").get<std::string>());
    if (modulus.size() < 256 || exponent.empty() || exponent.size() > 8)
        denied("JWKS RSA key strength is invalid");
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n(
        BN_bin2bn(modulus.data(), modulus.size(), nullptr), BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> e(
        BN_bin2bn(exponent.data(), exponent.size(), nullptr), BN_free);
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_new(), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!n || !e || BN_num_bits(n.get()) < 2048 || !BN_is_odd(e.get()) || BN_is_one(e.get()) ||
        !rsa || !key || !ctx || RSA_set0_key(rsa.get(), n.release(), e.release(), nullptr) != 1 ||
        EVP_PKEY_assign_RSA(key.get(), rsa.release()) != 1 ||
        EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestVerifyUpdate(ctx.get(), signing_input.data(), signing_input.size()) != 1 ||
        EVP_DigestVerifyFinal(ctx.get(), signature.data(), signature.size()) != 1)
        denied("JWT signature is invalid");
}

} // namespace

KeycloakOidcAuthenticator::KeycloakOidcAuthenticator(KeycloakOidcConfig config)
    : config_(std::move(config)) {
    if (config_.issuer.rfind("https://", 0) != 0 || config_.issuer.size() > 512 ||
        config_.audience.empty() || config_.audience.size() > 256 ||
        config_.jwks_json.size() > 1024 * 1024)
        throw std::invalid_argument("Keycloak OIDC configuration is invalid");
    try {
        const auto jwks = json::parse(config_.jwks_json);
        if (!jwks.is_object() || !jwks.contains("keys") || !jwks.at("keys").is_array())
            throw std::invalid_argument("Keycloak JWKS is invalid");
    } catch (const json::exception&) {
        throw std::invalid_argument("Keycloak JWKS is invalid");
    }
}

AuthenticatedPrincipal
KeycloakOidcAuthenticator::authenticate_bearer(const std::string& authorization,
                                               std::chrono::system_clock::time_point now) const {
    constexpr char prefix[] = "Bearer ";
    if (authorization.rfind(prefix, 0) != 0 || authorization.size() > 32768)
        denied("Bearer authorization is required");
    const auto token = authorization.substr(sizeof(prefix) - 1);
    const auto one = token.find('.');
    const auto two = one == std::string::npos ? std::string::npos : token.find('.', one + 1);
    if (one == std::string::npos || two == std::string::npos ||
        token.find('.', two + 1) != std::string::npos)
        denied("JWT shape is invalid");
    const auto header = decode_json(token.substr(0, one));
    const auto claims = decode_json(token.substr(one + 1, two - one - 1));
    if (!header.is_object() || header.contains("crit") || header.value("alg", "") != "RS256" ||
        !header.contains("kid") || !header.at("kid").is_string() || !claims.is_object() ||
        claims.value("iss", "") != config_.issuer || !claims.contains("aud") ||
        !audience_matches(claims.at("aud"), config_.audience) || !claims.contains("sub") ||
        !claims.at("sub").is_string() || claims.at("sub").get<std::string>().empty() ||
        !claims.contains("exp") || !claims.at("exp").is_number_integer())
        denied("JWT claims are invalid");
    const auto expires = std::chrono::system_clock::time_point{
        std::chrono::seconds{claims.at("exp").get<long long>()}};
    if (now - config_.clock_skew >= expires) denied("JWT is expired");
    if (claims.contains("nbf")) {
        if (!claims.at("nbf").is_number_integer()) denied("JWT not-before claim is invalid");
        const auto not_before = std::chrono::system_clock::time_point{
            std::chrono::seconds{claims.at("nbf").get<long long>()}};
        if (now + config_.clock_skew < not_before) denied("JWT is not active");
    }
    const auto jwks = json::parse(config_.jwks_json);
    const auto kid = header.at("kid").get<std::string>();
    const auto key =
        std::find_if(jwks.at("keys").begin(), jwks.at("keys").end(), [&](const json& candidate) {
            return candidate.is_object() && candidate.value("kid", "") == kid &&
                   candidate.value("use", "sig") == "sig" &&
                   key_permits_rs256_verification(candidate);
        });
    if (key == jwks.at("keys").end()) denied("JWT key id is unknown");
    verify_rs256(token.substr(0, two), base64url_decode(token.substr(two + 1)), *key);
    return AuthenticatedPrincipal(config_.issuer, claims.at("sub").get<std::string>(),
                                  config_.audience);
}

} // namespace axon::portfolio
