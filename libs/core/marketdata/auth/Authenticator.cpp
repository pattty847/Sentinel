/*
Sentinel — Authenticator
Role: Loads CDP API keys from a file (optional) and signs JWTs for authenticated channels.
Public market data (level2, market_trades, heartbeats, candles, etc.) does not require auth;
only the user and futures_balance_summary channels require a key. When key.json is missing or
empty, hasCredentials() is false and callers should omit JWT (see Advanced Trade WebSocket docs).
Threading: All methods execute on the calling thread.
*/
#include "Authenticator.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <iostream>
#include <chrono>
#include "SentinelLogging.hpp"

Authenticator::Authenticator(const std::string& keyFile) {
    loadKeyFile(keyFile);
}

std::string Authenticator::createJwt() const {
    if (m_keyId.empty() || m_privateKey.empty()) {
        throw std::runtime_error("🔑 Authenticator: API key/secret missing – cannot create JWT");
    }
    
    try {
        const std::string nonce = generateNonce();

        // Create JWT token following the Coinbase tutorial format
        auto token = jwt::create()
            .set_subject(m_keyId)                             // sub: key_name (keyId)
            .set_issuer("cdp")                               // iss: "cdp" (not the key!)
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_header_claim("kid", jwt::claim(m_keyId))    // kid: key_name
            .set_header_claim("nonce", jwt::claim(nonce))    // nonce: base64-encoded random bytes
            .sign(jwt::algorithm::es256("", m_privateKey));  // ES256 with private key

        return token;
    }
    catch (const std::exception& ex) {
        throw std::runtime_error(std::string("🔑 Authenticator: JWT generation failed: ") + ex.what());
    }
}

std::string Authenticator::createRestJwt(const std::string& method,
                                         const std::string& host,
                                         const std::string& path) const {
    if (m_keyId.empty() || m_privateKey.empty()) {
        throw std::runtime_error("🔑 Authenticator: API key/secret missing – cannot create JWT");
    }
    if (method.empty() || host.empty() || path.empty()) {
        throw std::runtime_error("🔑 Authenticator: REST JWT requires method, host, and path");
    }

    try {
        const std::string nonce = generateNonce();
        const std::string uri = method + " " + host + path;

        auto token = jwt::create()
            .set_subject(m_keyId)
            .set_issuer("cdp")
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_header_claim("kid", jwt::claim(m_keyId))
            .set_header_claim("nonce", jwt::claim(nonce))
            .set_payload_claim("uri", jwt::claim(uri))
            .sign(jwt::algorithm::es256("", m_privateKey));

        return token;
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("🔑 Authenticator: REST JWT generation failed: ") + ex.what());
    }
}

void Authenticator::loadKeyFile(const std::string& path) {
    std::ifstream key_file(path);
    if (!key_file.is_open()) {
        sLog_App("Authenticator: No key file at [" + path + "] – using public market data only (no JWT).");
        return;
    }

    nlohmann::json j;
    try {
        key_file >> j;
    } catch (const std::exception& ex) {
        sLog_App("Authenticator: Failed to parse key file [" + path + "]: " + ex.what() + " – using public only.");
        return;
    }

    m_keyId = j.value("key", "");
    m_privateKey = j.value("secret", "");

    if (m_keyId.empty() || m_privateKey.empty()) {
        sLog_App("Authenticator: Key file missing 'key' or 'secret' – using public market data only.");
        return;
    }

    sLog_App(std::string("Authenticator: Loaded API keys from [") + path + "] (JWT available for authenticated channels).");
} 

std::string Authenticator::generateNonce() {
    unsigned char nonce_raw[16];
    if (RAND_bytes(nonce_raw, sizeof(nonce_raw)) != 1) {
        throw std::runtime_error("🔑 Authenticator: Failed to generate random nonce");
    }
    
    BIO* bio = nullptr;
    BIO* b64 = nullptr;
    BUF_MEM* bufferPtr = nullptr;
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, nonce_raw, sizeof(nonce_raw));
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    std::string nonce(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    return nonce;
}
