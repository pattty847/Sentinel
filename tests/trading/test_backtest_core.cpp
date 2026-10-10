#include <gtest/gtest.h>

#include "trading/AlgoBacktestAdapter.hpp"
#include "trading/AvendellaMM.hpp"
#include "trading/IBacktestStrategy.hpp"
#include "trading/MarketEventSource.hpp"
#include "trading/ReplayEngine.hpp"
#include "trading/LiveTradingSession.hpp"
#include "trading/SimulationBroker.hpp"
#include "trading/TradeDrivenExecutionModel.hpp"
#include "servermodel/TickBinaryLogger.hpp"

#include "capture/RawCapture.hpp"
#include "roller/JournalFeed.hpp"
#include <QTemporaryDir>
#include <QProcess>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <limits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
#include <tuple>
#include <vector>

namespace {

trading::MarketEvent tradeEvent(int64_t timestampMs, double price, double qty = 1.0, std::string symbol = "BTC-USD") {
    trading::TradeEvent trade;
    trade.symbol = std::move(symbol);
    trade.price = price;
    trade.qty = qty;
    trade.timestampMs = timestampMs;

    trading::MarketEvent event;
    event.type = trading::MarketEventType::Trade;
    event.timestampMs = timestampMs;
    event.trade = trade;
    return event;
}

class ScriptedStrategy final : public trading::IBacktestStrategy {
public:
    explicit ScriptedStrategy(std::string id)
        : m_id(std::move(id)) {}

    const std::string& id() const override { return m_id; }

    void onExecutionEvent(const trading::ExecutionEvent& event) override {
        m_seenEvents.push_back(event.type);
        if (event.orderUpdate.has_value()) {
            m_orderUpdates.push_back(*event.orderUpdate);
        }
    }

    std::vector<trading::OrderIntent> onMarketEvent(const trading::MarketEvent& event,
                                                    const trading::BrokerSnapshot& snapshot) override {
        std::vector<trading::OrderIntent> intents;
        if (!event.trade.has_value()) {
            return intents;
        }

        if (m_behavior == "market_then_flatten") {
            if (!m_placed) {
                trading::OrderIntent buy;
                buy.intentId = "buy-1";
                buy.action = trading::OrderIntentAction::PlaceOrder;
                buy.symbol = snapshot.symbol;
                buy.side = trading::OrderSide::Buy;
                buy.orderType = trading::OrderType::Market;
                buy.qty = 1.0;
                buy.timestampMs = event.timestampMs;
                intents.push_back(buy);
                m_placed = true;
            } else if (!m_flattened && snapshot.position.has_value() && snapshot.position->netQty > 0.0) {
                trading::OrderIntent flatten;
                flatten.intentId = "flatten-1";
                flatten.action = trading::OrderIntentAction::Flatten;
                flatten.symbol = snapshot.symbol;
                flatten.timestampMs = event.timestampMs;
                intents.push_back(flatten);
                m_flattened = true;
            }
        } else if (m_behavior == "resting_limit") {
            if (!m_placed) {
                trading::OrderIntent buy;
                buy.intentId = "limit-1";
                buy.action = trading::OrderIntentAction::PlaceOrder;
                buy.symbol = snapshot.symbol;
                buy.side = trading::OrderSide::Buy;
                buy.orderType = trading::OrderType::Limit;
                buy.qty = 1.0;
                buy.price = event.trade->price - 1.0;
                buy.hasPrice = true;
                buy.timestampMs = event.timestampMs;
                intents.push_back(buy);
                m_placed = true;
            }
        } else if (m_behavior == "cancel_before_fill") {
            if (!m_placed) {
                trading::OrderIntent buy;
                buy.intentId = "limit-cancel";
                buy.action = trading::OrderIntentAction::PlaceOrder;
                buy.symbol = snapshot.symbol;
                buy.side = trading::OrderSide::Buy;
                buy.orderType = trading::OrderType::Limit;
                buy.qty = 1.0;
                buy.price = event.trade->price - 1.0;
                buy.hasPrice = true;
                buy.timestampMs = event.timestampMs;
                intents.push_back(buy);
                m_placed = true;
            } else if (!m_canceled) {
                for (const auto& order : snapshot.openOrders) {
                    if (order.status == trading::OrderStatus::Open) {
                        trading::OrderIntent cancel;
                        cancel.intentId = "cancel-1";
                        cancel.action = trading::OrderIntentAction::CancelOrder;
                        cancel.symbol = snapshot.symbol;
                        cancel.targetOrderId = order.id;
                        cancel.timestampMs = event.timestampMs;
                        intents.push_back(cancel);
                        m_canceled = true;
                        break;
                    }
                }
            }
        }
        return intents;
    }

    void setBehavior(std::string behavior) { m_behavior = std::move(behavior); }

    const std::vector<trading::OrderUpdate>& orderUpdates() const { return m_orderUpdates; }

private:
    std::string m_id;
    std::string m_behavior;
    bool m_placed = false;
    bool m_flattened = false;
    bool m_canceled = false;
    std::vector<trading::ExecutionEventType> m_seenEvents;
    std::vector<trading::OrderUpdate> m_orderUpdates;
};

trading::BacktestResult runBacktest(std::vector<trading::MarketEvent> events,
                                    trading::IBacktestStrategy& strategy,
                                    const std::string& symbol = "BTC-USD") {
    trading::VectorMarketEventSource source(std::move(events));
    trading::SimulationBroker broker(
        {},
        std::make_unique<trading::TradeDrivenExecutionModel>(),
        0.0);
    trading::BacktestConfig config;
    config.symbol = symbol;
    config.strategyId = strategy.id();
    trading::ReplayEngine replay;
    return replay.run(source, strategy, broker, config);
}

} // namespace

TEST(BacktestCore, MarketOrderLifecycleAndPnlAreDeterministic) {
    ScriptedStrategy strategy("Scripted");
    strategy.setBehavior("market_then_flatten");

    auto result = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(2000, 110.0),
    }, strategy);

    EXPECT_EQ(result.summary.fillCount, 2);
    EXPECT_DOUBLE_EQ(result.summary.realizedPnl, 10.0);
    ASSERT_FALSE(result.pnlCurve.empty());
    EXPECT_EQ(result.pnlCurve.back().timestampMs, 2000);
}

TEST(BacktestCore, RestingLimitOrderFillsOnLaterTrade) {
    ScriptedStrategy strategy("LimitStrategy");
    strategy.setBehavior("resting_limit");

    auto result = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(2000, 99.0),
    }, strategy);

    bool sawOpen = false;
    bool sawFill = false;
    for (const auto& update : result.orderLifecycleLog) {
        sawOpen = sawOpen || update.status == trading::OrderStatus::Open;
        sawFill = sawFill || update.status == trading::OrderStatus::Filled;
    }
    EXPECT_TRUE(sawOpen);
    EXPECT_TRUE(sawFill);
}

TEST(BacktestCore, CancelPreventsLaterLimitFill) {
    ScriptedStrategy strategy("CancelStrategy");
    strategy.setBehavior("cancel_before_fill");

    auto result = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(1500, 100.5),
        tradeEvent(2000, 99.0),
    }, strategy);

    bool sawCancel = false;
    bool sawFill = false;
    for (const auto& update : result.orderLifecycleLog) {
        sawCancel = sawCancel || update.status == trading::OrderStatus::Canceled;
        sawFill = sawFill || update.status == trading::OrderStatus::Filled;
    }
    EXPECT_TRUE(sawCancel);
    EXPECT_FALSE(sawFill);
}

TEST(BacktestCore, ReplayResultIsStableAcrossRuns) {
    ScriptedStrategy first("Deterministic");
    first.setBehavior("market_then_flatten");
    auto firstResult = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(2000, 101.0),
        tradeEvent(3000, 102.0),
    }, first);

    ScriptedStrategy second("Deterministic");
    second.setBehavior("market_then_flatten");
    auto secondResult = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(2000, 101.0),
        tradeEvent(3000, 102.0),
    }, second);

    EXPECT_EQ(firstResult.summary.eventCount, secondResult.summary.eventCount);
    EXPECT_EQ(firstResult.summary.fillCount, secondResult.summary.fillCount);
    EXPECT_DOUBLE_EQ(firstResult.summary.totalPnl, secondResult.summary.totalPnl);
    ASSERT_EQ(firstResult.orderLifecycleLog.size(), secondResult.orderLifecycleLog.size());
    for (std::size_t i = 0; i < firstResult.orderLifecycleLog.size(); ++i) {
        EXPECT_EQ(firstResult.orderLifecycleLog[i].status, secondResult.orderLifecycleLog[i].status);
        EXPECT_EQ(firstResult.orderLifecycleLog[i].orderId, secondResult.orderLifecycleLog[i].orderId);
    }
}

TEST(BacktestCore, AvendellaRunsThroughAdapter) {
    trading::AlgoParams params;
    params.spreadBps = 10.0;
    params.orderQty = 1.0;
    params.maxPositionQty = 5.0;
    params.skewBps = 0.0;

    trading::AlgoBacktestAdapter strategy(
        std::make_unique<trading::AvendellaMM>(),
        "BTC-USD",
        params);

    auto result = runBacktest({
        tradeEvent(1000, 100.0),
        tradeEvent(2000, 100.0),
        tradeEvent(3000, 101.0),
    }, strategy);

    EXPECT_FALSE(result.orderLifecycleLog.empty());
    bool sawOpen = false;
    for (const auto& update : result.orderLifecycleLog) {
        sawOpen = sawOpen || update.status == trading::OrderStatus::Open;
    }
    EXPECT_TRUE(sawOpen);
}

TEST(BacktestCore, CsvTradeEventSourceParsesRows) {
    std::istringstream input(
        "timestamp_ms,symbol,price,qty\n"
        "1000,BTC-USD,100.0,1.5\n");

    trading::CsvTradeEventSource source(input);
    auto event = source.next();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->trade.has_value());
    EXPECT_EQ(event->trade->symbol, "BTC-USD");
    EXPECT_DOUBLE_EQ(event->trade->price, 100.0);
    EXPECT_DOUBLE_EQ(event->trade->qty, 1.5);
}

TEST(BacktestCore, TickBinaryTradeEventSourceParsesTradeFiles) {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::temp_directory_path() / "sentinel_tick_reader_test";
    fs::create_directories(tempDir);
    for (const auto version : {LogFormat::LEGACY_VERSION, LogFormat::VERSION}) {
        const fs::path filePath = tempDir / (std::to_string(version) + ".bin");
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());

        LogFormat::FileHeader fileHeader;
        fileHeader.version = version;
        fileHeader.created_at_ms = 1000;
        std::memcpy(fileHeader.symbol, "BTC-USD", 7);
        out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));

        LogFormat::TradePayload tradePayload;
        tradePayload.price = 123.45;
        tradePayload.size = 0.75;
        tradePayload.side = 1;

        LogFormat::RecordHeader recordHeader;
        recordHeader.type = LogFormat::RecordType::Trade;
        recordHeader.timestamp_ms = 2000;
        recordHeader.payload_len = sizeof(tradePayload) + 5;

        out.write(reinterpret_cast<const char*>(&recordHeader), sizeof(recordHeader));
        out.write(reinterpret_cast<const char*>(&tradePayload), sizeof(tradePayload));
        out.write("tick1", 5);
    }

    { // the reader keeps 00.bin open until EOF: close it before removing the directory (Windows)
        trading::TickBinaryTradeEventSource source(tempDir, "BTC-USD");
        for (int version = 1; version <= 2; ++version) {
            auto event = source.next();
            ASSERT_TRUE(event.has_value());
            ASSERT_TRUE(event->trade.has_value());
            EXPECT_EQ(event->trade->symbol, "BTC-USD");
            EXPECT_EQ(event->trade->timestampMs, 2000);
            EXPECT_EQ(event->trade->tradeId, "tick1");
            EXPECT_EQ(event->trade->side, version == 1 ? AggressorSide::Sell : AggressorSide::Buy);
            EXPECT_DOUBLE_EQ(event->trade->price, 123.45);
            EXPECT_DOUBLE_EQ(event->trade->qty, 0.75);
        }
        EXPECT_FALSE(source.next().has_value());
    }

    fs::remove_all(tempDir);
}

TEST(BacktestCore, TickLoggerWritesAggressorBasisInVersionedFile) {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::temp_directory_path() / "sentinel_tick_logger_basis_test";
    fs::remove_all(tempDir);
    Trade trade{};
    trade.product_id = "BTC-USD";
    trade.trade_id = "basis";
    trade.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
    trade.price = 100;
    trade.size = 1;
    trade.side = AggressorSide::Sell;
    {
        TickBinaryLogger logger(tempDir.string());
        logger.logTrade(trade);
        logger.flush();
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(tempDir))
        if (entry.is_regular_file()) files.push_back(entry.path());
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].filename(), "00.v2.bin");
    std::ifstream in(files[0], std::ios::binary);
    LogFormat::FileHeader header{};
    LogFormat::RecordHeader record{};
    LogFormat::TradePayload payload{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    in.read(reinterpret_cast<char*>(&record), sizeof(record));
    in.read(reinterpret_cast<char*>(&payload), sizeof(payload));
    ASSERT_TRUE(in.good());
    EXPECT_EQ(header.version, 2); // independent of the writer's version constant
    EXPECT_EQ(record.type, LogFormat::RecordType::Trade);
    EXPECT_EQ(payload.side, 2); // sell aggressor
    fs::remove_all(tempDir);
}

TEST(BacktestCore, LiveTradingSessionProcessesManualCommand) {
    trading::LiveTradingSession session(
        [](const std::string&) { return 100.0; },
        0.0);

    trading::TradingResult captured;
    std::vector<trading::AlgoOrderEvent> algoEvents;
    session.setResultCallback([&](trading::TradingResult result, std::vector<trading::AlgoOrderEvent> events) {
        captured = std::move(result);
        algoEvents = std::move(events);
    });

    trading::TradeCommand cmd;
    cmd.commandId = "manual-1";
    cmd.action = trading::TradeAction::PlaceOrder;
    cmd.symbol = "BTC-USD";
    cmd.side = trading::OrderSide::Buy;
    cmd.orderType = trading::OrderType::Market;
    cmd.qty = 1.0;
    cmd.timestamp = 1000;

    session.processTradeCommand(cmd);

    EXPECT_FALSE(captured.orderUpdates.empty());
    EXPECT_FALSE(captured.positionUpdates.empty());
    EXPECT_TRUE(algoEvents.empty());
}

TEST(BacktestCore, LiveTradingSessionRunsAvendellaOnTradeTicks) {
    trading::LiveTradingSession session(
        [](const std::string&) { return 100.0; },
        0.0);
    session.registerAlgo(std::make_unique<trading::AvendellaMM>());

    trading::AlgoParams params;
    params.spreadBps = 10.0;
    params.orderQty = 1.0;
    params.maxPositionQty = 5.0;
    params.skewBps = 0.0;
    ASSERT_TRUE(session.startAlgo("AvendellaMM", "BTC-USD", params));

    trading::TradingResult captured;
    std::vector<trading::AlgoOrderEvent> algoEvents;
    session.setResultCallback([&](trading::TradingResult result, std::vector<trading::AlgoOrderEvent> events) {
        captured = std::move(result);
        algoEvents = std::move(events);
    });

    session.onTradeTick("BTC-USD", 100.0, 1000);

    EXPECT_FALSE(captured.orderUpdates.empty());
    EXPECT_FALSE(algoEvents.empty());
}

TEST(BacktestCore, LiveTradingSessionTakeProfitClosesPositionAndClearsSiblingRisk) {
    trading::LiveTradingSession session(
        [](const std::string&) { return 100.0; },
        0.0);

    trading::TradingResult captured;
    session.setResultCallback([&](trading::TradingResult result, std::vector<trading::AlgoOrderEvent>) {
        captured = std::move(result);
    });

    trading::TradeCommand entry;
    entry.commandId = "entry-1";
    entry.action = trading::TradeAction::PlaceOrder;
    entry.symbol = "BTC-USD";
    entry.side = trading::OrderSide::Buy;
    entry.orderType = trading::OrderType::Market;
    entry.qty = 1.0;
    entry.timestamp = 1000;
    session.processTradeCommand(entry);

    trading::TradeCommand risk;
    risk.commandId = "risk-1";
    risk.action = trading::TradeAction::SetAttachedRisk;
    risk.symbol = "BTC-USD";
    risk.hasTakeProfit = true;
    risk.takeProfitPrice = 110.0;
    risk.hasStopLoss = true;
    risk.stopLossPrice = 95.0;
    risk.timestamp = 1100;
    session.processTradeCommand(risk);
    ASSERT_FALSE(captured.riskOrderUpdates.empty());
    EXPECT_TRUE(captured.riskOrderUpdates.back().hasTakeProfit);
    EXPECT_TRUE(captured.riskOrderUpdates.back().hasStopLoss);

    session.onTradeTick("BTC-USD", 110.0, 1200);

    ASSERT_FALSE(captured.positionUpdates.empty());
    EXPECT_DOUBLE_EQ(captured.positionUpdates.back().positionQty, 0.0);
    ASSERT_FALSE(captured.riskOrderUpdates.empty());
    EXPECT_FALSE(captured.riskOrderUpdates.back().hasTakeProfit);
    EXPECT_FALSE(captured.riskOrderUpdates.back().hasStopLoss);
}

TEST(BacktestCore, LiveTradingSessionStopLossUsesStopMarketExit) {
    trading::LiveTradingSession session(
        [](const std::string&) { return 100.0; },
        10.0);

    trading::TradingResult captured;
    session.setResultCallback([&](trading::TradingResult result, std::vector<trading::AlgoOrderEvent>) {
        captured = std::move(result);
    });

    trading::TradeCommand entry;
    entry.commandId = "entry-2";
    entry.action = trading::TradeAction::PlaceOrder;
    entry.symbol = "BTC-USD";
    entry.side = trading::OrderSide::Buy;
    entry.orderType = trading::OrderType::Market;
    entry.qty = 1.0;
    entry.timestamp = 1000;
    session.processTradeCommand(entry);

    trading::TradeCommand risk;
    risk.commandId = "risk-2";
    risk.action = trading::TradeAction::SetAttachedRisk;
    risk.symbol = "BTC-USD";
    risk.hasStopLoss = true;
    risk.stopLossPrice = 95.0;
    risk.timestamp = 1100;
    session.processTradeCommand(risk);

    session.onTradeTick("BTC-USD", 94.0, 1200);

    ASSERT_FALSE(captured.orderUpdates.empty());
    EXPECT_EQ(captured.orderUpdates.back().status, trading::OrderStatus::Filled);
    EXPECT_NEAR(captured.orderUpdates.back().avgPrice, 93.906, 1e-6);
    ASSERT_FALSE(captured.positionUpdates.empty());
    EXPECT_DOUBLE_EQ(captured.positionUpdates.back().positionQty, 0.0);
}

namespace {
namespace capture = sentinel::capture;
constexpr int64_t JournalEpoch = 1791331200000; // 2026-10-07 UTC
capture::WriterConfig journalConfig(const QTemporaryDir& temp) {
    capture::WriterConfig c;
    c.root = temp.path();
    c.fsyncBlocks = 0;
    return c;
}
nlohmann::json journalMetadata() {
    return {{"product_metadata", {{"product_id", "BTC-USD"}, {"quote_increment", "0.01"},
                                   {"base_increment", "0.00000001"}}}};
}
capture::Record journalRecord(int64_t ms, capture::Kind kind, std::string payload = "{}", uint64_t connection = 1) {
    return {kind, {ms * 1'000'000, ms * 1'000'000}, connection, std::move(payload)};
}
std::string tradesFrame(const std::string& id, int64_t ms, const std::string& side = "BUY",
                       bool nested = false, const std::string& product = "BTC-USD") {
    const auto time = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString("yyyy-MM-ddTHH:mm:ss.zzz").toStdString() + "456Z";
    const auto trades = nlohmann::json::array({{{"product_id", product}, {"trade_id", id},
                                              {"time", time}, {"side", side}, {"price", "123.45"}, {"size", "0.75"}}});
    nlohmann::json j = {{"channel", "market_trades"}};
    if (nested) j["events"] = nlohmann::json::array({{{"trades", trades}}});
    else j["trades"] = trades;
    return j.dump();
}
void writeTick(const std::filesystem::path& root, int64_t ms, const std::string& id) {
    Trade t{};
    t.product_id = "BTC-USD"; t.trade_id = id; t.price = 123.45; t.size = 0.75;
    t.side = AggressorSide::Sell;
    t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    TickBinaryLogger logger(root.string());
    logger.logTrade(t); logger.flush();
}
}

TEST(BacktestCore, B1JournalTradesIdentitySideAndMilliseconds) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, "not JSON l2_data"));
        writer.append(journalRecord(JournalEpoch + 123, capture::Kind::Frame, tradesFrame("p1", JournalEpoch + 123)));
        writer.append(journalRecord(JournalEpoch + 124, capture::Kind::Frame, tradesFrame("p2", JournalEpoch + 124, "SELL", true)));
        writer.append(journalRecord(JournalEpoch + 125, capture::Kind::Frame, tradesFrame("foreign", JournalEpoch + 125, "BUY", false, "ETH-USD")));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    auto e = source.next(); ASSERT_TRUE(e && e->trade);
    EXPECT_EQ(e->trade->tradeId, "p1");
    EXPECT_EQ(e->trade->timestampMs, JournalEpoch + 123);
    EXPECT_EQ(e->trade->side, AggressorSide::Sell);
    EXPECT_DOUBLE_EQ(e->trade->price, 123.45);
    EXPECT_DOUBLE_EQ(e->trade->qty, 0.75);
    e = source.next(); ASSERT_TRUE(e && e->trade);
    EXPECT_EQ(e->trade->tradeId, "p2");
    EXPECT_EQ(e->trade->side, AggressorSide::Buy);
    EXPECT_FALSE(source.next());
}

TEST(BacktestCore, B2WindowStartsInEarlierFileAndToIsExclusive) {
    QTemporaryDir temp;
    const auto from = JournalEpoch + 30 * 60 * 1000, to = JournalEpoch + 2 * 60 * 60 * 1000;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, tradesFrame("before", from - 1)));
        writer.append(journalRecord(from + 1, capture::Kind::Frame, tradesFrame("inside", from + 1)));
        writer.append(journalRecord(JournalEpoch + 60 * 60 * 1000, capture::Kind::Frame, tradesFrame("at-to", to)));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", from, to);
    const auto e = source.next(); ASSERT_TRUE(e && e->trade);
    EXPECT_EQ(e->trade->tradeId, "inside");
    EXPECT_FALSE(source.next());
}

TEST(BacktestCore, B3TransportGapsAndConnectionBoundaries) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::TransportUp));
        writer.append(journalRecord(JournalEpoch + 10, capture::Kind::TransportDown));
        writer.append(journalRecord(JournalEpoch + 20, capture::Kind::TransportUp));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 100);
    EXPECT_FALSE(source.next());
    const std::vector<std::pair<int64_t, int64_t>> expected{{JournalEpoch + 10, JournalEpoch + 20}};
    EXPECT_EQ(source.gaps(), expected);
}

TEST(BacktestCore, B4JournalFeedAggressorParity) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::TransportUp));
        writer.append(journalRecord(JournalEpoch + 123, capture::Kind::Frame, tradesFrame("p1", JournalEpoch + 123)));
        writer.append(journalRecord(JournalEpoch + 124, capture::Kind::Frame, tradesFrame("p2", JournalEpoch + 124, "SELL", true)));
        writer.close();
    }
    using Identity = std::tuple<std::string, AggressorSide, int64_t>;
    std::vector<Identity> actual, expected;
    sentinel::roller::JournalFeed feed("BTC-USD");
    feed.onTrade = [&](const Trade& t) { expected.emplace_back(t.trade_id, t.side,
        std::chrono::duration_cast<std::chrono::milliseconds>(t.timestamp.time_since_epoch()).count()); };
    sentinel::roller::JournalReader reader(temp.path().toStdString(), "BTC-USD");
    sentinel::roller::JournalRecord record;
    while (reader.next(record)) feed.apply(record);
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    while (auto e = source.next()) actual.emplace_back(e->trade->tradeId, e->trade->side, e->timestampMs);
    ASSERT_EQ(actual.size(), 2u);
    ASSERT_EQ(expected.size(), 2u);
    EXPECT_EQ(actual[0], (Identity{"p1", AggressorSide::Sell, JournalEpoch + 123}));
    EXPECT_EQ(actual[1], (Identity{"p2", AggressorSide::Buy, JournalEpoch + 124}));
    EXPECT_EQ(actual, expected);
}

TEST(BacktestCore, B5SelectorChoosesWholeDaysWithoutMerging) {
    QTemporaryDir journal, legacy;
    constexpr int64_t day = 86'400'000;
    {
        capture::Writer writer(journalConfig(journal), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, tradesFrame("d1", JournalEpoch)));
        writer.append(journalRecord(JournalEpoch + 2 * day, capture::Kind::Frame, tradesFrame("d3", JournalEpoch + 2 * day)));
        writer.close();
    }
    writeTick(legacy.path().toStdString(), JournalEpoch, "d1");
    writeTick(legacy.path().toStdString(), JournalEpoch + day, "d2");
    auto source = trading::openTradeHistory("BTC-USD", JournalEpoch, JournalEpoch + 3 * day,
                                           journal.path().toStdString(), legacy.path().toStdString());
    std::vector<std::string> ids, sources;
    while (const auto e = source->next()) { ids.push_back(e->trade->tradeId); sources.emplace_back(source->sourceName()); }
    EXPECT_EQ(ids, (std::vector<std::string>{"d1", "d2", "d3"}));
    EXPECT_EQ(sources, (std::vector<std::string>{"journal", "file", "journal"}));
}

TEST(BacktestCore, PartialTickAndOpenJournalStopWithoutPolling) {
    QTemporaryDir journal, legacy;
    capture::Writer writer(journalConfig(journal), journalMetadata());
    writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, tradesFrame("open", JournalEpoch)));
    writer.flush();
    trading::JournalTradeEventSource source(journal.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    ASSERT_TRUE(source.next());
    EXPECT_FALSE(source.next());
    EXPECT_FALSE(source.next());
    writeTick(legacy.path().toStdString(), JournalEpoch, "complete");
    std::filesystem::path file;
    for (const auto& e : std::filesystem::recursive_directory_iterator(legacy.path().toStdString()))
        if (e.path().extension() == ".bin") file = e.path();
    {
        std::ofstream out(file, std::ios::binary | std::ios::app);
        LogFormat::RecordHeader header{}; header.type = LogFormat::RecordType::Trade;
        header.payload_len = sizeof(LogFormat::TradePayload) + 5;
        LogFormat::TradePayload payload{};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
        out.write("pa", 2);
    }
    trading::TickBinaryTradeEventSource tick(file, "BTC-USD");
    ASSERT_TRUE(tick.next()); EXPECT_FALSE(tick.next());
}

TEST(BacktestCore, B7CliHistoryDumpAndPositionalCsv) {
    QTemporaryDir temp;
    writeTick(temp.path().toStdString(), JournalEpoch + 1000, "cli");
    QProcess process;
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07T00:00:00Z",
        "--to", "2026-10-07T01:00:00Z", "--journal", temp.path() + "/missing", "--legacy", temp.path(), "--dump"});
    ASSERT_TRUE(process.waitForFinished(10000));
    EXPECT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
    const auto dump = process.readAllStandardOutput();
    EXPECT_TRUE(dump.contains("trade_id,ms,price,size,side,source"));
    EXPECT_TRUE(dump.contains("\"cli\",1791331201000,"));
    const auto csv = temp.path() + "/trades.csv";
    { std::ofstream out(csv.toStdString()); out << "timestamp_ms,symbol,price,qty\n1000,BTC-USD,100,1\n"; }
    process.start(QString::fromUtf8(BACKTEST_CLI), {csv, "BTC-USD"});
    ASSERT_TRUE(process.waitForFinished(10000));
    EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardOutput().contains("events=1"));
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07",
        "--to", "2026-10-08", "--journal", temp.path() + "/missing", "--legacy", temp.path(), "--dump"});
    ASSERT_TRUE(process.waitForFinished(10000)); EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardOutput().contains("\"cli\""));
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07T00:00:00",
        "--to", "2026-10-08", "--dump"});
    ASSERT_TRUE(process.waitForFinished(10000)); EXPECT_NE(process.exitCode(), 0);
}

TEST(BacktestCore, JournalConnectionChangeAndCaptureStopAreGaps) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::TransportUp));
        writer.append(journalRecord(JournalEpoch + 10, capture::Kind::Frame, tradesFrame("a", JournalEpoch + 10)));
        writer.append(journalRecord(JournalEpoch + 20, capture::Kind::TransportUp, "{}", 2));
        writer.append(journalRecord(JournalEpoch + 30, capture::Kind::CaptureStopped, "{}", 2));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 100);
    ASSERT_TRUE(source.next()); EXPECT_FALSE(source.next());
    EXPECT_EQ(source.gaps(), (std::vector<std::pair<int64_t, int64_t>>{
        {JournalEpoch + 10, JournalEpoch + 20}, {JournalEpoch + 30, JournalEpoch + 100}}));
}

namespace {
void binaryTickFile(const std::filesystem::path& path,
                    const std::vector<std::pair<std::string, int64_t>>& trades) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    LogFormat::FileHeader file;
    file.version = LogFormat::LEGACY_VERSION;
    std::memcpy(file.symbol, "BTC-USD", 7);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    for (const auto& [id, ms] : trades) {
        LogFormat::TradePayload payload{123.45, 0.75, 1};
        LogFormat::RecordHeader header{LogFormat::RecordType::Trade, static_cast<uint64_t>(ms),
                                      static_cast<uint32_t>(sizeof(payload) + id.size())};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
        out.write(id.data(), id.size());
    }
}
std::vector<std::string> collectIds(trading::IMarketEventSource& source) {
    std::vector<std::string> ids;
    while (auto e = source.next()) ids.push_back(e->trade->tradeId);
    return ids;
}
std::string batchFrame(const std::vector<std::pair<std::string, int64_t>>& ids, bool snapshot = false) {
    auto trades = nlohmann::json::array();
    for (const auto& [id, ms] : ids)
        trades.push_back(nlohmann::json::parse(tradesFrame(id, ms))["trades"][0]);
    return nlohmann::json{{"channel", "market_trades"}, {"events", nlohmann::json::array({
        {{"type", snapshot ? "snapshot" : "update"}, {"trades", trades}}})}}.dump();
}
}

TEST(BacktestCore, R1CorruptTickHeadersSkipFileAndContinue) {
    for (int kind = 0; kind < 3; ++kind) {
        SCOPED_TRACE(kind);
        QTemporaryDir temp;
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto bad = root / "00.bin";
        binaryTickFile(bad, {{"1", JournalEpoch}});
        {
            std::ofstream out(bad, std::ios::binary | std::ios::app);
            LogFormat::RecordHeader header{};
            header.timestamp_ms = JournalEpoch + 5;
            header.type = kind == 0 ? LogFormat::RecordType::BookUpdate
                        : kind == 1 ? LogFormat::RecordType::Trade : static_cast<LogFormat::RecordType>(99);
            header.payload_len = kind == 0 ? 3'392'839'995u : kind == 1 ? sizeof(LogFormat::TradePayload) + 257 : 0;
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (kind == 1) {
                LogFormat::TradePayload payload{999, 1, 1};
                out.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
                out << std::string(257, 'x');
            }
        }
        binaryTickFile(root / "01.bin", {{"2", JournalEpoch + 10}});
        trading::TickBinaryTradeEventSource source(root, "BTC-USD");
        EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1", "2"}));
        EXPECT_EQ(source.skippedFiles(), 1u);
        for (const auto& path : {temp.path(), QString::fromStdString(bad.string())}) {
            QProcess process;
            process.start(QString::fromUtf8(BACKTEST_CLI), {path, "BTC-USD"});
            ASSERT_TRUE(process.waitForFinished(10000));
            EXPECT_EQ(process.exitCode(), 0);
            const auto error = process.readAllStandardError();
            EXPECT_TRUE(error.contains("skipped_files=1"));
            EXPECT_TRUE(error.contains("offset="));
            EXPECT_TRUE(error.contains("00.bin"));
            EXPECT_TRUE(process.readAllStandardOutput().contains(path == temp.path() ? "events=2" : "events=1"));
        }
    }
}

TEST(BacktestCore, R2JournalDedupeSurvivesUpdatesAndReconnectSnapshots) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, batchFrame({{"1", JournalEpoch}})));
        writer.append(journalRecord(JournalEpoch + 10, capture::Kind::Frame, batchFrame({{"1", JournalEpoch + 10}})));
        writer.append(journalRecord(JournalEpoch + 20, capture::Kind::TransportUp, "{}", 2));
        writer.append(journalRecord(JournalEpoch + 30, capture::Kind::Frame,
            batchFrame({{"2", JournalEpoch + 30}, {"1", JournalEpoch + 20}}, true), 2));
        writer.append(journalRecord(JournalEpoch + 40, capture::Kind::Frame,
            batchFrame({{"", JournalEpoch + 40}, {"text", JournalEpoch + 40}}), 2));
        writer.append(journalRecord(JournalEpoch + 50, capture::Kind::Frame,
            batchFrame({{"", JournalEpoch + 50}, {"text", JournalEpoch + 50}}), 2));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    const auto first = source.next(); ASSERT_TRUE(first);
    EXPECT_EQ(first->trade->tradeId, "1");
    EXPECT_EQ(first->timestampMs, JournalEpoch); // Keep the first occurrence.
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"2", "", "text", "", "text"}));
}

TEST(BacktestCore, R2TickDedupeSurvivesFileRotationAndBypassesTextIds) {
    QTemporaryDir temp;
    const auto root = std::filesystem::path(temp.path().toStdString());
    binaryTickFile(root / "00.bin", {{"2", JournalEpoch + 2}, {"1", JournalEpoch + 1}, {"", JournalEpoch + 3}, {"text", JournalEpoch + 4}});
    binaryTickFile(root / "01.bin", {{"1", JournalEpoch + 5}, {"2", JournalEpoch + 6}, {"", JournalEpoch + 7}, {"text", JournalEpoch + 8}});
    trading::TickBinaryTradeEventSource source(root, "BTC-USD");
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"2", "1", "", "text", "", "text"}));
}

TEST(BacktestCore, R3JournalFramesSortNumericIdsBeforeDedupe) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch + 100, capture::Kind::Frame,
            batchFrame({{"3", JournalEpoch + 3}, {"2", JournalEpoch + 2}, {"1", JournalEpoch + 1},
                        {"1", JournalEpoch + 9}, {"10", JournalEpoch + 10}})));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    const auto first = source.next(); ASSERT_TRUE(first);
    EXPECT_EQ(first->trade->tradeId, "1");
    EXPECT_EQ(first->timestampMs, JournalEpoch + 1); // Stable order for equal IDs.
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"2", "3", "10"}));
}

TEST(BacktestCore, R4M3ReceiveSlackKeepsLateTradesAndStopsAtSixtySeconds) {
    QTemporaryDir temp;
    const auto to = JournalEpoch + 1000;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::TransportUp));
        writer.append(journalRecord(to + 30'000, capture::Kind::Frame, tradesFrame("1", to - 1)));
        writer.append(journalRecord(to + 60'001, capture::Kind::Frame, tradesFrame("2", to - 2)));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, to);
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1"}));
}

TEST(BacktestCore, R4M6UnsealedSegmentAddsGapBeforeSuccessor) {
    QTemporaryDir temp;
    std::filesystem::path first;
    uintmax_t durableBytes = 0;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, tradesFrame("1", JournalEpoch)));
        writer.flush();
        first = writer.currentPath().toStdString();
        durableBytes = std::filesystem::file_size(first);
        writer.sealSegment();
        writer.append(journalRecord(JournalEpoch + 100, capture::Kind::Frame, tradesFrame("2", JournalEpoch + 100)));
        writer.close();
    }
    std::filesystem::resize_file(first, durableBytes); // Same run/connection/ordinals, missing seal only.
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1", "2"}));
    EXPECT_EQ(source.gaps(), (std::vector<std::pair<int64_t, int64_t>>{{JournalEpoch, JournalEpoch + 100}}));
}

TEST(BacktestCore, R4M7LegacySpanClipsBothWindowEdges) {
    QTemporaryDir temp;
    const auto root = std::filesystem::path(temp.path().toStdString());
    binaryTickFile(root / "BTC-USD/2026-10-07/00.bin", {{"1", JournalEpoch + 9}, {"2", JournalEpoch + 11}, {"3", JournalEpoch + 20}});
    auto source = trading::openTradeHistory("BTC-USD", JournalEpoch + 10, JournalEpoch + 20, {}, root);
    EXPECT_EQ(collectIds(*source), (std::vector<std::string>{"2"}));
}

TEST(BacktestCore, R4M9LegacySelectorDoesNotOpenDaysAtOrAfterTo) {
    QTemporaryDir temp;
    const auto root = std::filesystem::path(temp.path().toStdString());
    binaryTickFile(root / "BTC-USD/2026-10-07/00.bin", {{"1", JournalEpoch}});
    const auto later = root / "BTC-USD/2026-10-08/00.bin";
    binaryTickFile(later, {});
    {
        std::ofstream out(later, std::ios::binary | std::ios::app);
        LogFormat::RecordHeader bad{static_cast<LogFormat::RecordType>(99), JournalEpoch + 86'400'000, 0};
        out.write(reinterpret_cast<const char*>(&bad), sizeof(bad));
    }
    auto source = trading::openTradeHistory("BTC-USD", JournalEpoch, JournalEpoch + 86'400'000, {}, root);
    EXPECT_EQ(collectIds(*source), (std::vector<std::string>{"1"}));
    EXPECT_EQ(source->skippedFiles(), 0u); // An out-of-window file must never be opened.
}

TEST(BacktestCore, R5JournalGapsAreClippedToRequestedWindow) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        for (const auto& [offset, kind] : std::vector<std::pair<int64_t, capture::Kind>>{
            {0, capture::Kind::TransportUp}, {10, capture::Kind::TransportDown}, {150, capture::Kind::TransportUp},
            {450, capture::Kind::TransportDown}, {600, capture::Kind::TransportUp},
            {700, capture::Kind::TransportDown}, {800, capture::Kind::TransportUp}})
            writer.append(journalRecord(JournalEpoch + offset, kind));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch + 100, JournalEpoch + 500);
    EXPECT_FALSE(source.next());
    EXPECT_EQ(source.gaps(), (std::vector<std::pair<int64_t, int64_t>>{
        {JournalEpoch + 100, JournalEpoch + 150}, {JournalEpoch + 450, JournalEpoch + 500}}));
}

TEST(BacktestCore, R6JournalLeadingCoverageGapAppearsInDump) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch + 100, capture::Kind::TransportUp));
        writer.append(journalRecord(JournalEpoch + 120, capture::Kind::Frame, tradesFrame("1", JournalEpoch + 120)));
        writer.close();
    }
    for (const auto from : {JournalEpoch - 1000, JournalEpoch + 10}) {
        trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", from, JournalEpoch + 1000);
        EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1"}));
        EXPECT_EQ(source.gaps(), (std::vector<std::pair<int64_t, int64_t>>{{std::max(from, JournalEpoch), JournalEpoch + 100}}));
    }
    QProcess process;
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07", "--to", "2026-10-08",
        "--journal", temp.path(), "--legacy", temp.path() + "/missing", "--dump"});
    ASSERT_TRUE(process.waitForFinished(10000)); EXPECT_EQ(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardOutput().contains("# gap,1791331200000,1791331200100"));
}

TEST(BacktestCore, R7EmptyRangeFailsWithRootsAndUsageListsParameters) {
    QTemporaryDir temp;
    for (const bool dump : {false, true}) {
        QStringList args{"--product", "BTC-USD", "--from", "2026-10-07", "--to", "2026-10-08",
            "--journal", temp.path() + "/journal", "--legacy", temp.path() + "/legacy"};
        if (dump) args << "--dump";
        QProcess process;
        process.start(QString::fromUtf8(BACKTEST_CLI), args);
        ASSERT_TRUE(process.waitForFinished(10000)); EXPECT_NE(process.exitCode(), 0);
        const auto error = process.readAllStandardError();
        EXPECT_TRUE(error.contains("no trades in range"));
        EXPECT_TRUE(error.contains((temp.path() + "/journal").toUtf8()));
        EXPECT_TRUE(error.contains((temp.path() + "/legacy").toUtf8()));
    }
    QProcess process;
    process.start(QString::fromUtf8(BACKTEST_CLI), QStringList{});
    ASSERT_TRUE(process.waitForFinished(10000));
    const auto usage = process.readAllStandardError();
    EXPECT_TRUE(usage.contains("--spread")); EXPECT_TRUE(usage.contains("--qty")); EXPECT_TRUE(usage.contains("--maxpos"));
}

TEST(BacktestCore, R8EmptyOrUndurableStartFilesSkipWithoutInvalidCursor) {
    for (int tailBytes : {0, 24, 49}) {
        for (const bool following : {false, true}) {
            SCOPED_TRACE(tailBytes);
            SCOPED_TRACE(following);
            QTemporaryDir temp;
            std::filesystem::path first;
            uintmax_t headerBytes = 0;
            {
                capture::Writer writer(journalConfig(temp), journalMetadata());
                writer.append(journalRecord(JournalEpoch, capture::Kind::Frame, tradesFrame("1", JournalEpoch)));
                first = writer.currentPath().toStdString();
                headerBytes = std::filesystem::file_size(first); // Append is buffered; only the header is durable.
                writer.sealSegment();
                if (following) writer.append(journalRecord(JournalEpoch + 3'600'000, capture::Kind::Frame,
                                                            tradesFrame("2", JournalEpoch + 3'600'000)));
                writer.close();
            }
            std::filesystem::resize_file(first, headerBytes + tailBytes);
            trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch + 1000, JournalEpoch + 7'200'000);
            EXPECT_NO_THROW({
                EXPECT_EQ(collectIds(source), (following ? std::vector<std::string>{"2"} : std::vector<std::string>{}));
                EXPECT_FALSE(source.next());
            });
        }
    }
}

TEST(BacktestCore, R2HistoryDedupePersistsAcrossSelectedDaySpans) {
    QTemporaryDir temp;
    const auto root = std::filesystem::path(temp.path().toStdString());
    binaryTickFile(root / "BTC-USD/2026-10-07/00.bin", {{"1", JournalEpoch}});
    binaryTickFile(root / "BTC-USD/2026-10-08/00.bin", {{"1", JournalEpoch + 86'400'000}, {"2", JournalEpoch + 86'400'001}});
    auto source = trading::openTradeHistory("BTC-USD", JournalEpoch, JournalEpoch + 2 * 86'400'000, {}, root);
    EXPECT_EQ(collectIds(*source), (std::vector<std::string>{"1", "2"}));
}

TEST(BacktestCore, R6FirstRecordBeyondStopSlackReportsWholeLeadingGap) {
    QTemporaryDir temp;
    {
        capture::Writer writer(journalConfig(temp), journalMetadata());
        writer.append(journalRecord(JournalEpoch + 120'000, capture::Kind::TransportUp));
        writer.close();
    }
    trading::JournalTradeEventSource source(temp.path().toStdString(), "BTC-USD", JournalEpoch, JournalEpoch + 1000);
    EXPECT_FALSE(source.next());
    EXPECT_EQ(source.gaps(), (std::vector<std::pair<int64_t, int64_t>>{{JournalEpoch, JournalEpoch + 1000}}));
    QProcess process;
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07T00:00:00Z",
        "--to", "2026-10-07T00:00:01Z", "--journal", temp.path(), "--legacy", temp.path() + "/missing", "--dump"});
    ASSERT_TRUE(process.waitForFinished(10000)); EXPECT_NE(process.exitCode(), 0);
    EXPECT_TRUE(process.readAllStandardOutput().contains("# gap,1791331200000,1791331201000"));
}

namespace {
void appendTickTrade(const std::filesystem::path& path, const std::string& id, int64_t ms,
                     LogFormat::TradePayload payload = {100, 1, 1}) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    LogFormat::RecordHeader header{LogFormat::RecordType::Trade, static_cast<uint64_t>(ms),
                                  static_cast<uint32_t>(sizeof(payload) + id.size())};
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    out.write(id.data(), id.size());
}
void corruptPriceFixture(const std::filesystem::path& path) {
    binaryTickFile(path, {});
    appendTickTrade(path, "1", JournalEpoch);
    appendTickTrade(path, std::string("2\x80", 2), JournalEpoch + 1, {9e163, 1, 1});
    appendTickTrade(path, "3", JournalEpoch + 2);
    appendTickTrade(path, "4", JournalEpoch + 3, {99, 1, 1});
    appendTickTrade(path, "5", JournalEpoch + 4, {101, 1, 2});
}
}

TEST(BacktestCore, R2CorruptTickIdsSkipRecordAndKeepFollowingTrade) {
    for (const unsigned char byte : {0x00, 0x20, 0x7f, 0x80, 0xff}) {
        SCOPED_TRACE(static_cast<unsigned>(byte));
        QTemporaryDir temp;
        const auto file = std::filesystem::path(temp.path().toStdString()) / "00.bin";
        binaryTickFile(file, {{"1", JournalEpoch}});
        appendTickTrade(file, std::string("2") + static_cast<char>(byte), JournalEpoch + 1);
        appendTickTrade(file, "3", JournalEpoch + 2);
        trading::TickBinaryTradeEventSource source(file);
        EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1", "3"}));
        EXPECT_EQ(source.skippedFiles(), 0u);
        EXPECT_EQ(source.skippedRecords(), 1u);
    }
}

TEST(BacktestCore, R2TickBodyFieldsValidateBeforeDedupe) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<LogFormat::TradePayload> bad{
        {nan, 1, 1}, {inf, 1, 1}, {0, 1, 1}, {-1, 1, 1},
        {100, nan, 1}, {100, inf, 1}, {100, 0, 1}, {100, -1, 1}, {100, 1, 3}};
    for (size_t i = 0; i < bad.size(); ++i) {
        SCOPED_TRACE(i);
        QTemporaryDir temp;
        const auto file = std::filesystem::path(temp.path().toStdString()) / "00.bin";
        binaryTickFile(file, {{"1", JournalEpoch}});
        appendTickTrade(file, "2", JournalEpoch + 1, bad[i]);
        appendTickTrade(file, "2", JournalEpoch + 2, {100, 1, 0});
        trading::TickBinaryTradeEventSource source(file);
        ASSERT_TRUE(source.next());
        const auto second = source.next();
        ASSERT_TRUE(second);
        EXPECT_EQ(second->trade->tradeId, "2");
        EXPECT_EQ(second->timestampMs, JournalEpoch + 2);
        EXPECT_EQ(second->trade->side, AggressorSide::Unknown);
        EXPECT_FALSE(source.next());
        EXPECT_EQ(source.skippedRecords(), 1u);
    }
}

TEST(BacktestCore, R2HugePriceWithDamagedIdIsSkipped) {
    QTemporaryDir temp;
    const auto file = std::filesystem::path(temp.path().toStdString()) / "00.bin";
    corruptPriceFixture(file); // 9e163 is finite: the damaged ID identifies this real-data failure.
    trading::TickBinaryTradeEventSource source(file);
    EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1", "3", "4", "5"}));
    EXPECT_EQ(source.skippedRecords(), 1u);
    EXPECT_EQ(source.skippedFiles(), 0u);
}

TEST(BacktestCore, R2CliCorruptTickBodyCannotPoisonRealizedPnl) {
    QTemporaryDir temp;
    const auto file = std::filesystem::path(temp.path().toStdString()) / "BTC-USD/2026-10-07/00.bin";
    corruptPriceFixture(file);
    QProcess process;
    process.start(QString::fromUtf8(BACKTEST_CLI), {"--product", "BTC-USD", "--from", "2026-10-07",
        "--to", "2026-10-08", "--journal", temp.path() + "/missing", "--legacy", temp.path()});
    ASSERT_TRUE(process.waitForFinished(10000));
    ASSERT_EQ(process.exitCode(), 0);
    const auto output = QString::fromUtf8(process.readAllStandardOutput());
    bool ok = false;
    const double pnl = output.section("realized_pnl=", 1, 1).section(' ', 0, 0).toDouble(&ok);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(std::isfinite(pnl));
    EXPECT_LT(std::abs(pnl), 1.0);
    EXPECT_TRUE(output.contains("events=4 "));
    const auto error = process.readAllStandardError();
    EXPECT_TRUE(error.contains("skipped_files=0 skipped_records=1"));
    EXPECT_TRUE(error.contains("00.bin"));
    EXPECT_TRUE(error.contains("offset="));
}

TEST(BacktestCore, R2PartialTickTailsAreCleanEndsWithoutSkipCounters) {
    for (int part = 0; part < 3; ++part) {
        SCOPED_TRACE(part);
        QTemporaryDir temp;
        const auto file = std::filesystem::path(temp.path().toStdString()) / "00.bin";
        binaryTickFile(file, {{"1", JournalEpoch}});
        {
            std::ofstream out(file, std::ios::binary | std::ios::app);
            LogFormat::RecordHeader h{LogFormat::RecordType::Trade, JournalEpoch + 1,
                                     sizeof(LogFormat::TradePayload) + 3};
            out.write(reinterpret_cast<const char*>(&h), part == 0 ? 5 : sizeof(h));
            if (part > 0) {
                LogFormat::TradePayload payload{100, 1, 1};
                out.write(reinterpret_cast<const char*>(&payload), part == 1 ? 8 : sizeof(payload));
                if (part == 2) out << '2';
            }
        }
        trading::TickBinaryTradeEventSource source(file);
        EXPECT_EQ(collectIds(source), (std::vector<std::string>{"1"}));
        EXPECT_EQ(source.skippedFiles(), 0u);
        EXPECT_EQ(source.skippedRecords(), 0u);
        QProcess process;
        process.start(QString::fromUtf8(BACKTEST_CLI), {QString::fromStdString(file.string()), "BTC-USD"});
        ASSERT_TRUE(process.waitForFinished(10000));
        EXPECT_EQ(process.exitCode(), 0);
        EXPECT_TRUE(process.readAllStandardError().isEmpty());
    }
}

TEST(BacktestCore, R2EmptyPositionalInputsKeepZeroEventSuccess) {
    QTemporaryDir temp;
    const auto root = std::filesystem::path(temp.path().toStdString());
    std::ofstream(root / "empty.csv").close();
    binaryTickFile(root / "empty.bin", {});
    std::filesystem::create_directory(root / "empty-dir");
    for (const auto& name : {"empty.csv", "empty.bin", "empty-dir"}) {
        SCOPED_TRACE(name);
        QProcess process;
        process.start(QString::fromUtf8(BACKTEST_CLI), {temp.path() + '/' + name, "BTC-USD"});
        ASSERT_TRUE(process.waitForFinished(10000));
        EXPECT_EQ(process.exitCode(), 0);
        EXPECT_TRUE(process.readAllStandardOutput().contains("events=0 "));
        EXPECT_TRUE(process.readAllStandardError().isEmpty());
    }
}
