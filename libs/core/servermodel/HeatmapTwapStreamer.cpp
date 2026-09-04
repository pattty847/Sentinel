#include "HeatmapTwapStreamer.hpp"
#include "IHeatmapDataSource.hpp"
#include "SentinelLogging.hpp"
#include <QByteArray>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

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

    // Persistence is an explicit runtime contract. Refuse startup when another
    // process owns the store; continuing would report a healthy server while
    // silently dropping every finalized column.
    if (m_config.persistenceEnabled) {
        HeatmapColumnStore::Config storeCfg;
        storeCfg.fsyncEveryNRecords = std::max(1, m_config.persistenceFsyncEveryNRecords);
        storeCfg.fsyncEveryMs = std::max(1, m_config.persistenceFsyncEveryMs);
        auto store = std::make_unique<HeatmapColumnStore>(m_config.persistenceDir, storeCfg);
        if (store->acquireLock()) {
            sLog_App("HeatmapTwapStreamer: persistence enabled at "
                     << QString::fromStdString(m_config.persistenceDir)
                     << " (active tf=" << m_activeTimeframeMs << " ms)");
            // Phase 5: enforce retention up front so disk doesn't grow
            // unbounded across long server runs. retentionDays <= 0 disables.
            if (m_config.persistenceRetentionDays > 0) {
                const int removed = store->enforceRetention(m_config.persistenceRetentionDays);
                if (removed > 0) {
                    sLog_App("HeatmapTwapStreamer: retention deleted " << removed
                             << " day file(s) older than "
                             << m_config.persistenceRetentionDays << " days");
                }
            }
            m_columnStore = std::move(store);
        } else {
            throw std::runtime_error(
                "Heatmap persistence is enabled but the data-directory lock could not be acquired: " +
                m_config.persistenceDir);
        }
    }
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

int HeatmapTwapStreamer::primeRingFromDisk(const std::string& symbol) {
    if (!m_columnStore || m_activeTimeframeMs <= 0 || m_defaultWidth <= 0) {
        return 0;
    }

    std::vector<HeatmapColumnStore::LoadedColumn> loaded;
    if (!m_columnStore->loadRecent(symbol, m_activeTimeframeMs,
                                   m_defaultWidth, loaded) || loaded.empty()) {
        return 0;
    }

    // Decide gridHeight from the on-disk record; refuse to prime if the disk
    // shape doesn't match what this server is configured to emit (config drift
    // since the columns were written). The data stays on disk for later use.
    const int diskGridHeight = loaded.back().gridHeight;
    if (diskGridHeight <= 0) {
        return 0;
    }
    if (m_defaultHeight > 0 && diskGridHeight != m_defaultHeight) {
        sLog_Warning("HeatmapTwapStreamer: skipping primeRing for "
                     << QString::fromStdString(symbol)
                     << " — disk gridHeight=" << diskGridHeight
                     << " does not match configured gridHeight=" << m_defaultHeight);
        return 0;
    }

    SymbolState& state = m_symbols[symbol];

    // Initialize SymbolState consistently with the band that wrote these columns.
    // Use the most-recent column's price band so live accumulation starts with
    // the band we ended on.
    const auto& latest = loaded.back();
    state.height = diskGridHeight;
    state.tickSize = (latest.tickSize > 0.0) ? latest.tickSize : state.tickSize;
    state.minPrice = latest.minPrice;
    state.maxPrice = latest.maxPrice;
    state.lastRecenterMid = (latest.minPrice + latest.maxPrice) * 0.5;
    state.lastSampleMs = latest.bucketEndMs;
    state.lastMidPrice = state.lastRecenterMid;
    state.rowValuesBid.assign(static_cast<size_t>(state.height), 0.0);
    state.rowValuesAsk.assign(static_cast<size_t>(state.height), 0.0);
    state.runningMaxBid = 0.0;
    state.runningMaxAsk = 0.0;

    // Initialize the active-timeframe frame so the first live sample slots in.
    state.frames.clear();
    TimeframeState frame;
    frame.timeframeMs = m_activeTimeframeMs;
    // Forming bucket starts at the next bucket after latest.bucketEndMs.
    frame.bucketStartMs = latest.bucketEndMs;
    frame.bucketEndMs   = frame.bucketStartMs + m_activeTimeframeMs;
    frame.accumBid.assign(static_cast<size_t>(state.height), 0.0);
    frame.accumAsk.assign(static_cast<size_t>(state.height), 0.0);
    state.frames.push_back(std::move(frame));
    state.initialized = true;

    // Push columns into the in-RAM ring in chronological order so writeIndex
    // ends at the slot AFTER the latest record, matching live append order.
    int primed = 0;
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        auto& ring = state.historyByTf[m_activeTimeframeMs];
        if (ring.capacity != m_defaultWidth) {
            ring.capacity = m_defaultWidth;
            ring.columns.assign(static_cast<size_t>(ring.capacity), HistoryColumn{});
            ring.writeIndex = 0;
            ring.count = 0;
        }
        for (const auto& src : loaded) {
            HistoryColumn entry;
            entry.bucketStartMs = src.bucketStartMs;
            entry.bucketEndMs   = src.bucketEndMs;
            entry.minPrice      = src.minPrice;
            entry.maxPrice      = src.maxPrice;
            entry.tickSize      = src.tickSize;
            entry.intensity     = QByteArray(reinterpret_cast<const char*>(src.intensity.data()),
                                             static_cast<int>(src.intensity.size()));
            if (!src.liquidity.empty()) {
                entry.liquidity = QByteArray(reinterpret_cast<const char*>(src.liquidity.data()),
                                             static_cast<int>(src.liquidity.size()));
            }
            entry.liquidityScale = src.liquidityScale;

            ring.columns[static_cast<size_t>(ring.writeIndex)] = std::move(entry);
            ring.writeIndex = (ring.writeIndex + 1) % ring.capacity;
            ring.count = std::min(ring.count + 1, ring.capacity);
            ++primed;
        }
    }

    sLog_App("HeatmapTwapStreamer: primed " << primed
             << " column(s) for " << QString::fromStdString(symbol)
             << " tf=" << m_activeTimeframeMs
             << " from " << QString::fromStdString(m_config.persistenceDir));

    return primed;
}

int64_t HeatmapTwapStreamer::oldestPersistedMs(const std::string& symbol,
                                               int64_t timeframeMs) const {
    if (!m_columnStore) return 0;
    return m_columnStore->oldestPersistedMs(symbol, timeframeMs);
}

int HeatmapTwapStreamer::bootstrapFromDisk(const std::vector<std::string>& symbols) {
    if (!m_columnStore) return 0;
    int total = 0;
    for (const auto& s : symbols) {
        total += primeRingFromDisk(s);
    }
    return total;
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

        emitFormingBucket(symbol, state, frame, nowMs, lastTrade);
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

    const QByteArray column = toIntensityColumnSigned(state, twapBid, twapAsk, true);
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

    storeHistory(symbol,
                 state,
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

void HeatmapTwapStreamer::emitFormingBucket(const std::string& symbol,
                                            SymbolState& state,
                                            const TimeframeState& frame,
                                            int64_t nowMs,
                                            double lastTrade) {
    if (frame.timeframeMs <= 0) {
        return;
    }
    if (m_activeTimeframeMs > 0 && frame.timeframeMs != m_activeTimeframeMs) {
        return;
    }
    if (frame.bucketStartMs <= 0 || frame.bucketEndMs <= frame.bucketStartMs) {
        return;
    }

    const int64_t elapsedMs = std::clamp(nowMs - frame.bucketStartMs, int64_t{1}, frame.timeframeMs);
    if (elapsedMs <= 0) {
        return;
    }

    std::vector<double> twapBid(frame.accumBid.size(), 0.0);
    std::vector<double> twapAsk(frame.accumAsk.size(), 0.0);
    const double denom = static_cast<double>(elapsedMs);
    for (size_t i = 0; i < frame.accumBid.size(); ++i) {
        twapBid[i] = frame.accumBid[i] / denom;
        twapAsk[i] = frame.accumAsk[i] / denom;
    }

    const QByteArray column = toIntensityColumnSigned(state, twapBid, twapAsk, false);
    double liquidityScale = 1.0;
    const QByteArray liquidityColumn = toLiquidityColumn(twapBid, twapAsk, liquidityScale);

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
    slice.reset = false;
    emit heatmapSliceReady(slice);
}

void HeatmapTwapStreamer::storeHistory(const std::string& symbol,
                                       SymbolState& state,
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

    // F1 phase 1.3: persist active-timeframe columns to disk if enabled.
    // Only the active timeframe is persisted in v1 (typically 1m).
    if (m_columnStore &&
        m_activeTimeframeMs > 0 &&
        timeframeMs == m_activeTimeframeMs) {
        persistColumn(symbol,
                      timeframeMs,
                      column,
                      liquidityColumn,
                      liquidityScale,
                      minPrice,
                      maxPrice,
                      tickSize,
                      bucketStartMs,
                      bucketEndMs);
    }
}

void HeatmapTwapStreamer::persistColumn(const std::string& symbol,
                                        int64_t timeframeMs,
                                        const QByteArray& column,
                                        const QByteArray& liquidityColumn,
                                        double liquidityScale,
                                        double minPrice,
                                        double maxPrice,
                                        double tickSize,
                                        int64_t bucketStartMs,
                                        int64_t bucketEndMs) {
    if (!m_columnStore || column.isEmpty() || (column.size() % sizeof(uint16_t)) != 0) {
        return;
    }
    const auto bytesPerCell = static_cast<int>(sizeof(uint16_t));
    const int gridHeight = static_cast<int>(column.size()) / bytesPerCell;
    if (gridHeight <= 0) {
        return;
    }

    const auto* intensity = reinterpret_cast<const uint16_t*>(column.constData());
    const uint16_t* liquidity = nullptr;
    if (!liquidityColumn.isEmpty() && liquidityColumn.size() == column.size()) {
        liquidity = reinterpret_cast<const uint16_t*>(liquidityColumn.constData());
    }

    const auto result = m_columnStore->append(symbol,
                                              timeframeMs,
                                              gridHeight,
                                              bucketStartMs,
                                              bucketEndMs,
                                              minPrice,
                                              maxPrice,
                                              tickSize,
                                              intensity,
                                              liquidity,
                                              liquidityScale);
    using R = HeatmapColumnStore::AppendResult;
    if (result != R::Written && result != R::AlreadyPresent) {
        // Conflict / IO error / bad input were already logged inside the store.
        // Fall through; we never block the live stream on persistence failures.
    }
}

bool HeatmapTwapStreamer::fetchHistory(const std::string& symbol,
                                       int64_t timeframeMs,
                                       int64_t endTimeMs,
                                       int count,
                                       int& outGridWidth,
                                       int& outGridHeight,
                                       std::vector<HistoryColumn>& out,
                                       int64_t startTimeMs) const {
    if (timeframeMs <= 0 || count <= 0) {
        return false;
    }

    out.clear();

    // Step 1: scan the in-RAM ring under m_historyMutex. Reverse-walk newest
    // -> oldest, collect into a reverse-order buffer.
    std::vector<HistoryColumn> ringReverse;
    int ringHeight = 0;
    int ringCapacity = 0;
    int64_t ringOldestBucketStart = std::numeric_limits<int64_t>::max();
    bool ringHadAny = false;
    {
        std::lock_guard<std::mutex> lock(m_historyMutex);
        auto it = m_symbols.find(symbol);
        if (it != m_symbols.end()) {
            const auto& state = it->second;
            auto ringIt = state.historyByTf.find(timeframeMs);
            if (ringIt != state.historyByTf.end()) {
                const auto& ring = ringIt->second;
                if (ring.count > 0 && ring.capacity > 0) {
                    ringHeight = state.height;
                    ringCapacity = ring.capacity;
                    ringHadAny = true;
                    const int latestIndex =
                        (ring.writeIndex - 1 + ring.capacity) % ring.capacity;
                    for (int i = 0; i < ring.count && static_cast<int>(ringReverse.size()) < count; ++i) {
                        const int idx = (latestIndex - i + ring.capacity) % ring.capacity;
                        const auto& col = ring.columns[static_cast<size_t>(idx)];
                        if (endTimeMs > 0 && col.bucketStartMs > endTimeMs) continue;
                        if (startTimeMs > 0 && col.bucketStartMs < startTimeMs) continue;
                        ringReverse.push_back(col);
                        if (col.bucketStartMs > 0 &&
                            col.bucketStartMs < ringOldestBucketStart) {
                            ringOldestBucketStart = col.bucketStartMs;
                        }
                    }
                }
            }
        }
    }

    // Step 2: disk fallthrough — only if (a) we have a store, (b) we still need
    // more columns, (c) we have a real lower bound to query before. The lower
    // bound is one full bucket before the ring's oldest matching column (so we
    // never duplicate a row that's already in `ringReverse`).
    const int needFromDisk = count - static_cast<int>(ringReverse.size());
    std::vector<HistoryColumn> diskChrono;
    if (m_columnStore && needFromDisk > 0) {
        int64_t diskEndMs = endTimeMs;
        if (ringHadAny && ringOldestBucketStart != std::numeric_limits<int64_t>::max()) {
            diskEndMs = ringOldestBucketStart - timeframeMs;
        }
        if (diskEndMs > 0) {
            std::vector<HeatmapColumnStore::LoadedColumn> loaded;
            if (m_columnStore->fetchRange(symbol, timeframeMs, diskEndMs,
                                          needFromDisk, loaded, startTimeMs)) {
                diskChrono.reserve(loaded.size());
                for (auto& src : loaded) {
                    HistoryColumn entry;
                    entry.bucketStartMs = src.bucketStartMs;
                    entry.bucketEndMs   = src.bucketEndMs;
                    entry.minPrice      = src.minPrice;
                    entry.maxPrice      = src.maxPrice;
                    entry.tickSize      = src.tickSize;
                    entry.intensity     = QByteArray(reinterpret_cast<const char*>(src.intensity.data()),
                                                     static_cast<int>(src.intensity.size()));
                    if (!src.liquidity.empty()) {
                        entry.liquidity = QByteArray(reinterpret_cast<const char*>(src.liquidity.data()),
                                                     static_cast<int>(src.liquidity.size()));
                    }
                    entry.liquidityScale = src.liquidityScale;
                    if (ringHeight == 0) ringHeight = src.gridHeight;
                    diskChrono.push_back(std::move(entry));
                }
            }
        }
    }

    if (ringReverse.empty() && diskChrono.empty()) {
        return false;
    }

    // Step 3: stitch. Disk results are chronological (oldest first); ring
    // results are newest-first. Final order: disk + reverse(ring).
    out.reserve(diskChrono.size() + ringReverse.size());
    for (auto& c : diskChrono) out.push_back(std::move(c));
    for (auto it = ringReverse.rbegin(); it != ringReverse.rend(); ++it) {
        out.push_back(std::move(*it));
    }

    const int availableWidth = (ringCapacity > 0) ? ringCapacity : m_defaultWidth;
    outGridWidth = std::min(availableWidth, count);
    outGridHeight = ringHeight;
    return true;
}

QByteArray HeatmapTwapStreamer::toIntensityColumnSigned(SymbolState& state,
                                                        const std::vector<double>& bidValues,
                                                        const std::vector<double>& askValues,
                                                        bool updateRunningMax) {
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
    if (cfg.useRunningMax && updateRunningMax) {
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

    const double denomBid = (cfg.useRunningMax ? std::max(state.runningMaxBid, maxBid) : maxBid);
    const double denomAsk = (cfg.useRunningMax ? std::max(state.runningMaxAsk, maxAsk) : maxAsk);
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
