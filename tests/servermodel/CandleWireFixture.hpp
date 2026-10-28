#pragma once
// Test-only in-process server; no config loading, recorder, capture socket or provider I/O.
#define SENTINEL_CANDLE_TEST
#include "protocol/SentinelStreamServer.cpp"
#include "protocol/SentinelStreamClient.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDateTime>
#include <openssl/pem.h>
#include <future>

using namespace std::chrono_literals;
using Json = nlohmann::json;

struct RecordingServerStopTest {
    static net::io_context &io(SentinelStreamServer &s) { return s.m_ioc; }
    static unsigned short port(SentinelStreamServer &s) { return s.m_acceptor->local_endpoint().port(); }
    static void rest(SentinelStreamServer &s, Authenticator &a, unsigned short port) {
        s.m_restClient = std::make_unique<CoinbaseRestClient>(a, "127.0.0.1", std::to_string(port), "", 200ms);
    }
    static std::vector<std::shared_ptr<Session>> sessions(SentinelStreamServer &s) {
        std::lock_guard lock(s.m_sessionsMutex);
        return {s.m_sessions.begin(), s.m_sessions.end()};
    }
    static size_t pendingModelEvents(const Session &s) { return s.pendingModelEvents_.load(); }
    static size_t pendingWriteBytes(const Session &s) { return s.pendingWriteBytes_.load(); }
    static size_t peakModelEvents(const Session &s) { return s.peakModelEvents_.load(); }
    static size_t peakWriteBytes(const Session &s) { return s.peakWriteBytes_.load(); }
    static bool closing(const Session &s) { return s.closing_.load(); }
    static bool closePosted(const Session &s) { return s.closePosted_.load(); }
    static void park(Session &s, std::promise<void> &entered, std::shared_future<void> release) {
        net::post(s.ws_.get_executor(), [&entered, release] {
            entered.set_value();
            release.wait();
        });
    }
    static void post(Session &s) {
        s.postModelEvent([](Session &) {});
    }
    static void send(Session &s, const Json &j) { s.do_write(j.dump()); }
    static void candle(Session &s, int64_t start) {
        s.postModelEvent([start](Session &session) {
            OHLCVBar bar{};
            bar.timestamp_ms = start;
            bar.open = bar.close = bar.low = bar.high = 100;
            bar.volume = 1;
            session.on_bar_updated("BTC-USD", 100, bar);
        });
    }
    static bool subscribed(Session &s) { return s.subscriptions_.contains("BTC-USD"); }
};

bool writeSelfSignedCert(const std::string &certFile, const std::string &keyFile) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                                                                    EVP_PKEY_CTX_free);
    if (!ctx) return false;
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) return false;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0) return false;
    EVP_PKEY *rawKey = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &rawKey) <= 0) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert) return false;
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    // OpenSSL 4 returns a const subject name, so build the name separately.
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
    if (!subject) return false;
    X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1,
                               -1, 0);
    X509_set_subject_name(cert.get(), subject.get());
    X509_set_issuer_name(cert.get(), subject.get());
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) return false;
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> pemKey(BIO_new_file(keyFile.c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> pemCert(BIO_new_file(certFile.c_str(), "w"), BIO_free);
        if (!pemKey || !pemCert) return false;
        if (PEM_write_bio_PrivateKey(pemKey.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) return false;
        if (PEM_write_bio_X509(pemCert.get(), cert.get()) != 1) return false;
    }
    return true;
}

inline bool spinUntil(const std::function<bool()> &predicate, std::chrono::milliseconds timeout = 3s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    do {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < until);
    return predicate();
}

inline void tradeAt(ServerDataModel &model, int64_t ms, const std::string &symbol = "BTC-USD", double price = 100,
                    double size = 1) {
    Trade trade{};
    trade.product_id = symbol;
    trade.price = price;
    trade.size = size;
    trade.side = AggressorSide::Buy;
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    model.onTrade(trade);
}

struct CandleFixture {
    QTemporaryDir dir{QDir::tempPath() + "/sentinel-candle-XXXXXX"};
    ServerConfig config;
    net::io_context stubIo;
    tcp::socket restStub{stubIo}; // bound but never listening: immediate connection refusal
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    explicit CandleFixture(bool hundred = true, unsigned short port = 0) {
        if (!dir.isValid()) throw std::runtime_error("scratch directory failed");
        config.recording.enabled = false;
        config.rollerShadow.enabled = false;
        config.rollerShadow.products = {"BTC-USD", "ETH-USD", "SOL-USD", "XRP-USD", "DOGE-USD", "ADA-USD", "AVAX-USD"};
        config.chart.timeframesMs =
            hundred ? std::vector<int64_t>{100, 1000, 60000} : std::vector<int64_t>{1000, 60000};
        config.chart.historyBars = {{100, 18000}};
        config.tls.certFile = (dir.path() + "/fixture.crt").toStdString();
        config.tls.keyFile = (dir.path() + "/fixture.key").toStdString();
        if (!writeSelfSignedCert(config.tls.certFile, config.tls.keyFile))
            throw std::runtime_error("scratch certificate failed");
        restStub.open(tcp::v4());
        restStub.bind({net::ip::make_address("127.0.0.1"), 0});
        model = std::make_unique<ServerDataModel>(config);
        auth = std::make_unique<Authenticator>((dir.path() + "/no-credentials").toStdString());
        server = std::make_unique<SentinelStreamServer>(*model, *auth, config, port);
        RecordingServerStopTest::rest(*server, *auth, restStub.local_endpoint().port());
        server->setFeedAdmissionHandler(
            [](const std::string &) { return SentinelStreamServer::FeedAdmission::Accepted; });
    }
    void start() { server->start(); }
    unsigned short port() { return RecordingServerStopTest::port(*server); }
    bool fence() {
        auto done = std::make_shared<std::promise<void>>();
        auto ready = done->get_future();
        net::post(RecordingServerStopTest::io(*server), [done] { done->set_value(); });
        return spinUntil([&] { return ready.wait_for(0ms) == std::future_status::ready; });
    }
    bool subscribed(const std::shared_ptr<Session> &s) {
        auto done = std::make_shared<std::promise<bool>>();
        auto ready = done->get_future();
        net::post(RecordingServerStopTest::io(*server),
                  [s, done] { done->set_value(RecordingServerStopTest::subscribed(*s)); });
        return spinUntil([&] { return ready.wait_for(0ms) == std::future_status::ready; }) && ready.get();
    }
    ~CandleFixture() { server->stop(); }
};

// A real TLS/WebSocket peer with no background reads; tests control reader stalls.
struct StalledCandleClient {
    net::io_context io;
    ssl::context tls{ssl::context::tls_client};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws{io, tls};
    explicit StalledCandleClient(unsigned short port) {
        beast::get_lowest_layer(ws).expires_after(3s);
        beast::get_lowest_layer(ws).connect({net::ip::make_address("127.0.0.1"), port});
        beast::get_lowest_layer(ws).socket().set_option(net::socket_base::receive_buffer_size(1024));
        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake("localhost", "/");
        beast::get_lowest_layer(ws).expires_never();
        beast::flat_buffer b;
        ws.read(b); // hello
        ws.write(net::buffer(std::string(R"({"type":"subscribe","symbol":"BTC-USD"})")));
    }
    ~StalledCandleClient() {
        beast::error_code ec;
        beast::get_lowest_layer(ws).socket().close(ec);
    }
};
