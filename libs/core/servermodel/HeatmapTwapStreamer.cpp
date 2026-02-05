#include "HeatmapTwapStreamer.hpp"
#include "IHeatmapDataSource.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr int64_t kMsPerSecond = 1000;
}

HeatmapTwapStreamer::HeatmapTwapStreamer(IHeatmapDataSource& model,
                                         const ServerHeatmapConfig& config,
                                         QObject* parent)
    : QObject(parent)
    , m_model(model)
    , m_config(config) {
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &HeatmapTwapStreamer::onSample);

    m_timeframesMs = m_config.timeframesMs;
    if (m_timeframesMs.empty()) {
        m_timeframesMs = {1000, 60000, 3600000, 86400000};
    }

    if (m_config.gridWidth > 0) {
        m_defaultWidth = m_config.gridWidth;
    }
    if (m_config.gridHeight > 0) {
        m_defaultHeight = m_config.gridHeight;
    }
    if (m_config.tickSize > 0.0) {
        m_defaultTickSize = m_config.tickSize;
        m_fixedTickSize = true;
    } else {
        m_fixedTickSize = false;
    }

    if (m_config.activeTimeframeMs > 0) {
        m_activeTimeframeMs = m_config.activeTimeframeMs;
    } else if (!m_timeframesMs.empty()) {
        m_activeTimeframeMs = m_timeframesMs.front();
    }

    if (m_config.recenterDelta > 0.0) {
        m_recenterDelta = m_config.recenterDelta;
    }
    if (m_config.bandFast > 0.0) {
        m_bandFast = m_config.bandFast;
    }
    if (m_config.bandMedium > 0.0) {
        m_bandMedium = m_config.bandMedium;
    }
    if (m_config.bandSlow > 0.0) {
        m_bandSlow = m_config.bandSlow;
    }

    m_intensity = parseIntensityConfig();
}

HeatmapTwapStreamer::IntensityConfig HeatmapTwapStreamer::parseIntensityConfig() const {
    IntensityConfig out;
    std::string mode = m_config.intensityMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
    if (mode == "linear") {
        out.mode = NormalizeMode::Linear;
    } else if (mode == "power" || mode == "pow") {
        out.mode = NormalizeMode::Power;
    } else {
        out.mode = NormalizeMode::Log;
    }

    std::string maxMode = m_config.intensityMaxMode;
    std::transform(maxMode.begin(), maxMode.end(), maxMode.begin(), ::tolower);
    out.useRunningMax = (maxMode != "column");

    if (m_config.intensityMaxDecay > 0.0 && m_config.intensityMaxDecay <= 1.0) {
        out.runningMaxDecay = m_config.intensityMaxDecay;
    }
    if (m_config.intensityLogScale > 0.0) {
        out.logScale = m_config.intensityLogScale;
    }
    if (m_config.intensityPower > 0.0 && m_config.intensityPower <= 1.0) {
        out.powerExp = m_config.intensityPower;
    }
    if (m_config.intensityFloor >= 0.0 && m_config.intensityFloor <= 1.0) {
        out.intensityFloor = m_config.intensityFloor;
    }
    return out;
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
    if (state.initialized) {
        return;
    }

    state.tickSize = state.tickSize > 0.0 ? state.tickSize : m_defaultTickSize;
    state.height = state.height > 0 ? state.height : m_defaultHeight;
    state.rowValuesBid.assign(static_cast<size_t>(state.height), 0.0);
    state.rowValuesAsk.assign(static_cast<size_t>(state.height), 0.0);

    const int64_t bandTf = (m_activeTimeframeMs > 0)
        ? m_activeTimeframeMs
        : (m_timeframesMs.empty() ? 1000 : m_timeframesMs.front());
    const double center = (midPrice > 0.0) ? midPrice : state.lastRecenterMid;
    if (center > 0.0) {
        if (m_fixedTickSize && state.tickSize > 0.0) {
            const double rangeSpan = static_cast<double>(state.height) * state.tickSize;
            state.minPrice = center - (rangeSpan * 0.5);
            state.maxPrice = center + (rangeSpan * 0.5);
        } else {
            const double bandPct = bandForTimeframe(bandTf);
            const double rangeSpan = center * bandPct * 2.0;
            state.tickSize = (rangeSpan > 0.0) ? (rangeSpan / static_cast<double>(state.height)) : state.tickSize;
            state.minPrice = center - (rangeSpan * 0.5);
            state.maxPrice = center + (rangeSpan * 0.5);
        }
        state.lastRecenterMid = center;
    } else {
        const double rangeSpan = static_cast<double>(state.height) * state.tickSize;
        state.minPrice = 0.0;
        state.maxPrice = state.minPrice + rangeSpan;
    }

    state.frames.clear();
    state.frames.reserve(m_timeframesMs.size());
    for (const auto tf : m_timeframesMs) {
        if (m_activeTimeframeMs > 0 && tf != m_activeTimeframeMs) {
            continue;
        }
        TimeframeState frame;
        frame.timeframeMs = tf;
        frame.bucketStartMs = 0;
        frame.bucketEndMs = 0;
        frame.accumBid.assign(static_cast<size_t>(state.height), 0.0);
        frame.accumAsk.assign(static_cast<size_t>(state.height), 0.0);
        state.frames.push_back(std::move(frame));
    }

    state.initialized = true;
}

double HeatmapTwapStreamer::bandForTimeframe(int64_t timeframeMs) const {
    if (timeframeMs <= 1000) {
        return m_bandFast;
    }
    if (timeframeMs <= 60000) {
        return m_bandMedium;
    }
    return m_bandSlow;
}

void HeatmapTwapStreamer::applyBandRange(SymbolState& state, double midPrice, int64_t timeframeMs) {
    if (midPrice <= 0.0 || state.height <= 0) {
        return;
    }
    if (m_fixedTickSize && state.tickSize > 0.0) {
        const double rangeSpan = static_cast<double>(state.height) * state.tickSize;
        if (rangeSpan <= 0.0) {
            return;
        }
        state.minPrice = midPrice - (rangeSpan * 0.5);
        state.maxPrice = midPrice + (rangeSpan * 0.5);
        state.lastRecenterMid = midPrice;
        return;
    }

    const double bandPct = bandForTimeframe(timeframeMs);
    const double rangeSpan = midPrice * bandPct * 2.0;
    if (rangeSpan <= 0.0) {
        return;
    }
    state.tickSize = rangeSpan / static_cast<double>(state.height);
    if (state.tickSize <= 0.0) {
        return;
    }
    state.minPrice = midPrice - (rangeSpan * 0.5);
    state.maxPrice = midPrice + (rangeSpan * 0.5);
    state.lastRecenterMid = midPrice;
}

void HeatmapTwapStreamer::onSample() {
    const int64_t nowMs = m_model.exchangeNowMs();

    const auto symbols = m_model.getSymbolsSnapshot();
    if (m_config.debugSliceLog) {
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

        hotData.liveBook.accumulateRangeSplit(state.minPrice, state.maxPrice, state.tickSize,
                                              state.rowValuesBid, state.rowValuesAsk,
                                              &bestBid, &bestAsk);

        double midPrice = midGuess;
        if (midPrice <= 0.0 && bestBid > 0.0 && bestAsk > 0.0) {
            midPrice = (bestBid + bestAsk) * 0.5;
        }
        if (midPrice > 0.0) {
            state.lastMidPrice = midPrice;
        }

        if (state.initialized && midPrice > 0.0 && state.lastRecenterMid <= 0.0) {
            state.pendingReset = true;
        }

        if (state.initialized && midPrice > 0.0 && state.lastRecenterMid > 0.0) {
            const double deltaPct = std::abs(midPrice - state.lastRecenterMid) / state.lastRecenterMid;
            if (deltaPct >= m_recenterDelta) {
                state.pendingReset = true;
            }
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
        if (m_activeTimeframeMs > 0 && frame.timeframeMs != m_activeTimeframeMs) {
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

            for (size_t i = 0; i < frame.accumBid.size(); ++i) {
                frame.accumBid[i] += state.rowValuesBid[i] * dtMs;
                frame.accumAsk[i] += state.rowValuesAsk[i] * dtMs;
            }

            t0 = segmentEnd;
            if (segmentEnd >= frame.bucketEndMs) {
                finalizeBucket(symbol, state, frame, lastTrade, midPrice);
                frame.bucketStartMs = frame.bucketEndMs;
                frame.bucketEndMs = frame.bucketStartMs + frame.timeframeMs;
                std::fill(frame.accumBid.begin(), frame.accumBid.end(), 0.0);
                std::fill(frame.accumAsk.begin(), frame.accumAsk.end(), 0.0);
            }
        }
    }

    state.lastSampleMs = nowMs;

    Q_UNUSED(midPrice);
}

void HeatmapTwapStreamer::finalizeBucket(const std::string& symbol,
                                         SymbolState& state,
                                         TimeframeState& frame,
                                         double lastTrade,
                                         double midPrice) {
    if (frame.timeframeMs <= 0) {
        return;
    }

    bool reset = false;
    if (state.pendingReset && midPrice > 0.0) {
        applyBandRange(state, midPrice, frame.timeframeMs);
        reset = true;
        state.pendingReset = false;
        state.runningMaxBid = 0.0;
        state.runningMaxAsk = 0.0;
        std::lock_guard<std::mutex> lock(m_historyMutex);
        state.historyByTf.clear();
    }

    const double denom = static_cast<double>(frame.timeframeMs);
    std::vector<double> twapBid(frame.accumBid.size(), 0.0);
    std::vector<double> twapAsk(frame.accumAsk.size(), 0.0);
    for (size_t i = 0; i < frame.accumBid.size(); ++i) {
        twapBid[i] = (denom > 0.0) ? (frame.accumBid[i] / denom) : 0.0;
        twapAsk[i] = (denom > 0.0) ? (frame.accumAsk[i] / denom) : 0.0;
    }

    const QByteArray column = toIntensityColumnSigned(state, twapBid, twapAsk);
    double liquidityScale = 1.0;
    const QByteArray liquidityColumn = toLiquidityColumn(twapBid, twapAsk, liquidityScale);

    static int logCount = 0;
    if (m_config.debugSliceLog && (++logCount % 10) == 0) {
        sLog_App("Heatmap slice emit: " << QString::fromStdString(symbol)
                 << " tf=" << frame.timeframeMs
                 << " rows=" << column.size()
                 << " grid=" << m_defaultWidth << "x" << state.height
                 << " reset=" << reset);
    }

    storeHistory(state,
                 frame.timeframeMs,
                 column,
                 liquidityColumn,
                 liquidityScale,
                 state.minPrice,
                 state.maxPrice,
                 state.tickSize,
                 frame.bucketStartMs,
                 frame.bucketEndMs);

    if (m_activeTimeframeMs <= 0 || frame.timeframeMs == m_activeTimeframeMs) {
        HeatmapSlice slice;
        slice.symbol = QString::fromStdString(symbol);
        slice.bucketStartMs = frame.bucketStartMs;
        slice.bucketEndMs = frame.bucketEndMs;
        slice.timeframeMs = frame.timeframeMs;
        slice.gridWidth = m_defaultWidth;
        slice.gridHeight = state.height;
        slice.minPrice = state.minPrice;
        slice.maxPrice = state.maxPrice;
        slice.tickSize = state.tickSize;
        slice.midPrice = state.lastMidPrice;
        slice.lastTrade = lastTrade;
        slice.format = QStringLiteral("u16");
        slice.column = column;
        slice.liquidityColumn = liquidityColumn;
        slice.liquidityScale = liquidityScale;
        slice.reset = reset;
        emit heatmapSliceReady(slice);
    }
}

void HeatmapTwapStreamer::storeHistory(SymbolState& state,
                                       int64_t timeframeMs,
                                       const QByteArray& column,
                                       const QByteArray& liquidityColumn,
                                       double liquidityScale,
                                       double minPrice,
                                       double maxPrice,
                                       double tickSize,
                                       int64_t bucketStartMs,
                                       int64_t bucketEndMs) {
    if (timeframeMs <= 0 || column.isEmpty() || m_defaultWidth <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_historyMutex);
    auto& ring = state.historyByTf[timeframeMs];
    if (ring.capacity <= 0) {
        ring.capacity = m_defaultWidth;
        ring.columns.assign(static_cast<size_t>(ring.capacity), HistoryColumn{});
        ring.writeIndex = 0;
        ring.count = 0;
    } else if (ring.capacity != m_defaultWidth) {
        ring.capacity = m_defaultWidth;
        ring.columns.assign(static_cast<size_t>(ring.capacity), HistoryColumn{});
        ring.writeIndex = 0;
        ring.count = 0;
    }

    HistoryColumn entry;
    entry.bucketStartMs = bucketStartMs;
    entry.bucketEndMs = bucketEndMs;
    entry.minPrice = minPrice;
    entry.maxPrice = maxPrice;
    entry.tickSize = tickSize;
    entry.intensity = column;
    entry.liquidity = liquidityColumn;
    entry.liquidityScale = liquidityScale;

    ring.columns[static_cast<size_t>(ring.writeIndex)] = std::move(entry);
    ring.writeIndex = (ring.writeIndex + 1) % ring.capacity;
    ring.count = std::min(ring.count + 1, ring.capacity);
}

bool HeatmapTwapStreamer::fetchHistory(const std::string& symbol,
                                       int64_t timeframeMs,
                                       int64_t endTimeMs,
                                       int count,
                                       int& outGridWidth,
                                       int& outGridHeight,
                                       std::vector<HistoryColumn>& out) const {
    if (timeframeMs <= 0 || count <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_historyMutex);
    auto it = m_symbols.find(symbol);
    if (it == m_symbols.end()) {
        return false;
    }
    const auto& state = it->second;
    auto ringIt = state.historyByTf.find(timeframeMs);
    if (ringIt == state.historyByTf.end()) {
        return false;
    }
    const auto& ring = ringIt->second;
    if (ring.count <= 0 || ring.capacity <= 0) {
        return false;
    }

    const int requested = std::min(count, ring.count);
    out.clear();
    out.reserve(static_cast<size_t>(requested));
    outGridWidth = ring.capacity;
    outGridHeight = state.height;

    const int latestIndex = (ring.writeIndex - 1 + ring.capacity) % ring.capacity;
    int collected = 0;
    for (int i = 0; i < ring.count && collected < requested; ++i) {
        const int idx = (latestIndex - i + ring.capacity) % ring.capacity;
        const auto& col = ring.columns[static_cast<size_t>(idx)];
        if (endTimeMs > 0 && col.bucketStartMs > endTimeMs) {
            continue;
        }
        out.push_back(col);
        ++collected;
    }

    if (out.empty()) {
        return false;
    }
    std::reverse(out.begin(), out.end());
    return true;
}

QByteArray HeatmapTwapStreamer::toIntensityColumnSigned(SymbolState& state,
                                                        const std::vector<double>& bidValues,
                                                        const std::vector<double>& askValues) {
    if (bidValues.empty() || askValues.empty()) {
        return {};
    }
    const IntensityConfig& cfg = m_intensity;
    double maxBid = 0.0;
    double maxAsk = 0.0;
    int nonZeroBid = 0;
    int nonZeroAsk = 0;
    for (const double v : bidValues) {
        if (v > 0.0) {
            ++nonZeroBid;
            if (v > maxBid) {
                maxBid = v;
            }
        }
    }
    for (const double v : askValues) {
        if (v > 0.0) {
            ++nonZeroAsk;
            if (v > maxAsk) {
                maxAsk = v;
            }
        }
    }
    if (cfg.useRunningMax) {
        if (state.runningMaxBid <= 0.0) {
            state.runningMaxBid = maxBid;
        } else {
            state.runningMaxBid = std::max(maxBid, state.runningMaxBid * cfg.runningMaxDecay);
        }
        if (state.runningMaxAsk <= 0.0) {
            state.runningMaxAsk = maxAsk;
        } else {
            state.runningMaxAsk = std::max(maxAsk, state.runningMaxAsk * cfg.runningMaxDecay);
        }
    }

    const double denomBid = (cfg.useRunningMax ? state.runningMaxBid : maxBid);
    const double denomAsk = (cfg.useRunningMax ? state.runningMaxAsk : maxAsk);
    const double safeDenomBid = (denomBid > 0.0) ? denomBid : 1.0;
    const double safeDenomAsk = (denomAsk > 0.0) ? denomAsk : 1.0;

    static int logCount = 0;
    if (m_config.debugSliceLog && (++logCount % 20) == 0) {
        sLog_App("Heatmap column stats: bids=" << nonZeroBid
                 << " asks=" << nonZeroAsk
                 << " maxBid=" << maxBid
                 << " maxAsk=" << maxAsk);
    }

    QByteArray out;
    const size_t height = bidValues.size();
    out.resize(static_cast<int>(height * sizeof(uint16_t)));
    auto* dst = reinterpret_cast<uint16_t*>(out.data());
    for (size_t i = 0; i < height; ++i) {
        const double bidValue = bidValues[i];
        const double askValue = askValues[i];
        double bidNorm = 0.0;
        double askNorm = 0.0;
        if (bidValue > 0.0) {
            if (cfg.mode == NormalizeMode::Log) {
                bidNorm = std::log1p(bidValue * cfg.logScale) / std::log1p(safeDenomBid * cfg.logScale);
            } else if (cfg.mode == NormalizeMode::Power) {
                bidNorm = std::pow(bidValue / safeDenomBid, cfg.powerExp);
            } else {
                bidNorm = bidValue / safeDenomBid;
            }
        }
        if (askValue > 0.0) {
            if (cfg.mode == NormalizeMode::Log) {
                askNorm = std::log1p(askValue * cfg.logScale) / std::log1p(safeDenomAsk * cfg.logScale);
            } else if (cfg.mode == NormalizeMode::Power) {
                askNorm = std::pow(askValue / safeDenomAsk, cfg.powerExp);
            } else {
                askNorm = askValue / safeDenomAsk;
            }
        }
        bidNorm = std::clamp(bidNorm, 0.0, 1.0);
        askNorm = std::clamp(askNorm, 0.0, 1.0);
        if (bidNorm < cfg.intensityFloor) {
            bidNorm = 0.0;
        }
        if (askNorm < cfg.intensityFloor) {
            askNorm = 0.0;
        }
        uint16_t encoded = 0;
        if (askNorm > 0.0) {
            const double scaled = std::round(askNorm * 32767.0);
            encoded = static_cast<uint16_t>(0x8000u + std::clamp(scaled, 0.0, 32767.0));
        } else if (bidNorm > 0.0) {
            const double scaled = std::round(bidNorm * 32767.0);
            encoded = static_cast<uint16_t>(std::clamp(scaled, 0.0, 32767.0));
        }
        dst[i] = qToLittleEndian(encoded);
    }
    return out;
}

QByteArray HeatmapTwapStreamer::toLiquidityColumn(const std::vector<double>& bidValues,
                                                  const std::vector<double>& askValues,
                                                  double& outScale) const {
    if (bidValues.empty() || askValues.empty()) {
        outScale = 1.0;
        return {};
    }

    const size_t height = bidValues.size();
    double maxValue = 0.0;
    for (size_t i = 0; i < height; ++i) {
        const double ask = askValues[i];
        const double bid = bidValues[i];
        const double value = (ask > 0.0) ? ask : bid;
        if (value > maxValue) {
            maxValue = value;
        }
    }

    const double scale = (maxValue > 0.0) ? (maxValue / 65535.0) : 1.0;
    outScale = (scale > 0.0) ? scale : 1.0;

    QByteArray out;
    out.resize(static_cast<int>(height * sizeof(uint16_t)));
    auto* dst = reinterpret_cast<uint16_t*>(out.data());
    for (size_t i = 0; i < height; ++i) {
        const double ask = askValues[i];
        const double bid = bidValues[i];
        const double value = (ask > 0.0) ? ask : bid;
        uint16_t scaled = 0;
        if (value > 0.0) {
            const double raw = std::floor(value / outScale);
            const double clamped = std::clamp(raw, 0.0, 65535.0);
            scaled = static_cast<uint16_t>(clamped);
        }
        dst[i] = qToLittleEndian(scaled);
    }
    return out;
}
