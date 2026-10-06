// Compile the private Session implementation here to exercise its actual queue
// and close path without exposing transport internals in a production header.
#include "protocol/SentinelStreamServer.cpp"
#include "marketdata/auth/Authenticator.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QThread>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <gtest/gtest.h>
#include <future>
#include <boost/asio/read.hpp>
#include <openssl/pem.h>

// Drive the actual stop path without starting sockets or needing TLS credentials.
struct RecordingServerStopTest {
    static bool listening(SentinelStreamServer& server) {
        if (!server.m_running) return false;
        auto result = std::make_shared<std::promise<bool>>();
        net::post(server.m_ioc, [&server, result] { result->set_value(server.m_acceptor && server.m_acceptor->is_open()); });
        auto ready = result->get_future();
        return ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready && ready.get();
    }
    // 0 when the listener is closed or the I/O thread does not answer; never hangs.
    static unsigned short port(SentinelStreamServer& server) {
        auto result = std::make_shared<std::promise<unsigned short>>();
        net::post(server.m_ioc, [&server, result] {
            beast::error_code ec;
            const auto endpoint = server.m_acceptor ? server.m_acceptor->local_endpoint(ec) : tcp::endpoint{};
            result->set_value(ec ? 0 : endpoint.port());
        });
        auto ready = result->get_future();
        return ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready ? ready.get() : 0;
    }
    static size_t sessionCount(SentinelStreamServer& server) {
        std::lock_guard lock(server.m_sessionsMutex);
        return server.m_sessions.size();
    }
    // A raw TCP client reaches registerSession(), i.e. the server really accepted it.
    static bool accepts(SentinelStreamServer& server, unsigned short port) {
        net::io_context ioc;
        tcp::socket socket(ioc);
        beast::error_code ec;
        socket.connect({net::ip::make_address("127.0.0.1"), port}, ec);
        if (ec) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (sessionCount(server) == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const bool accepted = sessionCount(server) > 0;
        socket.set_option(net::socket_base::linger(true, 0), ec); // no TIME_WAIT left behind
        socket.close(ec);
        return accepted;
    }
    // Wait until stop() has stopped the I/O context (its last step before the join).
    static bool waitIoStopped(SentinelStreamServer& server) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.m_ioc.stopped() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return server.m_ioc.stopped();
    }
    static net::io_context& ioContext(SentinelStreamServer& server) { return server.m_ioc; }
    // async_accept calls so far, read on the I/O thread; -1 when it does not answer.
    static int64_t acceptAttempts(SentinelStreamServer& server) {
        auto result = std::make_shared<std::promise<int64_t>>();
        net::post(server.m_ioc, [&server, result] { result->set_value(static_cast<int64_t>(server.m_acceptAttempts)); });
        auto ready = result->get_future();
        return ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready ? ready.get() : -1;
    }
    // Runs on the I/O thread after an accept passed its running check, before admission.
    static void beforeSessionAdmit(SentinelStreamServer& server, std::function<void()> hook) {
        server.m_beforeSessionAdmitForTest = std::move(hook);
    }
    // Close the current listener on the I/O thread, as an external accept failure would.
    static bool closeListener(SentinelStreamServer& server) {
        auto result = std::make_shared<std::promise<void>>();
        net::post(server.m_ioc, [&server, result] {
            beast::error_code ignored;
            server.m_acceptor->close(ignored);
            result->set_value();
        });
        return result->get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    }
    static void checkRecordingQueue(ServerDataModel& model) {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tls_server);
        auto session = std::make_shared<Session>(tcp::socket(ioc), ctx, model, nullptr);
        session->write_queue_.push_back({"in-flight", false});
        session->write_queue_.push_back({"history-1", false});
        session->write_queue_.push_back({"history-2", false});
        session->pendingWriteBytes_ = 27;
        const auto* buffer = session->write_queue_.front().payload.data();
        const auto slot = session->recordingWriteSlot_;
        ASSERT_TRUE(slot->tryAcquire());
        session->onRecordingWritePost("live");
        EXPECT_EQ(session->write_queue_.front().payload.data(), buffer);
        std::vector<std::string> order;
        for (const auto& item : session->write_queue_) order.push_back(item.payload);
        EXPECT_EQ(order, (std::vector<std::string>{"in-flight", "live", "history-1", "history-2"}));
        EXPECT_FALSE(slot->tryAcquire());
        session->beginClose("recording queue test");
        ASSERT_EQ(session->write_queue_.size(), 1);
        EXPECT_EQ(session->write_queue_.front().payload.data(), buffer);
        EXPECT_EQ(session->pendingWriteBytes_.load(), 9);
        ASSERT_TRUE(slot->tryAcquire()); // queued live was released by beginClose
        session->onRecordingWritePost("late post"); // worker post racing with close
        EXPECT_EQ(session->write_queue_.size(), 1);
        ASSERT_TRUE(slot->tryAcquire());
        slot->release();
        session->on_write_complete(net::error::operation_aborted, 0);
        EXPECT_TRUE(session->write_queue_.empty());
        EXPECT_EQ(session->pendingWriteBytes_.load(), 0);
    }
    static void checkOverlayBounds(SentinelStreamServer& server, ServerDataModel& model) {
        server.m_running = true;
        server.m_historyWorkers = std::make_unique<net::thread_pool>(2);
        std::promise<void> release, entered1, entered2;
        auto gate = release.get_future().share();
        EXPECT_TRUE(server.submitHistoryTask([&] { entered1.set_value(); gate.wait(); }));
        EXPECT_TRUE(server.submitHistoryTask([&] { entered2.set_value(); gate.wait(); }));
        EXPECT_EQ(entered1.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(entered2.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
        net::io_context ioc;
        ssl::context ctx(ssl::context::tls_server);
        auto session = std::make_shared<Session>(tcp::socket(ioc), ctx, model, &server);
        session->subscriptions_.insert("BTC-USD");
        session->overlayState("BTC-USD").request.grid.maxPrice = 120000;
        const auto oldSelectionStopped = session->overlayStopRequested("BTC-USD");
        EXPECT_FALSE(oldSelectionStopped());
        session->requestOverlayHistory({{"symbol", "BTC-USD"}, {"timeframe_ms", 300000}, {"count", 2}}, false);
        EXPECT_TRUE(oldSelectionStopped());
        const auto currentSelectionStopped = session->overlayStopRequested("BTC-USD");
        EXPECT_FALSE(currentSelectionStopped());
        server.m_running = false;
        EXPECT_TRUE(currentSelectionStopped()); // shutdown is visible before executor drain
        server.m_running = true;
        session->overlayHistory_.clear();
        session->write_queue_.push_back({"in-flight", false});
        session->pendingWriteBytes_ = 9;
        session->pumpOverlays();
        EXPECT_TRUE(session->overlayBusy_);
        EXPECT_EQ(server.m_pendingHistoryTasks.load(), 3);
        // A stalled worker never accumulates forming refreshes.
        for (int i = 0; i < 100; ++i) session->pumpOverlays();
        EXPECT_EQ(server.m_pendingHistoryTasks.load(), 3);
        nlohmann::json request = {{"symbol", "BTC-USD"}, {"timeframe_ms", 60000}, {"count", 2}};
        for (int i = 0; i < 9; ++i) session->requestOverlayHistory(request, false);
        EXPECT_EQ(session->overlayHistory_.size(), 8);
        for (int i = 0; i < 5; ++i) EXPECT_TRUE(server.submitHistoryTask([] {}));
        EXPECT_FALSE(server.submitHistoryTask([] {})); // global admission cap is eight
        session->armOverlayTimer();
        session->beginClose("overlay cancellation test");
        EXPECT_TRUE(currentSelectionStopped());
        EXPECT_TRUE(session->overlayHistory_.empty());
        release.set_value();
        server.m_historyWorkers->join(); // force the queued completion to race with the closed session
        server.stop(); // joins scans/builds before owner/model destruction
        ioc.run(); // late completion and cancelled timer must not publish
        EXPECT_EQ(session->write_queue_.size(), 1);
    }
    static void checkCandlePages(ServerDataModel& model) {
        net::io_context ioc;
        ssl::context ctx(ssl::context::tls_server);
        auto session = std::make_shared<Session>(tcp::socket(ioc), ctx, model, nullptr);
        // Hold a fake write in flight so posted replies stay inspectable without a socket.
        session->write_queue_.push_back({"in-flight", false});
        session->pendingWriteBytes_ = 9;
        const auto fetch = [&](const char* symbol, int endSec, int limit) {
            nlohmann::json request = {{"type", "candle_history_request"}, {"symbol", symbol},
                {"timeframe_sec", 1}, {"end_time_sec", endSec}, {"limit", limit}};
            session->handle_message(request.dump());
            ioc.restart();
            ioc.poll();
            return nlohmann::json::parse(session->write_queue_.back().payload);
        };
        // Client walks backward with oldestStart - 1; the server end remains inclusive.
        for (const auto [endSec, expectedCount] : {std::pair{1000, 350}, {650, 350}, {300, 300}, {1, 1}}) {
            const auto reply = fetch("BTC-USD", endSec, 350);
            ASSERT_EQ(reply.at("type"), "candle_history_chunk");
            EXPECT_EQ(reply.at("end_time_sec"), endSec);
            const auto& candles = reply.at("candles");
            ASSERT_EQ(candles.size(), expectedCount);
            EXPECT_EQ(candles.back().at("time_start_ms"), endSec * 1000LL);
            for (size_t i = 1; i < candles.size(); ++i)
                EXPECT_EQ(candles[i].at("time_start_ms").get<int64_t>(),
                          candles[i - 1].at("time_start_ms").get<int64_t>() + 1000);
        }
        const auto sparse = fetch("ETH-USD", 20, 2);
        const auto& candles = sparse.at("candles");
        ASSERT_EQ(candles.size(), 2);
        EXPECT_EQ(candles[0].at("time_start_ms"), 10'000);
        EXPECT_EQ(candles[1].at("time_start_ms"), 20'000);
        EXPECT_EQ(sparse.at("start_time_sec"), 18); // metadata, not a sparse-bar lower bound
        EXPECT_TRUE(fetch("UNKNOWN-USD", 20, 2).at("candles").empty());
        session->beginClose("candle paging test");
    }
    // Longer than the 3 s wait for the ClientHello below, so the fetch is
    // still blocked in TLS when stop() runs.
    static constexpr auto kStalledFetchDeadline = std::chrono::seconds(5);
    static std::weak_ptr<Session> startCandleFetch(SentinelStreamServer& server, ServerDataModel& model,
                                                  Authenticator& auth, unsigned short port) {
        server.m_restClient = std::make_unique<CoinbaseRestClient>(auth, "127.0.0.1", std::to_string(port),
                                                                 "", kStalledFetchDeadline);
        server.m_historyWorkers = std::make_unique<net::thread_pool>(2);
        startExecutor(server);
        std::promise<std::weak_ptr<Session>> created;
        auto future = created.get_future();
        net::post(server.m_ioc, [&] {
            auto session = std::make_shared<Session>(tcp::socket(server.m_ioc), server.m_sslCtx, model, &server);
            server.registerSession(session);
            session->handle_message(R"({"type":"candle_history_request","symbol":"BTC-USD","timeframe_sec":60,"end_time_sec":172860,"limit":1})");
            created.set_value(session);
        });
        return future.get();
    }
    static boost::asio::io_context& startExecutor(SentinelStreamServer& server) {
        server.m_running = true;
        server.m_thread = std::thread([&server, guard = boost::asio::make_work_guard(server.m_ioc)] {
            server.m_ioc.run();
        });
        return server.m_ioc;
    }
};
// An ephemeral self-signed fixture avoids external openssl commands/certs.
bool writeSelfSignedCert(const std::string& certFile, const std::string& keyFile) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!ctx) return false;
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) return false;
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0) return false;
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &rawKey) <= 0) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert) return false;
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
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

TEST(RecordingServerStop, ActiveDeliveryIsJoinedBeforeDelayedExecutorDrain) {
    int argc = 1;
    char name[] = "recording-stop";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ServerConfig config;
    config.recording.enabled = true;
    config.recording.dir = dir.path().toStdString();
    config.heatmap.persistenceEnabled = false;
    ServerDataModel model(config);
    ASSERT_NE(model.recordingLive(), nullptr);
    Authenticator auth(dir.path().toStdString() + "/no-credentials");
    SentinelStreamServer server(model, auth, config, 0);
    auto& executor = RecordingServerStopTest::startExecutor(server);
    std::promise<void> executorEntered, executorRelease, deliveryEntered, deliveryRelease;
    auto releaseExecutor = executorRelease.get_future().share();
    auto releaseDelivery = deliveryRelease.get_future().share();
    boost::asio::post(executor, [&] { executorEntered.set_value(); releaseExecutor.wait(); });
    ASSERT_EQ(executorEntered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    recording::LiveView view{"BTC-USD", "near", 60000, {90, 1, 20}, 1};
    std::atomic_bool postedWhileAlive{false};
    auto subscription = model.recordingLive()->subscribe(view, [&](const auto&, const auto&) {
        deliveryEntered.set_value();
        releaseDelivery.wait();
        postedWhileAlive = !executor.stopped();
        boost::asio::post(executor, [] {});
        return true;
    });
    auto record = std::make_shared<recording::Hmc2Record>();
    record->header.symbol = "BTC-USD"; record->header.layer = "near";
    record->bucketStartMs = record->committedThroughMs = recording::kHmc2MinMs;
    record->observedMs = 1000; record->flags = recording::kProvisional;
    record->bidRowLo = record->askRowLo = 90; record->bidRowHi = record->askRowHi = 110;
    model.recordingLive()->publish(record);
    auto entered = deliveryEntered.get_future();
    EXPECT_EQ(entered.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    auto stopped = std::async(std::launch::async, [&] { server.stop(); });
    EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    EXPECT_FALSE(executor.stopped());
    EXPECT_FALSE(subscription->active.load());
    deliveryRelease.set_value();
    // Keep the executor blocked beyond the old two-second drain timeout. Live
    // is joined and cannot post again while the server is waiting to join I/O.
    EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(2200)), std::future_status::timeout);
    EXPECT_TRUE(postedWhileAlive.load());
    EXPECT_FALSE(model.recordingLive()->subscribe(view, [](const auto&, const auto&) { return true; }));
    executorRelease.set_value();
    EXPECT_EQ(stopped.wait_for(std::chrono::seconds(3)), std::future_status::ready);
}

TEST(RecordingServerStop, FailedRecorderStartReleasesItsLiveService) {
    int argc = 1;
    char name[] = "recording-failure";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    recording::Hmc2Store locked(dir.path().toStdString());
    ServerConfig config;
    config.recording.enabled = true;
    config.recording.dir = dir.path().toStdString();
    config.heatmap.persistenceEnabled = false;
    ServerDataModel model(config);
    EXPECT_FALSE(model.recordingAvailable());
    EXPECT_EQ(model.recordingLive(), nullptr);
}

// Production handoff: a recorder self-invalidation (here a one-sided snapshot)
// reaches recordingResnapshotRequested on the model's (main) thread, which
// SentinelServerApp connects to MarketDataCoreEngine::requestResnapshot.
TEST(RecordingServerStop, SelfInvalidationIsHandedToTheMainThread) {
    int argc = 1;
    char name[] = "recording-resnapshot";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ServerConfig config;
    config.recording.enabled = true;
    config.recording.dir = dir.path().toStdString();
    config.heatmap.persistenceEnabled = false;
    ServerDataModel model(config);
    ASSERT_TRUE(model.recordingAvailable());
    std::vector<std::pair<QString, QString>> requests;
    bool onMainThread = false;
    QObject::connect(&model, &ServerDataModel::recordingResnapshotRequested,
                     [&](const QString& symbol, const QString& reason) {
                         requests.emplace_back(symbol, reason);
                         onMainThread = QThread::currentThread() == app.thread();
                     });
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    model.onLiveOrderBookInitialized("BTC-USD", {{100.0, 2.0}}, {}, nowMs);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (requests.empty() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].first, "BTC-USD");
    EXPECT_EQ(requests[0].second, "missing two-sided mid");
    EXPECT_TRUE(onMainThread);
}

TEST(RecordingServerStop, SessionPrioritizesLiveAndReleasesSlotOnBeginClose) {
    int argc = 1;
    char name[] = "recording-session";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerConfig config;
    config.heatmap.persistenceEnabled = false;
    config.recording.enabled = false;
    ServerDataModel model(config);
    RecordingServerStopTest::checkRecordingQueue(model);
}

TEST(RecordingServerStop, ActualServerStartStopStartRestoresLiveDelivery) {
    int argc = 1;
    char name[] = "recording-restart";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ServerConfig config;
    config.heatmap.persistenceEnabled = false;
    config.recording.enabled = true;
    config.recording.dir = dir.path().toStdString();
    config.tls.certFile = dir.path().toStdString() + "/cert.pem";
    config.tls.keyFile = dir.path().toStdString() + "/key.pem";
    ASSERT_TRUE(writeSelfSignedCert(config.tls.certFile, config.tls.keyFile));
    ServerDataModel model(config);
    Authenticator auth(dir.path().toStdString() + "/no-credentials");
    SentinelStreamServer server(model, auth, config, 0);
    for (int cycle = 0; cycle < 2; ++cycle) {
        server.start();
        EXPECT_TRUE(RecordingServerStopTest::listening(server));
        std::promise<uint64_t> delivered;
        auto sub = model.recordingLive()->subscribe({"BTC-USD", "near", 60000, {90, 1, 20}, 1},
            [&](const auto&, const auto& page) { delivered.set_value(page.columns.back().observedMs); return true; });
        ASSERT_TRUE(sub);
        auto record = std::make_shared<recording::Hmc2Record>();
        record->header.symbol = "BTC-USD"; record->header.layer = "near";
        record->bucketStartMs = record->committedThroughMs = recording::kHmc2MinMs;
        record->observedMs = 1000;
        record->flags = recording::kProvisional;
        record->bidRowLo = record->askRowLo = 90; record->bidRowHi = record->askRowHi = 110;
        model.recordingLive()->publish(record);
        auto result = delivered.get_future();
        ASSERT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        EXPECT_EQ(result.get(), 1000);
        server.stop();
        EXPECT_FALSE(sub->active.load());
    }
}

TEST(TradeOverlayServer, BoundedWorkersHistoryAndCloseDiscardLatePublication) {
    int argc = 1; char name[] = "overlay-queue"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerConfig config; config.heatmap.persistenceEnabled = false; config.recording.enabled = false;
    ServerDataModel model(config);
    QTemporaryDir dir;
    Authenticator auth(dir.path().toStdString() + "/no-credentials");
    SentinelStreamServer server(model, auth, config, 0);
    RecordingServerStopTest::checkOverlayBounds(server, model);
}

TEST(RecordingServerStop, StalledCandleFetchIsJoinedBeforeServerDestruction) {
    int argc = 1; char name[] = "candle-stop"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    struct RestoreDirectory { QString path = QDir::currentPath(); ~RestoreDirectory() { QDir::setCurrent(path); } } restore;
    ASSERT_TRUE(QDir::setCurrent(directory.path()));
    ServerConfig config; config.recording.enabled = false; config.heatmap.persistenceEnabled = false;
    config.heatmap.timeframesMs = {60000};
    ServerDataModel model(config);
    Authenticator auth(directory.path().toStdString() + "/no-key");
    net::io_context listener;
    tcp::acceptor acceptor(listener, {net::ip::make_address("127.0.0.1"), 0});
    tcp::socket peer(listener);
    std::array<char, 65536> buffer{};
    std::promise<unsigned char> clientHello;
    std::promise<void> closed;
    auto helloFuture = clientHello.get_future(); auto closedFuture = closed.get_future();
    acceptor.async_accept(peer, [&](beast::error_code ec) {
        if (ec) return;
        peer.async_read_some(net::buffer(buffer), [&](beast::error_code ec, size_t size) {
            clientHello.set_value(!ec && size > 0 ? static_cast<unsigned char>(buffer[0]) : 0);
            if (ec) { closed.set_value(); return; }
            // ClientHello arrived, but never answer it. The following read only
            // observes closure after the stalled TLS request's deadline.
            net::async_read(peer, net::buffer(buffer), [&](beast::error_code ec, size_t) {
                if (ec) closed.set_value();
            });
        });
    });
    std::thread listenerThread([&] { listener.run(); });
    struct Join { net::io_context& ioc; std::thread& thread; ~Join() { ioc.stop(); thread.join(); } } join{listener, listenerThread};
    auto server = std::make_unique<SentinelStreamServer>(model, auth, config, 0);
    auto session = RecordingServerStopTest::startCandleFetch(*server, model, auth, acceptor.local_endpoint().port());
    ASSERT_EQ(helloFuture.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    ASSERT_EQ(helloFuture.get(), 0x16); // TLS handshake record, not just TCP accept
    const auto start = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_TRUE(session.expired()); // no detached fetch retains a session/executor
    server.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              RecordingServerStopTest::kStalledFetchDeadline + std::chrono::seconds(2));
    EXPECT_EQ(closedFuture.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
}

#ifndef _WIN32
TEST(RecordingServerStop, JoinedProcessWorkHonorsCancellationAndDeadline) {
    int argc = 1; char name[] = "process-stop"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    EXPECT_EQ(kScreenerProcessBudget, std::chrono::seconds(90));
    for (bool cancel : {true, false}) {
        QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
        const auto pidFile = directory.path() + "/child.pid";
        pid_t child = 0;
        const auto readChild = [&] {
            QFile file(pidFile);
            if (file.open(QIODevice::ReadOnly)) child = static_cast<pid_t>(file.readAll().trimmed().toLongLong());
        };
        bool cancellationRequested = false;
        const auto start = std::chrono::steady_clock::now();
        const auto result = runBoundedProcess("/bin/sh",
            {"-c", "trap 'wait; exit' TERM; sleep 60 & echo $! > child.pid; wait"}, directory.path(), [&] {
                if (cancel && !cancellationRequested) {
                    readChild();
                    cancellationRequested = child > 0 && ::kill(child, 0) == 0;
                }
                return cancellationRequested;
            }, cancel ? std::chrono::seconds(5) : std::chrono::milliseconds(500));
        EXPECT_EQ(result.error, cancel ? "process cancelled" : "process deadline exceeded");
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
        readChild();
        ASSERT_GT(child, 0); // real shell -> sleep grandchild of this test process
        const auto reapedBy = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (::kill(child, 0) == 0 && std::chrono::steady_clock::now() < reapedBy)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        errno = 0;
        const auto status = ::kill(child, 0);
        EXPECT_EQ(status, -1) << "grandchild survived process-group termination: " << child;
        EXPECT_EQ(errno, ESRCH);
        if (status == 0) ::kill(child, SIGKILL); // clean up on regression without hiding failure
    }
}
#endif

TEST(CandleHistoryPaging, OneSecondPagesReachOlderRetainedBars) {
    int argc = 1;
    char name[] = "candle-pages";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    struct RestoreDirectory { QString path = QDir::currentPath(); ~RestoreDirectory() { QDir::setCurrent(path); } } restore;
    ASSERT_TRUE(QDir::setCurrent(directory.path()));
    ServerConfig config;
    config.recording.enabled = false;
    config.heatmap.persistenceEnabled = false;
    config.heatmap.timeframesMs = {1000, 60000};
    ServerDataModel model(config);
    for (int second = 1; second <= 1001; ++second) {
        Trade trade{};
        trade.product_id = "BTC-USD";
        trade.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(second));
        trade.price = 100;
        trade.size = 1;
        model.onTrade(trade);
    }
    model.acquireGuiFeed("ETH-USD");
    for (const int second : {1, 10, 20, 30}) {
        Trade trade{};
        trade.product_id = "ETH-USD";
        trade.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(second));
        trade.price = 100;
        trade.size = 1;
        model.onTrade(trade);
    }
    RecordingServerStopTest::checkCandlePages(model);
}

namespace {
struct ServerFixture {
    QTemporaryDir dir;
    ServerConfig config;
    std::unique_ptr<ServerDataModel> model;
    std::unique_ptr<Authenticator> auth;
    std::unique_ptr<SentinelStreamServer> server;
    bool init() {
        config.heatmap.persistenceEnabled = false;
        config.recording.enabled = false;
        config.tls.certFile = dir.path().toStdString() + "/cert.pem";
        config.tls.keyFile = dir.path().toStdString() + "/key.pem";
        if (!dir.isValid() || !writeSelfSignedCert(config.tls.certFile, config.tls.keyFile)) return false;
        model = std::make_unique<ServerDataModel>(config);
        auth = std::make_unique<Authenticator>(dir.path().toStdString() + "/no-credentials");
        server = std::make_unique<SentinelStreamServer>(*model, *auth, config, 0);
        return true;
    }
};
} // namespace

// FM-154/FM-202: stop() queues the acceptor close on the I/O thread and then stops
// the I/O context. If that thread is busy, the close (and the aborted accept) stay
// queued and run after the next start(): the old close then closes the new listener
// and the accept loop spins on "Bad file descriptor".
TEST(RecordingServerStop, StaleStopWorkCannotCloseTheNextListener) {
    int argc = 1; char name[] = "stale-acceptor"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerFixture fixture;
    ASSERT_TRUE(fixture.init());
    auto& server = *fixture.server;
    for (int cycle = 0; cycle < 3; ++cycle) {
        server.start();
        ASSERT_TRUE(RecordingServerStopTest::listening(server)) << "cycle " << cycle;
        const auto port = RecordingServerStopTest::port(server);
        ASSERT_NE(port, 0) << "cycle " << cycle;
        EXPECT_TRUE(RecordingServerStopTest::accepts(server, port)) << "cycle " << cycle;
        const auto drained = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (RecordingServerStopTest::sessionCount(server) != 0 && std::chrono::steady_clock::now() < drained)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // Hold the I/O thread so stop() stops the context before its close can run.
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        net::post(RecordingServerStopTest::ioContext(server), [&entered, gate] { entered.set_value(); gate.wait(); });
        ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
        auto stopped = std::async(std::launch::async, [&] { server.stop(); });
        const bool ioStopped = RecordingServerStopTest::waitIoStopped(server);
        release.set_value();
        ASSERT_TRUE(ioStopped);
        ASSERT_EQ(stopped.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    }
    server.start();
    EXPECT_TRUE(RecordingServerStopTest::listening(server)) << "stale shutdown work closed the new listener";
    const auto port = RecordingServerStopTest::port(server);
    ASSERT_NE(port, 0);
    EXPECT_TRUE(RecordingServerStopTest::accepts(server, port));
    server.stop();
}

// The same lifecycle without forcing the order: back-to-back start/stop while
// background clients keep connecting, so stop() races accepted sessions on the
// I/O thread. Every start must listen and accept.
TEST(RecordingServerStop, StartStopLoopUnderClientChurnKeepsAccepting) {
    int argc = 1; char name[] = "acceptor-churn"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerFixture fixture;
    ASSERT_TRUE(fixture.init());
    auto& server = *fixture.server;
    std::atomic<unsigned short> target{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> clients;
    struct JoinClients {
        std::atomic<bool>& done; std::vector<std::thread>& threads;
        ~JoinClients() { done = true; for (auto& t : threads) t.join(); }
    } joinClients{done, clients};
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back([&] {
            net::io_context ioc;
            while (!done) {
                const auto port = target.load();
                if (port == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
                tcp::socket socket(ioc);
                beast::error_code ec;
                socket.connect({net::ip::make_address("127.0.0.1"), port}, ec);
                // Reset instead of FIN: thousands of TIME_WAIT sockets would exhaust the
                // ephemeral ports and fail the next start() with EADDRNOTAVAIL.
                if (!ec) socket.set_option(net::socket_base::linger(true, 0), ec);
                socket.close(ec);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }
    for (int cycle = 0; cycle < 40; ++cycle) {
        server.start();
        ASSERT_TRUE(RecordingServerStopTest::listening(server)) << "cycle " << cycle;
        const auto port = RecordingServerStopTest::port(server);
        ASSERT_NE(port, 0) << "cycle " << cycle;
        target = port;
        EXPECT_TRUE(RecordingServerStopTest::accepts(server, port)) << "cycle " << cycle;
        server.stop(); // clients keep connecting while the server stops
    }
}

// An accept that fails at once (here: a closed listener, which returns EBADF) must
// not retry in a tight loop on the I/O thread. Before the backoff, this loop made
// tens of thousands of attempts per second and logged one error per attempt.
TEST(RecordingServerStop, FailingAcceptBacksOffInsteadOfSpinning) {
    int argc = 1; char name[] = "accept-backoff"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerFixture fixture;
    ASSERT_TRUE(fixture.init());
    auto& server = *fixture.server;
    server.start();
    ASSERT_TRUE(RecordingServerStopTest::listening(server));
    ASSERT_TRUE(RecordingServerStopTest::closeListener(server));
    const auto before = RecordingServerStopTest::acceptAttempts(server);
    ASSERT_GE(before, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto attempts = RecordingServerStopTest::acceptAttempts(server) - before;
    // Backoff schedule: one immediate retry, then waits of 10, 20, 40, 80, 160, 320 ms,
    // so at most 7 more attempts fit in the window. Load only delays the timers.
    EXPECT_LE(attempts, 8) << attempts << " accept attempts in 600 ms on a closed listener";
    EXPECT_FALSE(RecordingServerStopTest::listening(server));
    const auto start = std::chrono::steady_clock::now();
    server.stop(); // the pending retry wait must not delay shutdown
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    server.start(); // a closed listener does not poison the next start
    EXPECT_TRUE(RecordingServerStopTest::listening(server));
    const auto port = RecordingServerStopTest::port(server);
    ASSERT_NE(port, 0);
    EXPECT_TRUE(RecordingServerStopTest::accepts(server, port));
    server.stop();
}

// Review r1: an accept completion passed the running check, then stop() took its
// session snapshot before the completion registered the session. Nobody stopped that
// session; its socket and queued handshake survived into the next start().
TEST(RecordingServerStop, AcceptRacingStopLeavesNoSessionBehind) {
    int argc = 1; char name[] = "accept-vs-stop"; char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    ServerFixture fixture;
    ASSERT_TRUE(fixture.init());
    auto& server = *fixture.server;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<bool> armed{true};
    RecordingServerStopTest::beforeSessionAdmit(server, [&entered, gate, &armed] {
        if (!armed.exchange(false)) return;
        entered.set_value();
        gate.wait();
    });
    server.start();
    const auto port = RecordingServerStopTest::port(server);
    ASSERT_NE(port, 0);
    net::io_context ioc;
    tcp::socket client(ioc); // stays connected and idle, so its session cannot end by itself
    beast::error_code ec;
    client.connect({net::ip::make_address("127.0.0.1"), port}, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    // stop() snapshots sessions and stops the I/O context while the completion is parked.
    auto stopped = std::async(std::launch::async, [&] { server.stop(); });
    const bool ioStopped = RecordingServerStopTest::waitIoStopped(server);
    release.set_value();
    ASSERT_TRUE(ioStopped);
    ASSERT_EQ(stopped.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(RecordingServerStopTest::sessionCount(server), 0u) << "a session admitted after the shutdown snapshot survived stop()";
    // The refused session closed its socket: the idle client sees EOF or a reset.
    client.non_blocking(true, ec);
    char byte = 0;
    const auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        client.read_some(net::buffer(&byte, 1), ec);
        if (ec == net::error::would_block) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (ec == net::error::would_block && std::chrono::steady_clock::now() < readDeadline);
    EXPECT_TRUE(ec == net::error::eof || ec == net::error::connection_reset) << ec.message();
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(RecordingServerStopTest::sessionCount(server), 0u) << "an old session resumed on restart";
    const auto restarted = RecordingServerStopTest::port(server);
    ASSERT_NE(restarted, 0);
    EXPECT_TRUE(RecordingServerStopTest::accepts(server, restarted));
    client.close(ec);
    server.stop();
}
