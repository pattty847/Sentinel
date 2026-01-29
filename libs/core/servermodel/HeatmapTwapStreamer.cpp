#include "HeatmapTwapStreamer.hpp"
#include "ServerDataModel.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>
#include <QProcessEnvironment>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>

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

    const QByteArray tfEnv = qgetenv("SENTINEL_HEATMAP_TF");
    ok = false;
    const int64_t envTf = tfEnv.toLongLong(&ok);
    if (ok && envTf > 0) {
        m_activeTimeframeMs = envTf;
    }

    const QByteArray deltaEnv = qgetenv("SENTINEL_HEATMAP_RECENTER_DELTA");
    ok = false;
    const double envDelta = deltaEnv.toDouble(&ok);
    if (ok && envDelta > 0.0) {
        m_recenterDelta = envDelta;
    }

    const QByteArray bandFastEnv = qgetenv("SENTINEL_HEATMAP_BAND_FAST");
    ok = false;
    const double bandFast = bandFastEnv.toDouble(&ok);
    if (ok && bandFast > 0.0) {
        m_bandFast = bandFast;
    }

    const QByteArray bandMedEnv = qgetenv("SENTINEL_HEATMAP_BAND_MED");
    ok = false;
    const double bandMed = bandMedEnv.toDouble(&ok);
    if (ok && bandMed > 0.0) {
        m_bandMedium = bandMed;
    }

    const QByteArray bandSlowEnv = qgetenv("SENTINEL_HEATMAP_BAND_SLOW");
    ok = false;
    const double bandSlow = bandSlowEnv.toDouble(&ok);
    if (ok && bandSlow > 0.0) {
        m_bandSlow = bandSlow;
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
        const double bandPct = bandForTimeframe(bandTf);
        const double rangeSpan = center * bandPct * 2.0;
        state.tickSize = (rangeSpan > 0.0) ? (rangeSpan / static_cast<double>(state.height)) : state.tickSize;
        state.minPrice = center - (rangeSpan * 0.5);
        state.maxPrice = center + (rangeSpan * 0.5);
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
                           liquidityColumn,
                           liquidityScale,
                           reset);
}

QByteArray HeatmapTwapStreamer::toIntensityColumnSigned(SymbolState& state,
                                                        const std::vector<double>& bidValues,
                                                        const std::vector<double>& askValues) {
    if (bidValues.empty() || askValues.empty()) {
        return {};
    }
    QByteArray modeEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_MODE").toLower();
    enum class NormalizeMode { Linear, Log, Power };
    NormalizeMode mode = NormalizeMode::Log;
    if (modeEnv == "linear") {
        mode = NormalizeMode::Linear;
    } else if (modeEnv == "power" || modeEnv == "pow") {
        mode = NormalizeMode::Power;
    } else if (modeEnv == "log") {
        mode = NormalizeMode::Log;
    }

    const QByteArray maxModeEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_MAX_MODE").toLower();
    const bool useRunningMax = (maxModeEnv != "column");
    double runningMaxDecay = 0.995;
    const QByteArray decayEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_MAX_DECAY");
    bool ok = false;
    const double decayOverride = decayEnv.toDouble(&ok);
    if (ok && decayOverride > 0.0 && decayOverride <= 1.0) {
        runningMaxDecay = decayOverride;
    }

    double logScale = 1000.0;
    const QByteArray logScaleEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_LOG_SCALE");
    const double logScaleOverride = logScaleEnv.toDouble(&ok);
    if (ok && logScaleOverride > 0.0) {
        logScale = logScaleOverride;
    }

    double powerExp = 0.4;
    const QByteArray powerEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_POWER");
    const double powerOverride = powerEnv.toDouble(&ok);
    if (ok && powerOverride > 0.0 && powerOverride <= 1.0) {
        powerExp = powerOverride;
    }

    double intensityFloor = 0.001;
    const QByteArray floorEnv = qgetenv("SENTINEL_HEATMAP_INTENSITY_FLOOR");
    const double floorOverride = floorEnv.toDouble(&ok);
    if (ok && floorOverride >= 0.0 && floorOverride <= 1.0) {
        intensityFloor = floorOverride;
    }
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
    if (useRunningMax) {
        if (state.runningMaxBid <= 0.0) {
            state.runningMaxBid = maxBid;
        } else {
            state.runningMaxBid = std::max(maxBid, state.runningMaxBid * runningMaxDecay);
        }
        if (state.runningMaxAsk <= 0.0) {
            state.runningMaxAsk = maxAsk;
        } else {
            state.runningMaxAsk = std::max(maxAsk, state.runningMaxAsk * runningMaxDecay);
        }
    }

    const double denomBid = (useRunningMax ? state.runningMaxBid : maxBid);
    const double denomAsk = (useRunningMax ? state.runningMaxAsk : maxAsk);
    const double safeDenomBid = (denomBid > 0.0) ? denomBid : 1.0;
    const double safeDenomAsk = (denomAsk > 0.0) ? denomAsk : 1.0;

    static int logCount = 0;
    if (qEnvironmentVariableIsSet("SENTINEL_HEATMAP_SLICE_LOG") && (++logCount % 20) == 0) {
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
            if (mode == NormalizeMode::Log) {
                bidNorm = std::log1p(bidValue * logScale) / std::log1p(safeDenomBid * logScale);
            } else if (mode == NormalizeMode::Power) {
                bidNorm = std::pow(bidValue / safeDenomBid, powerExp);
            } else {
                bidNorm = bidValue / safeDenomBid;
            }
        }
        if (askValue > 0.0) {
            if (mode == NormalizeMode::Log) {
                askNorm = std::log1p(askValue * logScale) / std::log1p(safeDenomAsk * logScale);
            } else if (mode == NormalizeMode::Power) {
                askNorm = std::pow(askValue / safeDenomAsk, powerExp);
            } else {
                askNorm = askValue / safeDenomAsk;
            }
        }
        bidNorm = std::clamp(bidNorm, 0.0, 1.0);
        askNorm = std::clamp(askNorm, 0.0, 1.0);
        if (bidNorm < intensityFloor) {
            bidNorm = 0.0;
        }
        if (askNorm < intensityFloor) {
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
