/*
Sentinel — Authenticator
Role: Implements the logic for loading API keys from a file and signing JWTs.
Inputs/Outputs: Reads 'key.json'; creates and signs a JWT with an ES256 algorithm.
Threading: All methods execute on the calling thread.
Performance: File I/O is a one-time cost in the constructor.
Integration: The concrete implementation of the authentication token generator.
Observability: Logs file-not-found and JSON parsing errors to std::cerr.
Related: Authenticator.hpp.
Assumptions: The linked JWT library supports ES256 signing.
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
        throw std::runtime_error("🔑 Authenticator: Failed to open key file: " + path);
    }
    
    nlohmann::json j;
    try {
        key_file >> j;
    }
    catch (const std::exception& ex) {
        throw std::runtime_error("🔑 Authenticator: Failed to parse JSON from key file: " + std::string(ex.what()));
    }
    
    m_keyId = j.value("key", "");
    m_privateKey = j.value("secret", "");
    
    if (m_keyId.empty()) {
        throw std::runtime_error("🔑 Authenticator: Missing 'key' field in key file");
    }
    if (m_privateKey.empty()) {
        throw std::runtime_error("🔑 Authenticator: Missing 'secret' field in key file");
    }
    
    sLog_App(std::string("Authenticator: Successfully loaded API keys from [") + path + "]");
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
