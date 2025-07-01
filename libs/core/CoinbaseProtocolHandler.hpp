#pragma once
#include <vector>
#include <string>
#include "CoinbaseConnection.hpp"
#include "Authenticator.hpp"
#include "CoinbaseMessageParser.hpp"

// ----------------------------------------------------------------------------
// CoinbaseProtocolHandler
// Implements Coinbase-specific logic using CoinbaseConnection. Handles
// authentication, subscription messages and passes raw payloads to the parser.
// ----------------------------------------------------------------------------
class CoinbaseProtocolHandler {
public:
    CoinbaseProtocolHandler(CoinbaseConnection& connection,
                            Authenticator& auth,
                            CoinbaseMessageParser& parser,
                            const std::vector<std::string>& products);

    void start();
    void stop();

private:
    std::string buildSubscribe(const std::string& channel) const;
    void handleOpen();
    void handleMessage(const std::string& payload);

    CoinbaseConnection& m_connection;
    Authenticator& m_auth;
    CoinbaseMessageParser& m_parser;
    std::vector<std::string> m_products;
};
