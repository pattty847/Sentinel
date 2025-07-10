#include "gpudataadapter.h"
#include "SentinelLogging.hpp"
#include <QDebug>

GPUDataAdapter::GPUDataAdapter(QObject* parent)
    : QObject(parent)
    , m_processTimer(new QTimer(this))
    , m_orderBookTimer(new QTimer(this))
    , m_tradePointTimer(new QTimer(this))
    , m_highFreqCandleTimer(new QTimer(this))
    , m_mediumFreqCandleTimer(new QTimer(this))
    , m_secondCandleTimer(new QTimer(this))
    , m_fractalZoomController(new FractalZoomController(this))
{
    // Initialize buffers
    initializeBuffers();
    
    // Connect processing timers
    connect(m_processTimer, &QTimer::timeout, this, &GPUDataAdapter::processIncomingData);
    connect(m_highFreqCandleTimer, &QTimer::timeout, this, &GPUDataAdapter::processHighFrequencyCandles);
    connect(m_mediumFreqCandleTimer, &QTimer::timeout, this, &GPUDataAdapter::processMediumFrequencyCandles);
    connect(m_secondCandleTimer, &QTimer::timeout, this, &GPUDataAdapter::processSecondCandles);
    connect(m_orderBookTimer, &QTimer::timeout, this, &GPUDataAdapter::processHeatmap);
    
    // Start timers at their initial intervals
    m_processTimer->start(50);  // 20 Hz base processing rate
    m_highFreqCandleTimer->start(100);   // 100ms candles
    m_mediumFreqCandleTimer->start(500); // 500ms candles
    m_secondCandleTimer->start(1000);    // 1s candles
    m_orderBookTimer->start(100);        // 100ms order book updates
    
    // Initialize fractal zoom controller with default timeframe
    m_fractalZoomController->onTimeFrameChanged(m_currentTimeFrame);
    
    // Connect fractal zoom signals
    connect(m_fractalZoomController, &FractalZoomController::orderBookUpdateIntervalChanged,
            this, &GPUDataAdapter::onOrderBookUpdateIntervalChanged);
    connect(m_fractalZoomController, &FractalZoomController::tradePointUpdateIntervalChanged,
            this, &GPUDataAdapter::onTradePointUpdateIntervalChanged);
    connect(m_fractalZoomController, &FractalZoomController::orderBookDepthChanged,
            this, &GPUDataAdapter::onOrderBookDepthChanged);
    connect(m_fractalZoomController, &FractalZoomController::timeFrameChanged,
            this, &GPUDataAdapter::onTimeFrameChanged);
}

GPUDataAdapter::~GPUDataAdapter() {
    stop();
}

void GPUDataAdapter::setCoinbaseClient(CoinbaseStreamClient* client) {
    m_coinbaseClient = client;
}

void GPUDataAdapter::setFractalZoomController(FractalZoomController* controller) {
    m_fractalZoomController = controller;
}

void GPUDataAdapter::setSymbol(const std::string& symbol) {
    m_currentSymbol = symbol;
}

void GPUDataAdapter::start() {
    m_processTimer->start();
    m_orderBookTimer->start();
    m_tradePointTimer->start();
    m_highFreqCandleTimer->start();
    m_mediumFreqCandleTimer->start();
    m_secondCandleTimer->start();
}

void GPUDataAdapter::stop() {
    m_processTimer->stop();
    m_orderBookTimer->stop();
    m_tradePointTimer->stop();
    m_highFreqCandleTimer->stop();
    m_mediumFreqCandleTimer->stop();
    m_secondCandleTimer->stop();
}

void GPUDataAdapter::setReserveSize(size_t size) {
    m_reserveSize = size;
    initializeBuffers();
}

void GPUDataAdapter::setFirehoseRate(int rate) {
    m_firehoseRate = rate;
}

void GPUDataAdapter::setOrderBookDepth(size_t depth) {
    m_currentOrderBookDepth = depth;
}

void GPUDataAdapter::setTimeFrame(CandleLOD::TimeFrame timeframe) {
    m_currentTimeFrame = timeframe;
}

bool GPUDataAdapter::pushTrade(const Trade& trade) {
    // Push trade to lock-free queue for processing
    bool success = m_tradeQueue.push(trade);
    if (success) {
        m_pointsPushed++;
    }
    return success;
}

void GPUDataAdapter::processIncomingData() {
    // sLog_GPU("--- TRADES: Starting processIncomingData ---");
    
    Trade trade;
    while (m_tradeQueue.pop(trade)) {
        // sLog_GPU(QString("--- TRADES: Popped trade. Cursor is at %1 ---").arg(m_tradeWriteCursor));
        
        // --- FIX: Set the current symbol ---
        if (m_currentSymbol.empty() && !trade.product_id.empty()) {
            m_currentSymbol = trade.product_id;
            // sLog_GPU("--- TRADES: SYMBOL DISCOVERED AND SET TO: " + QString::fromStdString(m_currentSymbol));
        }

        if (m_tradeWriteCursor >= m_reserveSize) {
            // sLog_GPU(QString("--- TRADES: Write cursor (%1) hit limit, wrapping to 0 ---").arg(m_tradeWriteCursor));
            m_tradeWriteCursor = 0;
        }
        
        // --- Logging to debug the potential crash ---
        // sLog_GPU(QString("--- TRADES: Preparing to write to m_tradeBuffer at index %1. Buffer size is %2.")
        //              .arg(m_tradeWriteCursor)
        //              .arg(m_tradeBuffer.size()));
        
        m_tradeBuffer[m_tradeWriteCursor++] = convertTradeToGPUPoint(trade);
        
        // sLog_GPU(QString("--- TRADES: Successfully wrote to buffer at index %1 ---").arg(m_tradeWriteCursor - 1));
        m_processedTrades++;
    }
    
    // Emit trade points if we have any
    if (m_tradeWriteCursor > 0) {
        // sLog_GPU("--- TRADES: Preparing to emit tradesReady signal ---");
        std::vector<GPUTypes::Point> points(m_tradeBuffer.begin(), m_tradeBuffer.begin() + m_tradeWriteCursor);
        emit tradesReady(points);
        // sLog_GPU("--- TRADES: tradesReady signal emitted successfully ---");
        m_tradeWriteCursor = 0;
    }
    // sLog_GPU("--- TRADES: Finished processIncomingData ---");
}

void GPUDataAdapter::processHeatmap() {
    sLog_GPU("--- Step A: processHeatmap() slot triggered ---");
    if (!m_coinbaseClient) {
        sLog_GPU("--- Step A FAILED: m_coinbaseClient is null ---");
        return;
    }

    if (m_currentSymbol.empty()) { // <-- ADD THIS GUARD CLAUSE
        sLog_GPU("--- Step A SKIPPED: Symbol is not yet known. ---");
        return;
    }

    const UniversalOrderBook* book = m_coinbaseClient->getUniversalOrderBook(m_currentSymbol);
    if (book && !book->isEmpty()) {
        sLog_GPU("--- Step A: UniversalOrderBook retrieved successfully ---");
        // Convert UniversalOrderBook to OrderBook for processing
        OrderBook orderBook;
        orderBook.product_id = m_currentSymbol;
        orderBook.bids = m_coinbaseClient->getBids(m_currentSymbol, m_currentOrderBookDepth);
        orderBook.asks = m_coinbaseClient->getAsks(m_currentSymbol, m_currentOrderBookDepth);
        orderBook.timestamp = std::chrono::system_clock::now();
        
        sLog_GPU("--- Step A: Converting to OrderBook - Bids:" << orderBook.bids.size() << " Asks:" << orderBook.asks.size() << " ---");
        processHeatmapAggregation(orderBook);
    } else {
        sLog_GPU("--- Step A FAILED: UniversalOrderBook is null or empty ---");
    }
    sLog_GPU("--- Step A PASSED: processHeatmap() finished ---");
}

void GPUDataAdapter::processHeatmapAggregation(const OrderBook& book) {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(book.timestamp.time_since_epoch()).count();

    // --- STEP 1: Add current book's data to ALL in-progress buckets ---
    for (size_t i = 0; i < CandleLOD::NUM_TIMEFRAMES; ++i) {
        for (const auto& level : book.bids) {
            m_bidHeatmapAggregators[i][level.price] += level.size;
        }
        for (const auto& level : book.asks) {
            m_askHeatmapAggregators[i][level.price] += level.size;
        }
    }

    // --- STEP 2: Check if any buckets are now complete and ready to be emitted ---
    for (size_t i = 0; i < CandleLOD::NUM_TIMEFRAMES; ++i) {
        auto timeframe = static_cast<CandleLOD::TimeFrame>(i);
        int64_t bucketDuration = CandleLOD::getTimeFrameDuration(timeframe);
        
        if (m_heatmapBucketStartTimes[i] == 0) {
            m_heatmapBucketStartTimes[i] = (now / bucketDuration) * bucketDuration;
        }

        if (now >= m_heatmapBucketStartTimes[i] + bucketDuration) {
            std::vector<QuadInstance> finalBids, finalAsks;
            finalBids.reserve(m_bidHeatmapAggregators[i].size());
            finalAsks.reserve(m_askHeatmapAggregators[i].size());

            for (const auto& [price, total_size] : m_bidHeatmapAggregators[i]) {
                QuadInstance quad;
                quad.rawPrice = price;
                quad.size = total_size;
                quad.rawTimestamp = m_heatmapBucketStartTimes[i];
                finalBids.push_back(quad);
            }
            for (const auto& [price, total_size] : m_askHeatmapAggregators[i]) {
                QuadInstance quad;
                quad.rawPrice = price;
                quad.size = total_size;
                quad.rawTimestamp = m_heatmapBucketStartTimes[i];
                finalAsks.push_back(quad);
            }

            // Always emit, even if both are empty, for gap debugging
            bool willEmit = true;
            sLog_GPU(QString("HEATMAP BUCKET: tf=%1 t=%2 dur=%3 bids_in=%4 asks_in=%5 bids_out=%6 asks_out=%7 emit=%8")
                .arg(static_cast<int>(timeframe))
                .arg(m_heatmapBucketStartTimes[i])
                .arg(bucketDuration)
                .arg(book.bids.size())
                .arg(book.asks.size())
                .arg(finalBids.size())
                .arg(finalAsks.size())
                .arg(willEmit ? "Y" : "N"));
            emit aggregatedHeatmapReady(timeframe, finalBids, finalAsks);

            // Start a new bucket for the next cycle
            m_bidHeatmapAggregators[i].clear();
            m_askHeatmapAggregators[i].clear();
            m_heatmapBucketStartTimes[i] = (now / bucketDuration) * bucketDuration;
        }
    }
}

// In GPUDataAdapter.cpp

void GPUDataAdapter::initializeBuffers() {
    // Runtime-configurable buffer size (handle Intel UHD VRAM limits)
    QSettings settings;
    // Set a reasonable default and minimum size.
    size_t desiredSize = settings.value("chart/reserveSize", 100000).toULongLong();
    if (desiredSize < 10000) {
        desiredSize = 10000; // Enforce a sane minimum to prevent crashes
    }
    m_reserveSize = desiredSize;

    sLog_Init(QString("🔧 GPUDataAdapter: Resizing buffers to %1 elements.").arg(m_reserveSize));

    try {
        // Use resize() to actually create the elements and set the size.
        m_tradeBuffer.resize(m_reserveSize);
        m_heatmapBuffer.resize(m_reserveSize);
        m_candleBuffer.resize(m_reserveSize);
    } catch (const std::bad_alloc& e) {
        sLog_Error(QString("FATAL: Failed to allocate memory for buffers! %1").arg(e.what()));
        // In a real app, you might want to gracefully exit here.
        return;
    }
    
    // Final check to be 100% certain.
    sLog_Init(QString("💾 GPUDataAdapter: Buffer allocation complete. m_tradeBuffer size is now: %1").arg(m_tradeBuffer.size()));
}

GPUTypes::Point GPUDataAdapter::convertTradeToGPUPoint(const Trade& trade) {
    GPUTypes::Point point;
    point.x = 0.0f; // Will be calculated in GPU widget
    point.y = static_cast<float>(trade.price);
    
    // Color based on trade side
    if (trade.side == AggressorSide::Buy) {
        point.r = 0.0f; point.g = 1.0f; point.b = 0.0f; // Green for buys
    } else {
        point.r = 1.0f; point.g = 0.0f; point.b = 0.0f; // Red for sells
    }
    point.a = 0.8f;
    
    return point;
}

void GPUDataAdapter::updateCoordinateCache(double price) {
    if (!m_coordCache.initialized) {
        m_coordCache.minPrice = price;
        m_coordCache.maxPrice = price;
        m_coordCache.initialized = true;
        return;
    }
    
    m_coordCache.minPrice = std::min(m_coordCache.minPrice, price);
    m_coordCache.maxPrice = std::max(m_coordCache.maxPrice, price);
}

void GPUDataAdapter::onOrderBookUpdateIntervalChanged(int intervalMs) {
    m_orderBookTimer->setInterval(intervalMs);
}

void GPUDataAdapter::onTradePointUpdateIntervalChanged(int intervalMs) {
    m_tradePointTimer->setInterval(intervalMs);
}

void GPUDataAdapter::onOrderBookDepthChanged(size_t maxLevels) {
    m_currentOrderBookDepth = maxLevels;
}

void GPUDataAdapter::onTimeFrameChanged(CandleLOD::TimeFrame newTimeFrame) {
    m_currentTimeFrame = newTimeFrame;
}

void GPUDataAdapter::processHighFrequencyCandles() {
    // Process 100ms candles
    processCandleTimeFrame(CandleLOD::TF_100ms);
}

void GPUDataAdapter::processMediumFrequencyCandles() {
    // Process 500ms candles
    processCandleTimeFrame(CandleLOD::TF_500ms);
}

void GPUDataAdapter::processSecondCandles() {
    // Process 1s candles
    processCandleTimeFrame(CandleLOD::TF_1sec);
}

void GPUDataAdapter::processCandleTimeFrame(CandleLOD::TimeFrame timeframe) {
    // Process candles for the specified timeframe
    // This is a placeholder implementation - actual candle processing logic
    // would be implemented based on the specific requirements
    
    // For now, we'll just emit a signal to indicate the timeframe was processed
    // In a real implementation, this would process the CandleLOD data and emit candlesReady
    
    // Example placeholder:
    // std::vector<CandleUpdate> candles = m_candleLOD.getCandlesForTimeFrame(timeframe);
    // if (!candles.empty()) {
    //     emit candlesReady(candles);
    // }
}