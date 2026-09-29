#include <gtest/gtest.h>
#include "marketdata/rest/CoinbaseRestClient.hpp"
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
    enum class Reply { StallHandshake, StallRead, Candles, HttpError };
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
    EXPECT_TRUE(accepted); EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    EXPECT_NE(result.error.find("deadline"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("TLS handshake"), std::string::npos) << result.error;
    EXPECT_GE(elapsed, 200ms); EXPECT_LT(elapsed, 2s);
}
TEST_F(CoinbaseRestDeadline, StalledHttpReadReturnsBySameTotalDeadline) {
    serve(Reply::StallRead);
    const auto start = std::chrono::steady_clock::now();
    const auto result = fetch(350ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(receivedRequest); EXPECT_FALSE(result.ok); EXPECT_TRUE(result.candles.empty());
    EXPECT_NE(result.error.find("deadline"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("HTTP read"), std::string::npos) << result.error;
    EXPECT_GE(elapsed, 300ms); EXPECT_LT(elapsed, 2s);
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
} // namespace
