#include "servermodel/ServerDataModel.hpp"
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <cmath>
#include <map>

namespace {
nlohmann::json metadata(const std::string& symbol, const std::string& quote) {
    return {{"product_id", symbol}, {"quote_increment", quote},
            {"base_increment", "0.00000001"}};
}

struct LiveBookTickTest : testing::Test {
    int argc = 1;
    char name[32] = "live-book-tick";
    char* argv[2] = {name, nullptr};
    QCoreApplication app{argc, argv};
    ServerConfig config;
    LiveBookTickTest() {
        config.recording.enabled = false;
        config.heatmap.persistenceEnabled = false;
        config.defaultSymbols = {"BTC-USD"};
        config.orderbook.tickSize = .1;
    }
};
}

TEST_F(LiveBookTickTest, MetadataWaitNeverReplaysOldSnapshotAndKeepsAdjacentLowPriceBuckets) {
    ServerDataModel model(config);
    uint64_t lifetime = 0;
    int resnapshots = 0;
    std::vector<double> publishedTicks;
    std::vector<uint64_t> publishedVersions;
    QObject::connect(&model, &ServerDataModel::productMetadataRequested, &model,
        [&](const QString& symbol, uint64_t id) { if (symbol == "DOGE-USD") lifetime = id; });
    QObject::connect(&model, &ServerDataModel::liveBookResnapshotRequested, &model,
        [&](const QString& symbol) { if (symbol == "DOGE-USD") ++resnapshots; });
    QObject::connect(&model, &ServerDataModel::bookSnapshotBroadcast, &model,
        [&](const QString& symbol, const auto&, const auto&, double tick, const QString&, uint64_t version) {
            if (symbol == "DOGE-USD") {
                publishedTicks.push_back(tick);
                publishedVersions.push_back(version);
            }
        });
    model.acquireGuiFeed("DOGE-USD");
    ASSERT_GT(lifetime, 0u);
    const std::vector<OrderBookLevel> bids{{.20000, 1}, {.20001, 2}, {.20002, 2}};
    const std::vector<OrderBookLevel> asks{{.20006, 3}, {.20007, 4}, {.20008, 4}};
    model.onLiveOrderBookInitialized("DOGE-USD", bids, asks, 1000);
    EXPECT_EQ(model.ensureSymbol("DOGE-USD").liveBook.getTickSize(), 0.0);
    ASSERT_EQ(publishedTicks, std::vector<double>({0.0}));
    model.onLiveOrderBookLevelUpdates("DOGE-USD", {{true, .20004, 5}}, 1001);
    model.onProductMetadata("DOGE-USD", lifetime, metadata("DOGE-USD", "0.00001"), "");
    EXPECT_EQ(resnapshots, 1);
    EXPECT_EQ(model.ensureSymbol("DOGE-USD").liveBook.getTickSize(), 0.0);
    // Only a fresh upstream snapshot may restart the book after skipped deltas.
    model.onLiveOrderBookInitialized("DOGE-USD", bids, asks, 1002);
    const auto book = model.ensureSymbol("DOGE-USD").liveBook.snapshotLevels();
    EXPECT_NEAR(book.tickSize, .00002, 1e-12);
    ASSERT_EQ(book.bids.size(), 2u);
    ASSERT_EQ(book.asks.size(), 2u);
    EXPECT_DOUBLE_EQ(book.bids.front().size, 3.0);
    EXPECT_DOUBLE_EQ(book.asks.front().size, 7.0);
    EXPECT_GT(book.bids.front().price, 0.0);
    ASSERT_EQ(publishedTicks.size(), 2u);
    EXPECT_DOUBLE_EQ(publishedTicks.back(), book.tickSize);
    EXPECT_GT(publishedVersions[1], publishedVersions[0]);

    model.onLiveOrderBookLevelUpdates("DOGE-USD", {{true, .20001, 5}}, 1003);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("DOGE-USD").liveBook.getBidVolume(), 8.0);
    model.onLiveOrderBookLevelUpdates("DOGE-USD", {{true, .20000, 0}}, 1004);
    const auto afterDelete = model.ensureSymbol("DOGE-USD").liveBook.snapshotLevels();
    ASSERT_EQ(afterDelete.bids.size(), 2u);
    EXPECT_DOUBLE_EQ(afterDelete.bids.front().size, 5.0);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("DOGE-USD").liveBook.getBidVolume(), 7.0);
}

TEST_F(LiveBookTickTest, QuoteFloorNondecimalMultipleAndBtcConfiguredTick) {
    ServerDataModel model(config);
    std::map<std::string, uint64_t> lifetimes;
    QObject::connect(&model, &ServerDataModel::productMetadataRequested, &model,
        [&](const QString& symbol, uint64_t id) { lifetimes[symbol.toStdString()] = id; });
    model.acquireGuiFeed("PEPE-USD");
    model.onProductMetadata("PEPE-USD", lifetimes.at("PEPE-USD"), metadata("PEPE-USD", "0.00000001"), "");
    model.onLiveOrderBookInitialized("PEPE-USD", {{.00001000, 1}, {.00001001, 2}},
                                     {{.00001003, 3}}, 1000);
    const auto pepe = model.ensureSymbol("PEPE-USD").liveBook.snapshotLevels();
    EXPECT_DOUBLE_EQ(pepe.tickSize, 1e-8);
    ASSERT_EQ(pepe.bids.size(), 2u);
    EXPECT_GT(pepe.bids.front().price, 0.0);

    model.acquireGuiFeed("ODD-USD");
    model.onProductMetadata("ODD-USD", lifetimes.at("ODD-USD"), metadata("ODD-USD", "0.03"), "");
    model.onLiveOrderBookInitialized("ODD-USD", {{99.99, 1}, {100.02, 2}}, {{100.08, 3}}, 1000);
    const auto odd = model.ensureSymbol("ODD-USD").liveBook.snapshotLevels();
    EXPECT_DOUBLE_EQ(odd.tickSize, .03);
    ASSERT_EQ(odd.bids.size(), 2u);
    EXPECT_NEAR(odd.bids[1].price - odd.bids[0].price, .03, 1e-10);

    model.onLiveOrderBookInitialized("BTC-USD", {{100000, 1}, {100000.05, 2}},
                                     {{100000.2, 2}}, 1000);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getTickSize(), .1);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getBidVolume(), 3.0);
    model.onLiveOrderBookLevelUpdates("BTC-USD", {{true, 100000.05, 5}}, 1001);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getBidVolume(), 6.0);
    model.onLiveOrderBookLevelUpdates("BTC-USD", {{true, 100000, 0}}, 1002);
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getBidVolume(), 5.0);
}

TEST_F(LiveBookTickTest, Binary64BoundaryCorrectionDoesNotMoveAnOffBoundaryPrice) {
    LiveOrderBook book("TEST-USD");
    book.initialize(99, 101, .1);
    book.applyUpdates(std::vector<BookLevelUpdate>{{true, 100.1 - 1e-6, 1},
                                                   {true, 100.1, 2}},
                      std::chrono::system_clock::now(), nullptr);
    const auto levels = book.snapshotLevels();
    ASSERT_EQ(levels.bids.size(), 2u);
    EXPECT_NEAR(levels.bids[0].price, 100.0, 1e-10);
    EXPECT_NEAR(levels.bids[1].price, 100.1, 1e-10);
}

TEST_F(LiveBookTickTest, DeletingHugeContributionPreservesTinyPeerInSameBucket) {
    ServerDataModel model(config);
    for (const double huge : {1e6, 1e16}) {
        model.onLiveOrderBookInitialized("BTC-USD", {{100000, huge}, {100000.05, 1e-8}},
                                         {{100000.2, 2}}, 1000);
        EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getTickSize(), .1);
        model.onLiveOrderBookLevelUpdates("BTC-USD", {{true, 100000, 0}}, 1001);
        const auto after = model.ensureSymbol("BTC-USD").liveBook.snapshotLevels();
        ASSERT_EQ(after.bids.size(), 1u);
        EXPECT_GT(after.bids.front().size, 0.0);
        EXPECT_NEAR(after.bids.front().size, 1e-8, 1e-16);
    }
}

TEST_F(LiveBookTickTest, CrossedSnapshotIsUnavailableUntilFreshTwoSidedBook) {
    ServerDataModel model(config);
    QString status;
    QObject::connect(&model, &ServerDataModel::bookSnapshotBroadcast, &model,
        [&](const QString& symbol, const auto&, const auto&, double, const QString& next, uint64_t) {
            if (symbol == "BTC-USD") status = next;
        });
    model.onLiveOrderBookInitialized("BTC-USD", {{101, 1}}, {{100, 1}}, 1000);
    EXPECT_EQ(status, QStringLiteral("invalid_snapshot"));
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getTickSize(), 0.0);
    model.onLiveOrderBookInitialized("BTC-USD", {{99, 1}}, {{100, 1}}, 1001);
    EXPECT_EQ(status, QStringLiteral("ready"));
    EXPECT_DOUBLE_EQ(model.ensureSymbol("BTC-USD").liveBook.getTickSize(), .1);
}

TEST_F(LiveBookTickTest, StaleAndInvalidMetadataCannotInitializeReacquiredFeed) {
    ServerDataModel model(config);
    std::vector<uint64_t> lifetimes;
    QObject::connect(&model, &ServerDataModel::productMetadataRequested, &model,
        [&](const QString& symbol, uint64_t id) { if (symbol == "ETH-USD") lifetimes.push_back(id); });
    model.acquireGuiFeed("ETH-USD");
    ASSERT_EQ(lifetimes.size(), 1u);
    model.releaseGuiFeed("ETH-USD");
    model.acquireGuiFeed("ETH-USD");
    ASSERT_EQ(lifetimes.size(), 2u);
    model.onProductMetadata("ETH-USD", lifetimes[0], metadata("ETH-USD", "0.01"), "");
    model.onLiveOrderBookInitialized("ETH-USD", {{2500, 1}}, {{2500.5, 1}}, 1000);
    EXPECT_EQ(model.ensureSymbol("ETH-USD").liveBook.getTickSize(), 0.0);
    model.onProductMetadata("ETH-USD", lifetimes[1], metadata("ETH-USD", "bad"), "");
    model.onLiveOrderBookInitialized("ETH-USD", {{2500, 1}}, {{2500.5, 1}}, 1001);
    EXPECT_EQ(model.ensureSymbol("ETH-USD").liveBook.getTickSize(), 0.0);
    model.onProductMetadata("ETH-USD", lifetimes[1], metadata("ETH-USD", "0.01"), "");
    model.onLiveOrderBookInitialized("ETH-USD", {{2500, 1}}, {{2500.5, 1}}, 1002);
    EXPECT_GT(model.ensureSymbol("ETH-USD").liveBook.getTickSize(), 0.0);
    model.onLiveOrderBookInvalidated("ETH-USD", "test gap");
    EXPECT_EQ(model.ensureSymbol("ETH-USD").liveBook.getTickSize(), 0.0);
}
