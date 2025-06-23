#include <iostream>
#include <string>
#include <fstream>
#include <chrono>
#include <jwt-cpp/jwt.h>
#include <nlohmann/json.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/rand.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

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

int main() {
    try {
        // Load API credentials
        std::ifstream key_file("key.json");
        if (!key_file.is_open()) {
            std::cerr << "❌ key.json file not found!" << std::endl;
            std::cerr << "Create a key.json file with your Coinbase API credentials:" << std::endl;
            std::cerr << R"({"key": "your_api_key", "secret": "your_private_key"})" << std::endl;
            return 1;
        }
        
        nlohmann::json creds;
        key_file >> creds;
        std::string api_key = creds.value("key", "");
        std::string api_secret = creds.value("secret", "");
        
        if (api_key.empty() || api_secret.empty()) {
            std::cerr << "❌ Invalid API credentials in key.json" << std::endl;
            return 1;
        }

        std::cout << "🔐 Generating JWT token..." << std::endl;
        std::string jwt_token = create_jwt(api_key, api_secret);
        if (jwt_token.empty()) {
            std::cerr << "❌ Failed to generate JWT token" << std::endl;
            return 1;
        }
        std::cout << "✅ JWT token generated successfully" << std::endl;

        // Create subscription message
        nlohmann::json subscribe_msg;
        subscribe_msg["type"] = "subscribe";
        subscribe_msg["product_ids"] = {"BTC-USD"};
        subscribe_msg["channel"] = "level2";
        subscribe_msg["jwt"] = jwt_token;
        
        std::string message = subscribe_msg.dump();
        std::cout << "📤 Subscribe message: " << message << std::endl;

        // Set up networking
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

        std::cout << "🔌 Connecting to advanced-trade-ws.coinbase.com..." << std::endl;
        
        // Resolve and connect
        auto const results = resolver.resolve("advanced-trade-ws.coinbase.com", "443");
        auto ep = net::connect(get_lowest_layer(ws), results);

        // Set SNI hostname
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "advanced-trade-ws.coinbase.com")) {
            throw beast::system_error{beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category())};
        }

        // Perform SSL handshake
        ws.next_layer().handshake(ssl::stream_base::client);
        std::cout << "🔒 SSL handshake completed" << std::endl;

        // Perform WebSocket handshake
        ws.handshake("advanced-trade-ws.coinbase.com", "/");
        std::cout << "🌐 WebSocket handshake completed" << std::endl;

        // Send subscription message
        ws.write(net::buffer(message));
        std::cout << "📤 Subscription message sent!" << std::endl;

        // Read messages
        beast::flat_buffer buffer;
        for (int i = 0; i < 10; ++i) {  // Read 10 messages then exit
            ws.read(buffer);
            std::string response = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());
            
            std::cout << "\n📨 Message " << (i+1) << ": " << response << std::endl;
            
            auto j = nlohmann::json::parse(response, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                std::string channel = j.value("channel", "");
                if (channel == "level2") {
                    std::cout << "📊 Order book update received!" << std::endl;
                } else if (channel == "subscriptions") {
                    std::cout << "✅ Subscription confirmed!" << std::endl;
                } else if (j.value("type", "") == "error") {
                    std::cerr << "❌ Coinbase API Error: " << j.dump(4) << std::endl;
                    break;
                }
            }
        }

        // Close the WebSocket
        ws.close(websocket::close_code::normal);
        std::cout << "\n👋 Connection closed successfully!" << std::endl;

    } catch (std::exception const& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
} 