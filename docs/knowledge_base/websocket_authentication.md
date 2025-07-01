# 🧠 Knowledge Base: Coinbase WebSocket Authentication

This document details the successful implementation of ES256 JWT authentication for the Coinbase Advanced Trade API, consolidating the learnings from a 24-hour debugging marathon.

## 1. The Challenge: Undocumented JWT Authentication

The primary obstacle was authenticating with the Coinbase Advanced Trade WebSocket (`advanced-trade-ws.coinbase.com`). The API requires a specifically formatted ES256 JWT token, and the practical implementation details were not clearly documented.

Initial failed attempts included:
- Using standard HTTP `CB-ACCESS-*` headers.
- Employing the wrong signing algorithm (RS256 instead of ES256).
- Malforming JWT claims (`issuer`, `subject`, `kid`).
- Sending invalid JSON by concatenating multiple subscription messages.

## 2. The Breakthrough: JWT in the Message Body

The critical discovery was that the JWT token must be sent within the JSON body of the `subscribe` message, not as a transport-level header.

### 2.1. Working JWT Generation

The following C++ code using the `jwt-cpp` library correctly generates the required token:

```cpp
#include <jwt-cpp/jwt.h>
#include <openssl/rand.h>

std::string create_jwt(const std::string& key_name, const std::string& key_secret) {
    try {
        // Generate a random nonce
        unsigned char nonce_raw[16];
        RAND_bytes(nonce_raw, sizeof(nonce_raw));
        std::string nonce(reinterpret_cast<char*>(nonce_raw), sizeof(nonce_raw));

        // Create JWT token
        auto token = jwt::create()
            .set_subject(key_name)           // sub: your_api_key
            .set_issuer("cdp")               // iss: must be "cdp"
            .set_not_before(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds{120})
            .set_header_claim("kid", jwt::claim(key_name))    // kid: your_api_key
            .set_header_claim("nonce", jwt::claim(nonce))     // nonce: random bytes
            .sign(jwt::algorithm::es256("", key_secret));     // ES256 with your private key

        return token;
    }
    catch (const std::exception& ex) {
        std::cerr << "[JWT] generation failed: " << ex.what() << '\n';
        return {};
    }
}
```

### 2.2. Working Subscription Message

The JWT is included as a field in the JSON subscription message:

```cpp
#include <nlohmann/json.hpp>

// Create subscription message
nlohmann::json subscribe_msg;
subscribe_msg["type"] = "subscribe";
subscribe_msg["product_ids"] = {"BTC-USD"};
subscribe_msg["channel"] = "level2"; // or "market_trades"
subscribe_msg["jwt"] = jwt_token; // The generated token

std::string message = subscribe_msg.dump();
```

### 2.3. Dual Channel Subscriptions

To receive both order book data (`level2`) and live trades (`market_trades`), send two separate `subscribe` messages sequentially over the established WebSocket connection. Do not batch them into a single invalid JSON array.

## 3. Key Implementation Learnings

### Authentication & Connection
- **Host**: `advanced-trade-ws.coinbase.com`
- **Port**: `443` (SSL required)
- **Algorithm**: `ES256` is mandatory.
- **Issuer (`iss`)**: Must be the string `"cdp"`.
- **Nonce**: A random nonce in the header is required to prevent replay attacks.
- **Expiration**: A short expiration time (e.g., 120 seconds) is recommended.

### Data Pipeline
- The working data flow is: `WebSocket (Network Thread) -> Parse JSON -> StreamController (Worker Thread) -> Qt Signals -> GUI Widget (GUI Thread)`.
- Use a polling mechanism (e.g., `QTimer`) in the `StreamController` to periodically fetch new data from the network client, which prevents deadlocks and ensures thread safety.
- The `getNewTrades()` function in the data client was a critical missing piece that needed to be implemented to filter and return only new trades since the last poll.

## 4. Final Architecture

The successful integration involved:
1.  **Copying the working JWT logic** into the `CoinbaseStreamClient`.
2.  **Fixing the subscribe message format** to include the JWT in the body.
3.  **Implementing sequential subscriptions** for both `level2` and `market_trades`.
4.  **Ensuring the `getNewTrades()` method was fully implemented** to bridge the backend data store with the frontend polling mechanism.

This approach integrated the standalone proof-of-concept (`simple_coinbase.cpp`) into the existing robust, multi-threaded application architecture without compromising its design. 