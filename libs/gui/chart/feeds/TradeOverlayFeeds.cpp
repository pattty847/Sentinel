#include "chart/feeds/TradeOverlayFeeds.hpp"
#include "chart/feeds/TradeOverlaySequence.hpp"
#include "chart/layers/TradeLayers.hpp"
#include "render/DataProcessor.hpp"
#include "SentinelLogging.hpp"
#include <QThread>
#include <limits>

namespace chart {
TradeOverlayFeeds::TradeOverlayFeeds(TradeLayers& layers, QObject* context, std::function<void()> update)
    : layers_(layers), context_(context), update_(std::move(update)) {
    thread_ = std::make_unique<QThread>();
    thread_->setObjectName(QStringLiteral("DataProcessor"));
    processor_ = std::make_unique<DataProcessor>();
    processor_->moveToThread(thread_.get());
}
TradeOverlayFeeds::~TradeOverlayFeeds() { shutdown(); }
void TradeOverlayFeeds::shutdown() {
    teardownTradeOverlayFeeds(processor_, thread_, [](DataProcessor* processor) {
        QMetaObject::invokeMethod(processor, &DataProcessor::stopProcessing, Qt::BlockingQueuedConnection);
    }, [this](DataProcessor* processor) { disconnect(processor, nullptr, context_, nullptr); });
}
void TradeOverlayFeeds::startThread() { thread_->start(); }
void TradeOverlayFeeds::startProcessing() {
    QMetaObject::invokeMethod(processor_.get(), &DataProcessor::startProcessing, Qt::QueuedConnection);
}
void TradeOverlayFeeds::clearData() {
    clearTradeOverlayFeeds(processor_, layers_.footprint.renderer, layers_.vp.renderer, [](DataProcessor* processor) {
        QMetaObject::invokeMethod(processor, &DataProcessor::clearData, Qt::QueuedConnection);
    });
}

void TradeOverlayFeeds::connectDataProcessorSignals() {
    connect(processor_.get(), &DataProcessor::footprintColumnReady,
            context_,
            [this](int x, int gridWidth, int gridHeight, QByteArray columnQ16, TradeOverlayGrid grid) {
                const bool validGrid = gridWidth > 0 && gridHeight > 0 &&
                    gridHeight <= (std::numeric_limits<int>::max() / static_cast<int>(sizeof(uint16_t)));
                const int expectedBytes = validGrid ? gridHeight * static_cast<int>(sizeof(uint16_t)) : 0;
                if (!validGrid || columnQ16.size() != expectedBytes || x < 0 || x >= gridWidth) {
                    sLog_RenderN(1000, "footprint column dropped: x=" << x
                                 << " grid=" << gridWidth << "x" << gridHeight
                                 << " bytes=" << columnQ16.size() << " expectedBytes=" << expectedBytes);
                    return;
                }

                layers_.footprint.renderer.enqueue(
                    FootprintOverlayRenderer::PendingUpload{x, gridWidth, gridHeight, std::move(columnQ16), grid});
                sLog_Probe("footprint.queue",
                           "x=" << x << " grid=" << gridWidth << "x" << gridHeight);
                update_();
            },
            Qt::QueuedConnection);

    connect(processor_.get(), &DataProcessor::tpoColumnReady,
            context_,
            [this](int x, int gridWidth, int gridHeight, QByteArray letters,
                   int64_t sessionStartMs, int64_t sessionEndMs, int64_t timeframeMs, TradeOverlayGrid grid) {
                if (gridWidth <= 0 || gridHeight <= 0 || x < 0 || x >= gridWidth ||
                    letters.size() != gridHeight) {
                    sLog_RenderN(1000, "tpo column dropped: x=" << x
                                 << " grid=" << gridWidth << "x" << gridHeight
                                 << " bytes=" << letters.size());
                    return;
                }
                if (grid.startMs != sessionStartMs || grid.endMs != sessionEndMs) {
                    sLog_RenderN(1000, "tpo column dropped: session mismatch grid=["
                                 << grid.startMs << ".." << grid.endMs << "] session=["
                                 << sessionStartMs << ".." << sessionEndMs << "]");
                    return;
                }
                layers_.tpo.renderer.enqueue(TpoOverlayRenderer::PendingUpload{
                    x, gridWidth, gridHeight, std::move(letters), grid, timeframeMs});
                update_();
            },
            Qt::QueuedConnection);

    connect(processor_.get(), &DataProcessor::volumeProfileReady,
            context_,
            [this](std::vector<float> bins, VolumeProfileState::Snapshot snap) {
                if (bins.empty()) return;
                layers_.vp.renderer.enqueue(std::move(bins), std::move(snap));
                update_();
            },
            Qt::QueuedConnection);
}
} // namespace chart
