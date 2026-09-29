// Compile the private Session implementation here to exercise its actual queue
// and close path without exposing transport internals in a production header.
#include "protocol/SentinelStreamServer.cpp"
#include "marketdata/auth/Authenticator.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <future>
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
    static boost::asio::io_context& startExecutor(SentinelStreamServer& server) {
        server.m_running = true;
        server.m_thread = std::thread([&server, guard = boost::asio::make_work_guard(server.m_ioc)] {
            server.m_ioc.run();
        });
        return server.m_ioc;
    }
};
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
    // An ephemeral self-signed fixture avoids external openssl commands/certs.
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    ASSERT_TRUE(ctx);
    ASSERT_GT(EVP_PKEY_keygen_init(ctx.get()), 0);
    ASSERT_GT(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048), 0);
    EVP_PKEY* rawKey = nullptr;
    ASSERT_GT(EVP_PKEY_keygen(ctx.get(), &rawKey), 0);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    ASSERT_TRUE(cert);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_set_pubkey(cert.get(), key.get());
    auto* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);
    ASSERT_GT(X509_sign(cert.get(), key.get(), EVP_sha256()), 0);
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> pemKey(BIO_new_file(config.tls.keyFile.c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> pemCert(BIO_new_file(config.tls.certFile.c_str(), "w"), BIO_free);
        ASSERT_TRUE(pemKey); ASSERT_TRUE(pemCert);
        ASSERT_EQ(PEM_write_bio_PrivateKey(pemKey.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
        ASSERT_EQ(PEM_write_bio_X509(pemCert.get(), cert.get()), 1);
    }
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
