#include "HeatmapDataService.hpp"
#include "SentinelLogging.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <stdexcept>
#include <exception>

namespace heatmap {
HeatmapDataService::HeatmapDataService(TransportFactory factory, HeatmapBudgets budgets, StartTransport start,
                                       std::function<void()> beforeStop) : beforeStop_(std::move(beforeStop)) {
    if (!factory || !budgets.valid()) throw std::invalid_argument("Invalid heatmap service factory or budgets");
    thread_ = std::make_unique<QThread>();
    thread_->setObjectName(QStringLiteral("heatmap-data"));
    context_ = new QObject;
    context_->moveToThread(thread_.get());
    thread_->start();
    std::exception_ptr failure;
    onData([&] {
        // Exceptions must not unwind through Qt's worker event dispatcher.
        try {
            transport_ = factory(context_);
            if (!transport_) throw std::runtime_error("Heatmap transport factory returned null");
            Q_ASSERT(transport_ && transport_->thread() == QThread::currentThread());
            fetcher_ = new ChunkFetcher(store_, *transport_);
            cache_ = new SpanSourceCache;
            HeatmapSourceController::applyBudgets(budgets, store_, *cache_);
            QObject::connect(transport_, &ChunkTransport::connected, context_, [this] { connected_ = true; });
            QObject::connect(transport_, &ChunkTransport::disconnected, context_, [this] {
                connected_ = false;
                std::scoped_lock lock(mutex_);
                availability_.clear();
            });
            QObject::connect(fetcher_, &ChunkFetcher::availabilityChanged, context_, [this](const ChunkAvailability &value) {
                std::scoped_lock lock(mutex_);
                availability_[value.symbol] = value;
            });
            QObject::connect(fetcher_, &ChunkFetcher::storeCleared, context_, [this] {
                std::scoped_lock lock(mutex_);
                availability_.clear();
            });
            statsTimer_ = new QTimer(context_);
            statsTimer_->setInterval(250);
            QObject::connect(statsTimer_, &QTimer::timeout, context_, [this] { refreshStats(); });
            statsTimer_->start();
            // The fetcher and service are attached before any connection or local
            // availability emission. External clients connect after construction.
            if (start) start(*transport_);
        } catch (...) {
            failure = std::current_exception();
            destroyData();
        }
    });
    if (failure) {
        stopThread();
        std::rethrow_exception(failure);
    }
    sLog_App("Heatmap data service started decodedBytes=" << budgets.decodedChunks
             << " spanBytes=" << budgets.spanSources << " ceilingBytes=" << budgets.cpuCeiling);
}
HeatmapDataService::~HeatmapDataService() {
    // Holders forget their controllers first (each hook may remove itself).
    while (!shutdownHooks_.empty()) {
        auto hook = std::move(shutdownHooks_.begin()->second);
        shutdownHooks_.erase(shutdownHooks_.begin());
        if (hook) hook();
    }
    onData([this] {
        if (beforeStop_) beforeStop_();
        destroyData();
    });
    stopThread();
}
void HeatmapDataService::destroyData() {
    delete statsTimer_;
    for (auto *controller : controllers_) delete controller;
    controllers_.clear();
    delete cache_;
    delete fetcher_;
    delete transport_;
    // Factory-owned clients/timers also die on their thread, including when
    // the factory/start callback threw. Drop their queued calls (FM-122).
    qDeleteAll(context_->children());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}
void HeatmapDataService::stopThread() {
    thread_->quit();
    thread_->wait();
    delete context_;
}
void HeatmapDataService::onData(std::function<void()> work) const {
    if (QThread::currentThread() == thread_.get()) work();
    else QMetaObject::invokeMethod(context_, std::move(work), Qt::BlockingQueuedConnection);
}
int HeatmapDataService::addShutdownHook(std::function<void()> hook) {
    const int id = ++nextHook_;
    shutdownHooks_[id] = std::move(hook);
    return id;
}
void HeatmapDataService::removeShutdownHook(int id) { shutdownHooks_.erase(id); }
HeatmapSourceController *HeatmapDataService::createController(size_t gpuBytes) {
    HeatmapSourceController *out = nullptr;
    onData([&] {
        HeatmapSourceController::Options options;
        options.gpuBytes = gpuBytes;
        out = new HeatmapSourceController(store_, *fetcher_, *cache_, options);
        controllers_.insert(out);
    });
    return out;
}
void HeatmapDataService::destroyController(HeatmapSourceController *controller) {
    onData([&] { if (controllers_.erase(controller)) delete controller; });
}
size_t HeatmapDataService::controllerCount() const {
    size_t count = 0;
    onData([&] { count = controllers_.size(); });
    return count;
}
std::optional<ChunkAvailability> HeatmapDataService::availability(const std::string &symbol) const {
    std::scoped_lock lock(mutex_);
    const auto it = availability_.find(symbol);
    return it == availability_.end() ? std::nullopt : std::optional(it->second);
}
HeatmapDataService::Stats HeatmapDataService::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}
void HeatmapDataService::refreshStats() {
    onData([this] {
        Stats value{store_.stats(), fetcher_->stats(), cache_->stats(), cache_->committedCpuBytes()};
        std::scoped_lock lock(mutex_);
        stats_ = value;
    });
}
bool HeatmapDataService::setBudgets(const HeatmapBudgets &budgets) {
    bool applied = false;
    onData([&] { applied = HeatmapSourceController::applyBudgets(budgets, store_, *cache_); });
    return applied;
}
} // namespace heatmap
