#include "HeatmapTwapStreamer.hpp"
#include "ServerDataModel.hpp"
#include "../marketdata/cache/DataCache.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>
#include <QProcessEnvironment>
#include <algorithm>
#include <chrono>

namespace {
constexpr int64_t kMsPerSecond = 1000;
}

HeatmapTwapStreamer::HeatmapTwapStreamer(ServerDataModel& model, QObject* parent)
    : QObject(parent)
    , m_model(model) {
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &HeatmapTwapStreamer::onSample);

    m_timeframesMs = {100, 250, 500, 1000, 2000, 5000, 10000};

    const QByteArray heightEnv = qgetenv("SENTINEL_HEATMAP_GRID");
    bool ok = false;
    const int envHeight = heightEnv.toInt(&ok);
    if (ok && envHeight > 0) {
        m_defaultHeight = envHeight;
    }

    const QByteArray tickEnv = qgetenv("SENTINEL_HEATMAP_TICK_SIZE");
    ok = false;
    const double envTick = tickEnv.toDouble(&ok);
    if (ok && envTick > 0.0) {
        m_defaultTickSize = envTick;
    }
}

void HeatmapTwapStreamer::start() {
    if (!m_timer.isActive()) {
        m_timer.start(m_sampleMs);
        sLog_App("HeatmapTwapStreamer: timer started (" << m_sampleMs << " ms)");
    }
}

void HeatmapTwapStreamer::stop() {
    m_timer.stop();
}

int64_t HeatmapTwapStreamer::alignBucketStart(int64_t nowMs, int64_t timeframeMs) {
    if (timeframeMs <= 0) return nowMs;
    return (nowMs / timeframeMs) * timeframeMs;
}

void HeatmapTwapStreamer::ensureSymbolState(const std::string& symbol, SymbolState& state, double midPrice) {
    if (state.initialized && state.lastMidPrice > 0.0) {
        const double range = state.maxPrice - state.minPrice;
        if (range > 0.0) {
            const double edge = range * m_recenterEdgeFraction;
            if (midPrice > 0.0 &&
                (midPrice < (state.minPrice + edge) || midPrice > (state.maxPrice - edge))) {
                state.pendingReset = true;
                state.initialized = false;
            }
        }
    }

    if (state.initialized) {
        return;
    }

    state.tickSize = state.tickSize > 0.0 ? state.tickSize : m_defaultTickSize;
    state.height = state.height > 0 ? state.height : m_defaultHeight;
    state.rowValues.assign(static_cast<size_t>(state.height), 0.0);

    const double rangeSpan = static_cast<double>(state.height) * state.tickSize;
    const double center = (midPrice > 0.0) ? midPrice : state.lastMidPrice;
    state.minPrice = center - (rangeSpan * 0.5);
    state.maxPrice = state.minPrice + rangeSpan;
    state.lastMidPrice = center;

    state.frames.clear();
    state.frames.reserve(m_timeframesMs.size());
    for (const auto tf : m_timeframesMs) {
        TimeframeState frame;
        frame.timeframeMs = tf;
        frame.bucketStartMs = 0;
        frame.bucketEndMs = 0;
        frame.accum.assign(static_cast<size_t>(state.height), 0.0);
        state.frames.push_back(std::move(frame));
    }

    state.initialized = true;
}

void HeatmapTwapStreamer::onSample() {
    const auto now = std::chrono::system_clock::now();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    const auto symbols = m_model.getSymbolsSnapshot();
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG")) {
        sLog_App("Heatmap sample tick: symbols=" << symbols.size() << " t=" << nowMs);
    }
    for (const auto& symbol : symbols) {
        auto& state = m_symbols[symbol];

        const auto& hotData = m_model.ensureSymbol(symbol);
        const double lastTrade = hotData.lastTradePrice;

        double bestBid = 0.0;
        double bestAsk = 0.0;
        double midGuess = (lastTrade > 0.0) ? lastTrade : 0.0;
        if (midGuess <= 0.0) {
            const double bookMin = hotData.liveBook.getMinPrice();
            const double bookMax = hotData.liveBook.getMaxPrice();
            if (bookMax > bookMin) {
                midGuess = (bookMin + bookMax) * 0.5;
            }
        }
        ensureSymbolState(symbol, state, midGuess);

        hotData.liveBook.accumulateRange(state.minPrice, state.maxPrice, state.tickSize,
                                         state.rowValues, &bestBid, &bestAsk);

        double midPrice = midGuess;
        if (midPrice <= 0.0 && bestBid > 0.0 && bestAsk > 0.0) {
            midPrice = (bestBid + bestAsk) * 0.5;
        }
        if (midPrice > 0.0) {
            state.lastMidPrice = midPrice;
        }

        accumulateForSymbol(symbol, state, nowMs, midPrice, lastTrade);
    }
}

void HeatmapTwapStreamer::accumulateForSymbol(const std::string& symbol,
                                              SymbolState& state,
                                              int64_t nowMs,
                                              double midPrice,
                                              double lastTrade) {
    if (!state.initialized) {
        return;
    }

    if (state.lastSampleMs == 0) {
        state.lastSampleMs = nowMs;
        return;
    }

    int64_t intervalStart = state.lastSampleMs;
    int64_t intervalEnd = nowMs;
    if (intervalEnd <= intervalStart) {
        return;
    }

    for (auto& frame : state.frames) {
        if (frame.timeframeMs <= 0) {
            continue;
        }

        if (frame.bucketStartMs == 0) {
            frame.bucketStartMs = alignBucketStart(intervalStart, frame.timeframeMs);
            frame.bucketEndMs = frame.bucketStartMs + frame.timeframeMs;
        }

        int64_t t0 = intervalStart;
        while (t0 < intervalEnd) {
            const int64_t segmentEnd = std::min(intervalEnd, frame.bucketEndMs);
            const double dtMs = static_cast<double>(segmentEnd - t0);

            for (size_t i = 0; i < frame.accum.size(); ++i) {
                frame.accum[i] += state.rowValues[i] * dtMs;
            }

            t0 = segmentEnd;
            if (segmentEnd >= frame.bucketEndMs) {
                finalizeBucket(symbol, state, frame, lastTrade);
                frame.bucketStartMs = frame.bucketEndMs;
                frame.bucketEndMs = frame.bucketStartMs + frame.timeframeMs;
                std::fill(frame.accum.begin(), frame.accum.end(), 0.0);
            }
        }
    }

    state.lastSampleMs = nowMs;

    Q_UNUSED(midPrice);
}

void HeatmapTwapStreamer::finalizeBucket(const std::string& symbol,
                                         SymbolState& state,
                                         TimeframeState& frame,
                                         double lastTrade) {
    if (frame.timeframeMs <= 0) {
        return;
    }

    const double denom = static_cast<double>(frame.timeframeMs);
    std::vector<double> twap(frame.accum.size(), 0.0);
    for (size_t i = 0; i < frame.accum.size(); ++i) {
        twap[i] = (denom > 0.0) ? (frame.accum[i] / denom) : 0.0;
    }

    const QByteArray column = toIntensityColumn(twap);
    const bool reset = false;
    state.pendingReset = false;

    static int logCount = 0;
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG") && (++logCount % 10) == 0) {
        sLog_App("Heatmap slice emit: " << QString::fromStdString(symbol)
                 << " tf=" << frame.timeframeMs
                 << " rows=" << column.size()
                 << " reset=" << reset);
    }

    emit heatmapSliceReady(QString::fromStdString(symbol),
                           frame.bucketStartMs,
                           frame.bucketEndMs,
                           frame.timeframeMs,
                           state.minPrice,
                           state.maxPrice,
                           state.tickSize,
                           state.lastMidPrice,
                           lastTrade,
                           column,
                           reset);
}

QByteArray HeatmapTwapStreamer::toIntensityColumn(const std::vector<double>& values) const {
    if (values.empty()) {
        return {};
    }
    double maxValue = 0.0;
    for (const double v : values) {
        if (v > maxValue) {
            maxValue = v;
        }
    }
    const double denom = (maxValue > 0.0) ? maxValue : 1.0;

    QByteArray out;
    out.resize(static_cast<int>(values.size()));
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    for (size_t i = 0; i < values.size(); ++i) {
        const double normalized = std::clamp(values[i] / denom, 0.0, 1.0);
        dst[i] = static_cast<unsigned char>(normalized * 255.0);
    }
    return out;
}
