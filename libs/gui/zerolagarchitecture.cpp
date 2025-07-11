#include "zerolagarchitecture.h"
#include <QDebug>
#include <QThread>

ZeroLagArchitecture::ZeroLagArchitecture(QObject* parent)
    : QObject(parent)
    , m_viewport(std::make_unique<UnifiedViewport>(this))
    , m_lod(std::make_unique<DynamicLOD>(this))
    , m_testData(std::make_unique<InstantTestData>(this))
{
    setupConnections();
    optimizePerformance();
}

void ZeroLagArchitecture::initialize(QQuickItem* rootItem) {
    qDebug() << "🚀 INITIALIZING ZERO-LAG ARCHITECTURE";
    
    // Create heatmap component
    m_heatmap = std::make_unique<ZeroLagHeatmap>();
    m_heatmap->setParentItem(rootItem);
    m_heatmap->setZ(10);
    m_heatmap->setWidth(rootItem->width());
    m_heatmap->setHeight(rootItem->height());
    
    // 🔥 FIX: Set REAL timestamps from test data
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    m_viewport->setViewport(
        now - 60000,  // 1 minute ago - REAL TIME
        now,          // now - REAL TIME  
        113650.0,     // BTC price range
        113750.0
    );
    
    // 🔥 ALSO: Set the pixel size
    m_viewport->setPixelSize(rootItem->width(), rootItem->height());
    
    qDebug() << "✅ ZERO-LAG ARCHITECTURE INITIALIZED with time:" << (now - 60000) << "to" << now;
}

void ZeroLagArchitecture::startTestMode() {
    qDebug() << "🚀 STARTING ZERO-LAG TEST MODE";
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Generate complete historical dataset instantly
    m_testData->generateFullHistoricalSet(now, 86400000); // 24 hours
    
    // Start real-time simulation for ongoing updates
    m_testData->startRealTimeSimulation();
    
    qDebug() << "✅ TEST MODE ACTIVE - Ready for immediate zoom testing!";
    qDebug() << "🎯 Scroll to zoom, drag to pan - ZERO LAG GUARANTEED!";
}

void ZeroLagArchitecture::handleMousePress(const QPointF& position) {
    m_isDragging = true;
    m_lastMousePos = position;
}

void ZeroLagArchitecture::handleMouseMove(const QPointF& delta) {
    if (!m_isDragging) return;
    
    // IMMEDIATE viewport update - no throttling!
    m_viewport->pan(delta.x(), delta.y());
    
    // Performance monitoring
    m_frameCount.fetch_add(1);
}

void ZeroLagArchitecture::handleWheel(double delta, const QPointF& center) {
    // Immediate zoom with exponential scaling for smooth feel
    double zoomFactor = 1.0 + (delta * 0.001); // Smooth zoom sensitivity
    m_viewport->zoom(zoomFactor, center);
    
    qDebug() << "🔍 INSTANT ZOOM:" << zoomFactor << "at" << center;
}

void ZeroLagArchitecture::onViewportChanged(const UnifiedViewport::ViewState& state) {
    // Check if LOD should change
    auto newTF = m_lod->selectOptimalTimeFrame(state.msPerPixel());
    auto gridInfo = m_lod->calculateGrid(state.timeStart_ms, state.timeEnd_ms, state.pixelWidth);
    
    // Instant LOD switching
    if (m_heatmap) {
        m_heatmap->switchToTimeFrame(newTF);
    }
    
    qDebug() << "📊 VIEWPORT UPDATE:" 
             << "TimeFrame:" << DynamicLOD::getTimeFrameName(newTF)
             << "BucketSize:" << gridInfo.bucketDuration_ms << "ms"
             << "PixelsPerBucket:" << gridInfo.pixelsPerBucket;
}

void ZeroLagArchitecture::onOrderBookUpdate(const InstantTestData::OrderBookSnapshot& snapshot) {
    // Feed data to heatmap with all timeframes
    if (m_heatmap) {
        m_heatmap->addOrderBookUpdate(snapshot.timestamp_ms, snapshot.bids, snapshot.asks);
    }
}

void ZeroLagArchitecture::onTradeExecuted(int64_t timestamp_ms, double price, double size, bool isBuy) {
    Q_UNUSED(timestamp_ms);
    Q_UNUSED(price);
    Q_UNUSED(size);
    Q_UNUSED(isBuy);
    // TODO: Handle trade execution if needed
}

void ZeroLagArchitecture::onTimeFrameChanged(DynamicLOD::TimeFrame newTF, const DynamicLOD::GridInfo& gridInfo) {
    Q_UNUSED(newTF);
    Q_UNUSED(gridInfo);
    // TODO: Handle timeframe changes if needed
}

void ZeroLagArchitecture::setupConnections() {
    // Viewport coordination
    connect(m_viewport.get(), &UnifiedViewport::viewportChanged,
            this, &ZeroLagArchitecture::onViewportChanged);
    
    // Test data pipeline
    connect(m_testData.get(), &InstantTestData::orderBookUpdate,
            this, &ZeroLagArchitecture::onOrderBookUpdate);
    
    connect(m_testData.get(), &InstantTestData::tradeExecuted,
            this, &ZeroLagArchitecture::onTradeExecuted);
    
    qDebug() << "🔗 ZERO-LAG CONNECTIONS ESTABLISHED";
}

void ZeroLagArchitecture::optimizePerformance() {
    // Set thread priorities for real-time performance
    QThread::currentThread()->setPriority(QThread::TimeCriticalPriority);
    
    qDebug() << "⚡ PERFORMANCE OPTIMIZATIONS ACTIVE";
}