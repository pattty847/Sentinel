/*
Sentinel — Authenticator
Role: Creates signed JSON Web Tokens (JWTs) for Coinbase Advanced Trade API authentication.
Inputs/Outputs: Reads API key/secret from 'key.json'; outputs a signed JWT string.
Threading: Intended for use from a single thread; createJwt() is stateless and thread-safe.
Performance: JWT creation is a cryptographic operation, performed once per subscription.
Integration: Instantiated by CoinbaseStreamClient, used by MarketDataCore during subscription.
Observability: Logs errors to std::cerr if 'key.json' is missing or malformed.
Related: Authenticator.cpp, CoinbaseStreamClient.hpp, MarketDataCore.hpp.
Assumptions: A valid 'key.json' file exists in the application's working directory.
*/
#pragma once
// ─────────────────────────────────────────────────────────────
// Authenticator – loads API keys & produces ES256 JWT tokens.
// SRP: authentication only.
// ─────────────────────────────────────────────────────────────
#include <string>
#include <chrono>

class Authenticator {
public:
    explicit Authenticator(const std::string& keyFile = "key.json");

    /// Return a freshly-signed ES256 JWT.  Throws on failure.
    [[nodiscard]] std::string createJwt() const;

    // Non-copyable, movable.
    Authenticator(const Authenticator&)            = delete;
    Authenticator& operator=(const Authenticator&) = delete;
    Authenticator(Authenticator&&)                 = default;
    Authenticator& operator=(Authenticator&&)      = default;

private:
    std::string m_keyId;        // kid / sub
    std::string m_privateKey;   // PEM (ES256)

    void loadKeyFile(const std::string& path);
}; 