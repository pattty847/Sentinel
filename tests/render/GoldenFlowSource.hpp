// A data source with no network that only carries flow batches, so a golden scene can feed the
// footprint cells through the same flowBarsReceived path the chart uses live.
#pragma once
#include "datasources/IGridDataSource.hpp"
#include <cmath>
#include <memory>

class GoldenFlowSource final : public IGridDataSource {
public:
    void subscribe(const QString&) override {}
    void unsubscribe(const QString&) override {}
    void requestTpoHistory(const QString&, int64_t, int, int64_t, int, const QString&) override {}
    void cancelTpoHistory(const QString&, const QString&) override {}
    void setCandleHistoryViewport(const QString&, int64_t, qint64, qint64) override {}
    void sendTradeCommand(const trading::TradeCommand&) override {}
    void sendAlgoCommand(const std::string&, const std::string&, const std::string&, const trading::AlgoParams&) override {}
    bool isConnectionActive() const override { return true; }
    const LiveOrderBook& getDirectLiveOrderBook(const std::string&) const override { return book; }

    // `bars` one-minute bars from `firstMs`, 1-dollar rows from `lowPrice`, a fixed deterministic pattern.
    void deliver(int64_t firstMs, int bars, double lowPrice, int rows) {
        auto batch = std::make_shared<SentinelStreamClient::FlowBatch>();
        batch->symbol = "BTC-USD"; batch->timeframeMs = 60000;
        for (int i = 0; i < bars; ++i) {
            auto& bar = batch->bars.emplace_back().bar;
            bar.startMs = firstMs + i * 60000; bar.timeframeMs = 60000; bar.nativeTick = 1; bar.baseScale = 1;
            bar.lowRow = int64_t(lowPrice);
            for (int r = 0; r < rows; ++r) {
                const auto bid = int64_t(30 + 28 * std::sin(r * 0.9 + i)), ask = int64_t(30 + 28 * std::cos(r * 0.7 + i * 1.3));
                bar.rows.push_back({int16_t(r), bid, ask, 1, 1});
                bar.sellVol += bid; bar.buyVol += ask;
                bar.maxRowVol = std::max({bar.maxRowVol, bid, ask});
            }
            bar.delta = bar.buyVol - bar.sellVol;
        }
        emit flowBarsReceived(batch);
    }
    LiveOrderBook book;
};
