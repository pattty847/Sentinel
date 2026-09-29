#include "protocol/SentinelStreamClient.hpp"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <openssl/pem.h>
#include <condition_variable>
#include <future>

using namespace std::chrono_literals;
namespace beast = boost::beast;

// Real TLS/WebSocket peer, with executor scheduling that deterministically gives
// the old handshake drain a second write kick before the first completion.
struct SentinelStreamClientWriteTest : testing::Test {
    net::io_context ioc;
    ssl::context tls{ssl::context::tls_server};
    tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), 0}};
    net::executor_work_guard<net::io_context::executor_type> work{net::make_work_guard(ioc)};
    QTemporaryDir dir;
    std::string ca;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable received;
    std::vector<std::string> messages;
    std::atomic_bool stallRead{false};
    struct Peer : std::enable_shared_from_this<Peer> {
        beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws;
        beast::flat_buffer buffer;
        const std::string ack = R"({"type":"ack","symbol":"fixture"})";
        SentinelStreamClientWriteTest& owner;
        Peer(SentinelStreamClientWriteTest& o) : ws(o.ioc, o.tls), owner(o) {}
        void read() {
            ws.async_read(buffer, [self = shared_from_this()](beast::error_code ec, size_t) {
                if (ec) return;
                {
                    std::lock_guard lock(self->owner.mutex);
                    self->owner.messages.push_back(beast::buffers_to_string(self->buffer.data()));
                }
                self->owner.received.notify_all();
                self->buffer.consume(self->buffer.size());
                // Exercise simultaneous TLS reads/writes, as the live server
                // sends traffic while processing subscription commands.
                self->ws.async_write(net::buffer(self->ack), [self](beast::error_code ec, size_t) {
                    if (!ec) self->read();
                });
            });
        }
    };
    std::vector<std::shared_ptr<Peer>> peers; // retain stalled peers until teardown
    void accept() {
        auto p = std::make_shared<Peer>(*this);
        acceptor.async_accept(beast::get_lowest_layer(p->ws).socket(), [this, p](beast::error_code ec) {
            if (ec) return;
            peers.push_back(p);
            accept();
            p->ws.next_layer().async_handshake(ssl::stream_base::server, [this, p](beast::error_code ec) {
                if (ec) return;
                p->ws.async_accept([this, p](beast::error_code ec) {
                    if (!ec && !stallRead) p->read();
                });
            });
        });
    }
    void SetUp() override {
        ASSERT_TRUE(dir.isValid());
        ca = dir.path().toStdString() + "/ca.pem";
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gen(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        ASSERT_TRUE(gen);
        ASSERT_GT(EVP_PKEY_keygen_init(gen.get()), 0);
        ASSERT_GT(EVP_PKEY_CTX_set_rsa_keygen_bits(gen.get(), 2048), 0);
        EVP_PKEY* raw = nullptr;
        ASSERT_GT(EVP_PKEY_keygen(gen.get(), &raw), 0);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        ASSERT_TRUE(cert);
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
        X509_set_pubkey(cert.get(), key.get());
        auto* name = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert.get(), name);
        ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
        ASSERT_EQ(SSL_CTX_use_certificate(tls.native_handle(), cert.get()), 1);
        ASSERT_EQ(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()), 1);
        std::unique_ptr<BIO, decltype(&BIO_free)> pem(BIO_new_file(ca.c_str(), "w"), BIO_free);
        ASSERT_TRUE(pem);
        ASSERT_EQ(PEM_write_bio_X509(pem.get(), cert.get()), 1);
        pem.reset();
        accept();
        thread = std::thread([this] { ioc.run(); });
    }
    void TearDown() override {
        ioc.stop();
        if (thread.joinable()) thread.join();
    }
    auto client() { return std::make_unique<SentinelStreamClient>("127.0.0.1",
        std::to_string(acceptor.local_endpoint().port()), ca); }
    bool connected(SentinelStreamClient& c) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!c.m_isConnected && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        return c.m_isConnected;
    }
    bool count(size_t n) {
        std::unique_lock lock(mutex);
        return received.wait_for(lock, 5s, [&] { return messages.size() >= n; });
    }
    void duplicateDrain(SentinelStreamClient& c) {
        // Both kicks are already posted before the enqueue handler can start.
        net::post(c.m_strand, [&c] {
            c.subscribe("BTC-USD");
            net::post(c.m_strand, [&c] { c.doWrite(); });
            c.subscribe("ETH-USD");
        });
    }
    void largePendingWrite(SentinelStreamClient& c, std::promise<void>& started) {
        net::post(c.m_strand, [&c, &started] {
            beast::get_lowest_layer(*c.m_ws).socket().set_option(net::socket_base::send_buffer_size(1024));
            c.m_writeQueue.emplace_back(8 * 1024 * 1024, 'x');
            c.doWrite();
            c.m_writeQueue.emplace_back("must not survive reconnect");
            started.set_value();
        });
    }
    void expectDrained(SentinelStreamClient& c) {
        EXPECT_FALSE(c.m_writeInFlight);
        EXPECT_TRUE(c.m_writeQueue.empty());
        c.m_ioc.restart();
        EXPECT_EQ(c.m_ioc.poll(), 0); // No stale completion can consume a new queue.
    }
    void checkDefensiveCompletions(SentinelStreamClient& c) {
        c.m_writeInFlight = true;
        c.onWrite({}, 0); // Defensive empty-queue path.
        EXPECT_FALSE(c.m_writeInFlight);
        c.m_writeQueue.emplace_back("new payload");
        c.onWrite({}, 0); // Unowned/duplicate completion must not pop a new head.
        ASSERT_EQ(c.m_writeQueue.size(), 1);
        c.m_writeInFlight = true;
        c.m_running = true;
        c.m_isConnected = true;
        c.onWrite(net::error::connection_reset, 0);
        EXPECT_FALSE(c.m_writeInFlight);
        EXPECT_FALSE(c.m_isConnected);
        c.doWrite(); // Error cannot silently retry an ambiguous partial frame.
        EXPECT_FALSE(c.m_writeInFlight);
        c.m_running = false;
    }
};
TEST_F(SentinelStreamClientWriteTest, DuplicateDrainSendsEachMessageExactlyOnce) {
    auto c = client();
    c->connectToServer();
    ASSERT_TRUE(connected(*c));
    duplicateDrain(*c);
    ASSERT_TRUE(count(2));
    c->disconnectFromServer();
    expectDrained(*c);
    std::lock_guard lock(mutex);
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(nlohmann::json::parse(messages[0])["symbol"], "BTC-USD");
    EXPECT_EQ(nlohmann::json::parse(messages[1])["symbol"], "ETH-USD");
}
TEST_F(SentinelStreamClientWriteTest, StopDrainsBorrowedBufferBeforeReconnect) {
    stallRead = true;
    auto c = client();
    c->connectToServer();
    ASSERT_TRUE(connected(*c));
    std::promise<void> started;
    auto ready = started.get_future();
    largePendingWrite(*c, started);
    ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
    c->disconnectFromServer();
    expectDrained(*c);
    stallRead = false;
    c->connectToServer();
    ASSERT_TRUE(connected(*c));
    c->subscribe("BTC-USD");
    ASSERT_TRUE(count(1));
    c->disconnectFromServer();
    expectDrained(*c);
    std::lock_guard lock(mutex);
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(nlohmann::json::parse(messages[0])["symbol"], "BTC-USD");
}
TEST_F(SentinelStreamClientWriteTest, UnexpectedAndFailedCompletionsAreSafe) {
    auto c = client();
    checkDefensiveCompletions(*c);
}
