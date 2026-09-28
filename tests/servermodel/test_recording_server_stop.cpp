#include "protocol/SentinelStreamServer.hpp"
#include "marketdata/auth/Authenticator.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <future>

// Drive the actual stop path without starting sockets or needing TLS credentials.
struct RecordingServerStopTest {
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
