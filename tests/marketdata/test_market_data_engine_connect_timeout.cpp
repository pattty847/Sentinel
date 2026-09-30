// Real BeastWsTransport against local peers: bounded connect phases, bounded
// close, late callbacks from timed-out attempts ignored. Loopback only.
#include <gtest/gtest.h>
#include "marketdata/MarketDataCoreEngine.hpp"
#include "marketdata/ws/BeastWsTransport.hpp"
#include <QTemporaryDir>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/pem.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Engine = MarketDataCoreEngine;
using Kind = Engine::IngestKind;
using ServerWs = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

// Self-signed "localhost" certificate: server context uses it, client trusts it.
struct TestTls {
    ssl::context server{ssl::context::tls_server};
    ssl::context client{ssl::context::tls_client};
    TestTls() {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY_keygen_init(generator.get());
        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048);
        EVP_PKEY* rawKey = nullptr;
        EVP_PKEY_keygen(generator.get(), &rawKey);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
        X509_set_pubkey(cert.get(), key.get());
        auto* subject = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert.get(), subject);
        X509_sign(cert.get(), key.get(), EVP_sha256());
        SSL_CTX_use_certificate(server.native_handle(), cert.get());
        SSL_CTX_use_PrivateKey(server.native_handle(), key.get());
        X509_STORE_add_cert(SSL_CTX_get_cert_store(client.native_handle()), cert.get());
        client.set_verify_mode(ssl::verify_peer);
    }
};

// Loopback peer on its own thread. Keeps every accepted connection open.
class Peer {
public:
    enum class Mode { AcceptTcpOnly, WsSilent, WsHeartbeats };
    Peer(ssl::context& tls, Mode mode) : m_tls(tls), m_mode(mode) {
        accept();
        m_thread = std::thread([this] { m_io.run(); });
    }
    ~Peer() {
        m_guard.reset();
        m_io.stop();
        m_thread.join();
    }
    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }
    tcp::endpoint endpoint() const { return m_acceptor.local_endpoint(); }
    std::vector<Clock::time_point> accepts() const { std::lock_guard lock(m_mutex); return m_accepts; }
    size_t acceptCount() const { std::lock_guard lock(m_mutex); return m_accepts.size(); }

private:
    struct Session {
        explicit Session(tcp::socket socket, ssl::context& tls) : ws(std::move(socket), tls), timer(ws.get_executor()) {}
        ServerWs ws;
        net::steady_timer timer;
        uint64_t sequence = 0;
    };
    void accept() {
        m_acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec) return;
            { std::lock_guard lock(m_mutex); m_accepts.push_back(Clock::now()); }
            auto session = std::make_shared<Session>(std::move(socket), m_tls);
            m_sessions.push_back(session);
            if (m_mode != Mode::AcceptTcpOnly) {
                session->ws.next_layer().async_handshake(ssl::stream_base::server, [this, session](beast::error_code ec) {
                    if (ec) return;
                    session->ws.async_accept([this, session](beast::error_code ec) {
                        // WsSilent never reads again, so a client close frame is never answered.
                        if (!ec && m_mode == Mode::WsHeartbeats) heartbeat(session);
                    });
                });
            }
            accept();
        });
    }
    void heartbeat(std::shared_ptr<Session> session) {
        auto frame = std::make_shared<std::string>(nlohmann::json({{"channel", "heartbeats"},
            {"sequence_num", session->sequence++}, {"events", nlohmann::json::array()}}).dump());
        session->ws.async_write(net::buffer(*frame), [this, session, frame](beast::error_code ec, size_t) {
            if (ec) return;
            session->timer.expires_after(20ms);
            session->timer.async_wait([this, session](beast::error_code ec) { if (!ec) heartbeat(session); });
        });
    }

    ssl::context& m_tls;
    Mode m_mode;
    net::io_context m_io;
    std::optional<net::executor_work_guard<net::io_context::executor_type>> m_guard{net::make_work_guard(m_io)};
    tcp::acceptor m_acceptor{m_io, {net::ip::make_address("127.0.0.1"), 0}};
    std::vector<std::shared_ptr<Session>> m_sessions;
    mutable std::mutex m_mutex;
    std::vector<Clock::time_point> m_accepts;
    std::thread m_thread;
};

// Resolver that never answers on its own; the test can answer a stored call late.
struct HangingResolver {
    std::mutex mutex;
    std::vector<std::pair<Clock::time_point, BeastWsTransport::ResolveHandler>> calls;
    BeastWsTransport::ResolveFn fn() {
        return [this](const std::string&, const std::string&, BeastWsTransport::ResolveHandler handler) {
            std::lock_guard lock(mutex);
            calls.emplace_back(Clock::now(), std::move(handler));
        };
    }
    size_t count() { std::lock_guard lock(mutex); return calls.size(); }
};

struct Observed {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Clock::time_point> ups, downs;
    std::vector<std::string> errors;
    template<class P> bool wait(P predicate, std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return predicate(*this); });
    }
    bool anyError(const std::string& needle) {
        std::lock_guard lock(mutex);
        return std::any_of(errors.begin(), errors.end(), [&](auto& e) { return e.find(needle) != std::string::npos; });
    }
};

struct EngineConnectTimeout : testing::Test {
    Authenticator auth{"/nonexistent-sentinel-test-credentials"};
    TestTls tls;
    Observed observed;
    BeastWsTransport::Options options{150ms, 200ms, {}};
    Engine::ReconnectPolicy policy{50ms, 100ms, 10ms, 2s, 50ms};
    std::unique_ptr<Engine> engine;

    void start(const std::string& port) {
        ServerMdcConfig config;
        config.host = "localhost";
        config.port = port;
        config.sslCaBundle = SENTINEL_TEST_CA;
        engine = std::make_unique<Engine>(auth, config, [this](net::io_context& io, ssl::context&) {
            return std::make_unique<BeastWsTransport>(io, tls.client, options);
        }, policy);
        engine->onIngest([this](const Engine::IngestObservation& event) {
            if (event.kind != Kind::TransportUp && event.kind != Kind::TransportDown) return;
            std::lock_guard lock(observed.mutex);
            (event.kind == Kind::TransportUp ? observed.ups : observed.downs).push_back(Clock::now());
            observed.changed.notify_all();
        });
        engine->onError([this](const std::string& error) {
            std::lock_guard lock(observed.mutex);
            observed.errors.push_back(error);
            observed.changed.notify_all();
        });
        engine->start();
    }
    void TearDown() override { if (engine) engine->stop(); }
};

// Gaps between attempts: at least timeout + backoff (no storm), and bounded.
void expectPaced(const std::vector<Clock::time_point>& attempts, std::chrono::milliseconds timeout,
                 std::chrono::milliseconds minBackoff, std::chrono::milliseconds maxBackoff) {
    for (size_t i = 1; i < attempts.size(); ++i) {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(attempts[i] - attempts[i - 1]);
        EXPECT_GE(gap, timeout + minBackoff - 5ms) << "attempt " << i;
        EXPECT_LT(gap, timeout + maxBackoff + 1500ms) << "attempt " << i;
    }
}

TEST_F(EngineConnectTimeout, TcpAcceptedButTlsNeverCompletesTimesOutIntoBackoff) {
    Peer peer(tls.server, Peer::Mode::AcceptTcpOnly);
    start(peer.port());
    ASSERT_TRUE(observed.wait([&](auto&) { return peer.acceptCount() >= 4; }));
    engine->stop();
    const auto accepts = peer.accepts();
    expectPaced(accepts, options.connectTimeout, policy.initialDelay, policy.maximumDelay);
    std::lock_guard lock(observed.mutex);
    EXPECT_TRUE(observed.ups.empty());
    // One down per timed-out attempt, never a duplicate burst.
    EXPECT_GE(observed.downs.size(), accepts.size() - 1);
    EXPECT_LE(observed.downs.size(), accepts.size());
    EXPECT_TRUE(std::any_of(observed.errors.begin(), observed.errors.end(),
        [](auto& e) { return e.find("connect timed out in tls-handshake") != std::string::npos; }));
}

TEST_F(EngineConnectTimeout, HungResolveTimesOutAndLateAnswerIsIgnored) {
    Peer peer(tls.server, Peer::Mode::WsHeartbeats);
    HangingResolver resolver;
    options.resolve = resolver.fn();
    options.connectTimeout = 100ms;
    start(peer.port());
    ASSERT_TRUE(observed.wait([&](auto&) { return resolver.count() >= 3; }));
    EXPECT_TRUE(observed.anyError("connect timed out in resolve"));
    // Answer the first (long timed-out) attempt with the live peer's address.
    net::io_context local;
    tcp::resolver numeric(local);
    const auto results = numeric.resolve("127.0.0.1", peer.port(), tcp::resolver::numeric_host);
    BeastWsTransport::ResolveHandler late;
    { std::lock_guard lock(resolver.mutex); late = resolver.calls.front().second; }
    late({}, results);
    EXPECT_FALSE(observed.wait([](auto& o) { return !o.ups.empty(); }, 400ms));
    EXPECT_EQ(peer.acceptCount(), 0u) << "stale resolve must not open a connection";
    ASSERT_TRUE(observed.wait([&](auto&) { return resolver.count() >= 5; }));
    engine->stop();
    std::vector<Clock::time_point> calls;
    { std::lock_guard lock(resolver.mutex); for (auto& c : resolver.calls) calls.push_back(c.first); }
    expectPaced(calls, options.connectTimeout, policy.initialDelay, policy.maximumDelay);
}

TEST_F(EngineConnectTimeout, StaleHeartbeatCloseToSilentPeerIsBoundedThenReconnects) {
    Peer peer(tls.server, Peer::Mode::WsSilent);
    policy.heartbeatStale = 150ms;
    start(peer.port());
    ASSERT_TRUE(observed.wait([](auto& o) { return o.ups.size() >= 2; }));
    engine->stop();
    std::lock_guard lock(observed.mutex);
    ASSERT_FALSE(observed.downs.empty());
    const auto accepts = peer.accepts();
    ASSERT_GE(accepts.size(), 2u);
    // stale detection + bounded close + stale backoff, far below Beast's 30 s close timeout.
    const auto upToDown = observed.downs.front() - observed.ups.front();
    EXPECT_GE(upToDown, policy.heartbeatStale + options.closeTimeout - 5ms);
    EXPECT_LT(upToDown, policy.heartbeatStale + options.closeTimeout + 1500ms);
    EXPECT_LT(accepts[1] - observed.downs.front(), policy.staleHeartbeatDelay + 1500ms);
    EXPECT_TRUE(std::any_of(observed.errors.begin(), observed.errors.end(),
        [](auto& e) { return e.find("timed out") != std::string::npos; }));
}

TEST_F(EngineConnectTimeout, HealthyConnectionOutlivesConnectDeadlineWithoutReconnect) {
    Peer peer(tls.server, Peer::Mode::WsHeartbeats);
    policy.heartbeatStale = 150ms;
    start(peer.port());
    ASSERT_TRUE(observed.wait([](auto& o) { return !o.ups.empty(); }));
    // Several connect and close deadlines pass while connected.
    EXPECT_FALSE(observed.wait([](auto& o) { return !o.downs.empty(); }, 5 * options.connectTimeout));
    engine->stop();
    EXPECT_EQ(peer.acceptCount(), 1u);
    std::lock_guard lock(observed.mutex);
    EXPECT_EQ(observed.ups.size(), 1u);
    EXPECT_TRUE(observed.downs.empty());
}
} // namespace
