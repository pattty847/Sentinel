#include "CoinbaseProtocolHandler.hpp"
#include <utility>

CoinbaseProtocolHandler::CoinbaseProtocolHandler(CoinbaseConnection& connection,
                                                 Authenticator& auth,
                                                 CoinbaseMessageParser& parser,
                                                 const std::vector<std::string>& products)
    : m_connection(connection)
    , m_auth(auth)
    , m_parser(parser)
    , m_products(products)
{
    m_connection.on_open = [this]() { handleOpen(); };
    m_connection.on_message = [this](const std::string& msg) { handleMessage(msg); };
}

void CoinbaseProtocolHandler::start() {
    m_connection.start();
}

void CoinbaseProtocolHandler::stop() {
    m_connection.stop();
}

std::string CoinbaseProtocolHandler::buildSubscribe(const std::string& channel) const {
    nlohmann::json sub;
    sub["type"] = "subscribe";
    sub["product_ids"] = m_products;
    sub["channel"] = channel;
    sub["jwt"] = m_auth.createJwt();
    return sub.dump();
}

void CoinbaseProtocolHandler::handleOpen() {
    std::string l2 = buildSubscribe("level2");
    m_connection.send(l2);
    std::string trades = buildSubscribe("market_trades");
    m_connection.send(trades);
}

void CoinbaseProtocolHandler::handleMessage(const std::string& payload) {
    m_parser.parseAndDispatch(payload);
}

