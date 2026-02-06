#pragma once
#include <string>
#include <chrono>

class Authenticator {
public:
    explicit Authenticator(const std::string& keyFile = "key.json");

    [[nodiscard]] std::string createJwt() const;
    [[nodiscard]] std::string createRestJwt(const std::string& method,
                                            const std::string& host,
                                            const std::string& path) const;

    Authenticator(const Authenticator&)            = delete;
    Authenticator& operator=(const Authenticator&) = delete;
    Authenticator(Authenticator&&)                 = default;
    Authenticator& operator=(Authenticator&&)      = default;

private:
    std::string m_keyId;        // kid / sub
    std::string m_privateKey;   // PEM (ES256)

    void loadKeyFile(const std::string& path);
    static std::string generateNonce();
}; 
