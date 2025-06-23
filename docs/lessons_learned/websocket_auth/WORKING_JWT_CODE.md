# 🔑 WORKING JWT AUTHENTICATION CODE

## This is the EXACT code that works from `simple_coinbase.cpp`

### Working JWT Generation Function (lines 23-42)
```cpp
std::string create_jwt(const std::string& key_name, const std::string& key_secret) {
    try {
        // Generate a random nonce (following the Coinbase tutorial exactly)
        unsigned char nonce_raw[16];
        RAND_bytes(nonce_raw, sizeof(nonce_raw));
        std::string nonce(reinterpret_cast<char*>(nonce_raw), sizeof(nonce_raw));

        // Create JWT token following the Coinbase tutorial format
        auto token = jwt::create()
            .set_subject(key_name)           // sub: key_name
            .set_issuer("cdp")               // iss: "cdp" (not the key!)
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_header_claim("kid", jwt::claim(key_name))    // kid: key_name
            .set_header_claim("nonce", jwt::claim(nonce))     // nonce: random bytes
            .sign(jwt::algorithm::es256("", key_secret));     // ES256 with private key

        return token;
    }
    catch (const std::exception& ex) {
        std::cerr << "[JWT] generation failed: " << ex.what() << '\n';
        return {};
    }
}
```

### Working Subscribe Message Format (lines 76-81)
```cpp
// Create subscription message
nlohmann::json subscribe_msg;
subscribe_msg["type"] = "subscribe";
subscribe_msg["product_ids"] = {"BTC-USD"};
subscribe_msg["channel"] = "level2";
subscribe_msg["jwt"] = jwt_token;

std::string message = subscribe_msg.dump();
```

### Working Connection Details
```cpp
// Host: "advanced-trade-ws.coinbase.com"
// Port: "443" 
// Endpoint: "/"
// SSL: Required
```

### Key Points:
1. **JWT goes in message body, NOT headers**
2. **ES256 algorithm with EC private key**
3. **iss = "cdp", sub = api_key, kid = api_key**
4. **Random nonce in header**
5. **120 second expiration**

## Integration Target: CoinbaseStreamClient::generateJwt()
Replace the broken JWT logic in `libs/core/coinbasestreamclient.cpp` with the above working code. 