#include <gtest/gtest.h>
#include "marketdata/rest/CoinbaseRestClient.hpp"
#include "marketdata/rest/RestResolver.hpp"
#include <QTemporaryDir>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/pem.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

class CoinbaseRestDeadline : public testing::Test {
protected:
    enum class Reply { StallHandshake, StallRead, Candles, HttpError, Product, WrongProduct, BadProduct };
    QTemporaryDir directory;
    Authenticator auth{"/nonexistent-sentinel-test-credentials"};
    net::io_context ioc;
    ssl::context tls{ssl::context::tls_server};
    tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), 0}};
    beast::ssl_stream<beast::tcp_stream> peer{ioc, tls};
    net::executor_work_guard<net::io_context::executor_type> guard{net::make_work_guard(ioc)};
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::response<http::string_body> response;
    std::thread thread;
    std::atomic_bool accepted{false}, receivedRequest{false};
    std::string caFile;

    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        caFile = directory.path().toStdString() + "/ca.pem";
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        ASSERT_TRUE(generator);
        ASSERT_GT(EVP_PKEY_keygen_init(generator.get()), 0);
        ASSERT_GT(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048), 0);
        EVP_PKEY* rawKey = nullptr;
        ASSERT_GT(EVP_PKEY_keygen(generator.get(), &rawKey), 0);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_TRUE(cert);
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
        X509_set_pubkey(cert.get(), key.get());
        auto* subject = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert.get(), subject);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(tls.native_handle(), cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()), 1);
        // peer was constructed before the context's certificate was installed.
        ASSERT_EQ(SSL_use_certificate(peer.native_handle(), cert.get()), 1);
        ASSERT_EQ(SSL_use_PrivateKey(peer.native_handle(), key.get()), 1);
        std::unique_ptr<BIO, decltype(&BIO_free)> pem(BIO_new_file(caFile.c_str(), "w"), BIO_free);
        ASSERT_TRUE(pem);
        ASSERT_EQ(PEM_write_bio_X509(pem.get(), cert.get()), 1);
    }
    void serve(Reply reply) {
        acceptor.async_accept(peer.next_layer().socket(), [this, reply](beast::error_code ec) {
            if (ec) return;
            accepted = true;
            if (reply == Reply::StallHandshake) return;
            peer.async_handshake(ssl::stream_base::server, [this, reply](beast::error_code ec) {
                if (ec) return;
                http::async_read(peer, buffer, request, [this, reply](beast::error_code ec, size_t) {
                    if (ec) return;
                    receivedRequest = true;
                    if (reply == Reply::StallRead) return;
                    response.version(11);
                    response.result(reply == Reply::HttpError ? http::status::service_unavailable : http::status::ok);
                    response.body() = reply == Reply::HttpError ? "fixture unavailable" :
                        R"({"candles":[{"start":"60","open":"10","high":"12","low":"9","close":"11","volume":"3"}]})";
                    if (reply == Reply::Product || reply == Reply::WrongProduct || reply == Reply::BadProduct) {
                        nlohmann::json product = {{"product_id", reply == Reply::WrongProduct ? "ETH-USD" : "BTC-USD"},
                            {"quote_increment", "0.0100"}, {"base_increment", "0.00000001"}, {"status", "online"}};
                        if (reply == Reply::BadProduct) product["base_increment"] = 0.00000001;
                        response.body() = product.dump();
                    }
                    response.prepare_payload();
                    http::async_write(peer, response, [](beast::error_code, size_t) {});
                    // Intentionally never send close_notify, even after success.
                });
            });
        });
        thread = std::thread([this] { ioc.run(); });
    }
    CandleFetchResult fetch(std::chrono::milliseconds timeout) {
        CoinbaseRestClient client(auth, "127.0.0.1", std::to_string(acceptor.local_endpoint().port()), caFile, timeout);
        return client.fetchProductCandles("BTC-USD", 60, 120, "ONE_MINUTE", 1);
    }
    void TearDown() override {
        ioc.stop();
        if (thread.joinable()) thread.join();
    }
};

TEST_F(CoinbaseRestDeadline, StalledTlsHandshakeReturnsByTotalDeadline) {
    serve(Reply::StallHandshake);
    const auto start = std::chrono::steady_clock::now();
    const auto result = fetch(250ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    EXPECT_NE(result.error.find("deadline"), std::string::npos) << result.error;
    EXPECT_LT(elapsed, 250ms + 2s);
}
TEST_F(CoinbaseRestDeadline, StalledHttpReadReturnsBySameTotalDeadline) {
    serve(Reply::StallRead);
    const auto start = std::chrono::steady_clock::now();
    const auto result = fetch(350ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    EXPECT_NE(result.error.find("deadline"), std::string::npos) << result.error;
    EXPECT_LT(elapsed, 350ms + 2s);
}
TEST_F(CoinbaseRestDeadline, ParsesCandlesWithoutWaitingForTlsShutdown) {
    serve(Reply::Candles);
    const auto start = std::chrono::steady_clock::now();
    const auto result = fetch(2s);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.candles.size(), 1);
    EXPECT_EQ(result.candles[0].timestamp_ms, 60000);
    EXPECT_DOUBLE_EQ(result.candles[0].high, 12);
    EXPECT_DOUBLE_EQ(result.candles[0].volume, 3);
    EXPECT_TRUE(request.target().starts_with("/api/v3/brokerage/market/products/BTC-USD/candles?"));
}
TEST_F(CoinbaseRestDeadline, HttpFailureKeepsErrorContractWithoutWaitingForTlsShutdown) {
    serve(Reply::HttpError);
    const auto result = fetch(1s);
    EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    EXPECT_NE(result.error.find("HTTP 503"), std::string::npos);
}

TEST_F(CoinbaseRestDeadline, ProductMetadataRetainsExactIncrementStringsAndSource) {
    serve(Reply::Product);
    CoinbaseRestClient client(auth, "127.0.0.1", std::to_string(acceptor.local_endpoint().port()), caFile, 2s);
    const auto result = client.fetchProductMetadata("BTC-USD");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.quoteIncrement, "0.0100");
    EXPECT_EQ(result.baseIncrement, "0.00000001");
    EXPECT_EQ(result.metadata["status"], "online");
    EXPECT_EQ(request.target(), "/api/v3/brokerage/market/products/BTC-USD");
    EXPECT_EQ(result.sourcePath, request.target());
}
TEST_F(CoinbaseRestDeadline, ProductMetadataRejectsDifferentProduct) {
    serve(Reply::WrongProduct);
    CoinbaseRestClient client(auth, "127.0.0.1", std::to_string(acceptor.local_endpoint().port()), caFile, 2s);
    const auto result = client.fetchProductMetadata("BTC-USD");
    EXPECT_FALSE(result.ok); EXPECT_EQ(result.error, "product_id mismatch");
}
TEST_F(CoinbaseRestDeadline, ProductMetadataRequiresExactDecimalStrings) {
    serve(Reply::BadProduct);
    CoinbaseRestClient client(auth, "127.0.0.1", std::to_string(acceptor.local_endpoint().port()), caFile, 2s);
    EXPECT_FALSE(client.fetchProductMetadata("BTC-USD").ok);
}
TEST_F(CoinbaseRestDeadline, ProductMetadataUsesTheExistingTotalDeadline) {
    serve(Reply::StallRead);
    CoinbaseRestClient client(auth, "127.0.0.1", std::to_string(acceptor.local_endpoint().port()), caFile, 250ms);
    const auto start = std::chrono::steady_clock::now();
    const auto result = client.fetchProductMetadata("BTC-USD");
    EXPECT_FALSE(result.ok); EXPECT_NE(result.error.find("deadline"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 250ms + 2s);
}

TEST(RestResolver, FreshCacheAvoidsRepeatedLookups) {
    using Resolver = sentinel::rest::Resolver;
    auto calls = std::make_shared<std::atomic_int>(0);
    const Resolver::Endpoints endpoints{{net::ip::make_address("127.0.0.1"), 443}};
    Resolver resolver([calls, endpoints](auto&, auto&) { ++*calls; return endpoints; });
    EXPECT_EQ(resolver.resolve("coinbase.test", "443", Resolver::Clock::now() + 2s), endpoints);
    EXPECT_EQ(resolver.resolve("coinbase.test", "443", Resolver::Clock::now() + 2s), endpoints);
    EXPECT_EQ(calls->load(), 1);
}
TEST(RestResolver, StalledRefreshSharesLookupKeepsCachedHostAndWaitsForCapacity) {
    using Resolver = sentinel::rest::Resolver;
    struct Gates {
        std::mutex mutex;
        std::condition_variable entered;
        int count = 0;
        std::atomic_int calls{0};
        std::promise<void> release;
        std::shared_future<void> gate = release.get_future().share();
    };
    auto gates = std::make_shared<Gates>();
    struct Release { std::shared_ptr<Gates> gates; ~Release() { if (gates) gates->release.set_value(); } } release{gates};
    const Resolver::Endpoints endpoints{{net::ip::make_address("127.0.0.1"), 443}};
    Resolver resolver([gates, endpoints](auto&, auto&) {
        if (++gates->calls != 1) {
            { std::lock_guard lock(gates->mutex); ++gates->count; }
            gates->entered.notify_all();
            gates->gate.wait();
        }
        return endpoints;
    }, 0ms); // Force refresh after priming a usable endpoint.
    ASSERT_EQ(resolver.resolve("coinbase.test", "443", Resolver::Clock::now() + 2s), endpoints);
    ASSERT_EQ(resolver.resolve("coinbase.test", "443", Resolver::Clock::now() + 2s), endpoints);
    std::vector<std::future<void>> callers;
    for (int i = 0; i < 3; ++i) callers.push_back(std::async(std::launch::async, [&, i] {
        try { resolver.resolve("other" + std::to_string(i), "443", Resolver::Clock::now() + 100ms); }
        catch (const std::exception&) {}
    }));
    {
        std::unique_lock lock(gates->mutex);
        ASSERT_TRUE(gates->entered.wait_for(lock, 2s, [&] { return gates->count == 4; }));
    }
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ(resolver.resolve("coinbase.test", "443", Resolver::Clock::now() + 100ms), endpoints);
    EXPECT_EQ(gates->calls.load(), 5); // one prime + four stalls; no repeated same-host jobs
    EXPECT_THROW(resolver.resolve("uncached", "443", Resolver::Clock::now() + 20ms), std::runtime_error);
    auto waiting = std::async(std::launch::async, [&] {
        return resolver.resolve("uncached", "443", Resolver::Clock::now() + 2s);
    });
    EXPECT_EQ(waiting.wait_for(20ms), std::future_status::timeout);
    gates->release.set_value(); release.gates.reset();
    EXPECT_EQ(waiting.get(), endpoints); // releasing a slot recovers without a client restart
    for (auto& caller : callers) caller.get();
}
} // namespace
