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