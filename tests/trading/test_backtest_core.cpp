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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
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
    const fs::path filePath = tempDir / "00.bin";

    {
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());

        LogFormat::FileHeader fileHeader;
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
        recordHeader.payload_len = sizeof(tradePayload);

        out.write(reinterpret_cast<const char*>(&recordHeader), sizeof(recordHeader));
        out.write(reinterpret_cast<const char*>(&tradePayload), sizeof(tradePayload));
    }

    trading::TickBinaryTradeEventSource source(tempDir, "BTC-USD");
    auto event = source.next();
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->trade.has_value());
    EXPECT_EQ(event->trade->symbol, "BTC-USD");
    EXPECT_EQ(event->trade->timestampMs, 2000);
    EXPECT_DOUBLE_EQ(event->trade->price, 123.45);
    EXPECT_DOUBLE_EQ(event->trade->qty, 0.75);

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
