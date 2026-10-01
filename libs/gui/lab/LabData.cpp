#include "LabData.hpp"
#include "LabSources.hpp"
#include "SentinelLogging.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "protocol/SentinelStreamClientTransport.hpp"
#include "servermodel/RecordingChunks.hpp"
#include <QCoreApplication>
#include <QTimer>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace lab {
uint64_t processFootprintBytes() {
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.phys_footprint;
#endif
    return 0;
}
namespace {
std::mutex instanceMutex;
std::unique_ptr<LabData> current;
std::string configuredRoot;
int64_t configuredPin = 0;
std::optional<LabData::Server> configuredServer;
bool postRoutine = false;

void shutdownInstance() {
    std::unique_ptr<LabData> dying;
    {
        std::scoped_lock lock(instanceMutex);
        dying = std::move(current);
    }
    dying.reset(); // joins the data thread
}

// Blocking call on the data thread (never from it).
template <class F> void onData(QObject *context, F &&f) {
    if (QThread::currentThread() == context->thread()) { f(); return; }
    QMetaObject::invokeMethod(context, std::forward<F>(f), Qt::BlockingQueuedConnection);
}
} // namespace

LabData &LabData::instance() {
    std::scoped_lock lock(instanceMutex);
    if (!current) {
        current.reset(new LabData(configuredRoot.empty() ? recordingRoot() : configuredRoot, configuredPin,
                                  configuredServer));
        current->start();
        if (!postRoutine) {
            qAddPostRoutine(shutdownInstance); // before the QCoreApplication goes
            postRoutine = true;
        }
    }
    return *current;
}

void LabData::configure(const std::string &root, int64_t pinnedEndMs) {
    std::unique_ptr<LabData> old;
    {
        std::scoped_lock lock(instanceMutex);
        if (current && current->controllers_ > 0)
            sLog_Warning("Lab data path reconfigured while " << current->controllers_ << " charts still exist");
        old = std::move(current);
        configuredRoot = root;
        configuredPin = std::max<int64_t>(pinnedEndMs, 0);
    }
    old.reset(); // the next instance() starts afresh
}
void LabData::configureServer(const Server &server) {
    std::unique_ptr<LabData> old;
    {
        std::scoped_lock lock(instanceMutex);
        old = std::move(current);
        configuredServer = server;
    }
    old.reset();
}
std::optional<LabData::Server> LabData::server() {
    std::scoped_lock lock(instanceMutex);
    return configuredServer;
}
QString LabData::connectionText() const {
    switch (connection_.load()) {
    case Connection::Local: return QStringLiteral("local recording");
    case Connection::Connecting: return QStringLiteral("connecting");
    case Connection::Connected: return QStringLiteral("connected");
    case Connection::Disconnected: return QStringLiteral("disconnected");
    }
    return {};
}
std::string LabData::root() {
    std::scoped_lock lock(instanceMutex);
    return configuredRoot.empty() ? recordingRoot() : configuredRoot;
}
int64_t LabData::pinnedEndMs() {
    std::scoped_lock lock(instanceMutex);
    return configuredPin;
}

LabData::LabData(std::string root, int64_t pinnedEndMs, std::optional<Server> server)
    : root_(std::move(root)), pinnedEndMs_(pinnedEndMs), server_(std::move(server)) {}
LabData::~LabData() { shutdown(); }

void LabData::start() {
    thread_ = std::make_unique<QThread>();
    thread_->setObjectName(QStringLiteral("heatmap-data"));
    context_ = new QObject;
    context_->moveToThread(thread_.get());
    thread_->start();
    onData(context_, [this] {
        if (server_) {
            startServerTransport();
        } else {
            heatmap::LocalChunkTransport::TestHooks hooks;
            hooks.pinnedEndMs = pinnedEndMs_;
            transport_ = new heatmap::LocalChunkTransport(root_, hooks);
        }
        fetcher_ = new heatmap::ChunkFetcher(store_, *transport_);
        cache_ = new heatmap::SpanSourceCache;
        heatmap::HeatmapSourceController::applyBudgets(heatmap::HeatmapBudgets{}, store_, *cache_);
        QObject::connect(fetcher_, &heatmap::ChunkFetcher::availabilityChanged, context_,
                         [this](const heatmap::ChunkAvailability &value) {
                             std::scoped_lock lock(mutex_);
                             if (value.symbol == kSymbol) availability_ = value;
                         });
        statsTimer_ = new QTimer(context_);
        statsTimer_->setInterval(250);
        QObject::connect(statsTimer_, &QTimer::timeout, context_, [this] { refreshStats(); });
        statsTimer_->start();
        if (auto *local = qobject_cast<heatmap::LocalChunkTransport *>(transport_)) local->start({kSymbol});
        else if (auto *client = static_cast<SentinelStreamClient *>(client_)) {
            connection_ = Connection::Connecting;
            client->connectToServer(); // the fetcher is attached: it sees connected + fresh availability
            reconnectTimer_->start();
        }
    });
    if (server_)
        sLog_App("Lab heatmap data path started server=" << server_->host << ":" << server_->port);
    else
        sLog_App("Lab heatmap data path started root=" << root_ << " pinnedEndMs=" << pinnedEndMs_);
}

// Data thread. The stream client has no reconnect of its own: retry every 2 s
// while not connected. Availability is pushed on "subscribe" (and on change),
// so subscribe on every connect; the fetcher then (re)subscribes the live edge.
void LabData::startServerTransport() {
    auto *client = new SentinelStreamClient(server_->host, server_->port, server_->caFile);
    client_ = client;
    auto *transport = new protocol::SentinelStreamClientTransport(*client);
    transport_ = transport;
    QObject::connect(client, &SentinelStreamClient::connected, context_, [this, client] {
        connection_ = Connection::Connected;
        client->subscribe(kSymbol);
        sLog_App("Lab connected to sentinel-server " << server_->host << ":" << server_->port);
    }, Qt::QueuedConnection);
    QObject::connect(client, &SentinelStreamClient::disconnected, context_, [this] {
        if (connection_.exchange(Connection::Disconnected) != Connection::Disconnected)
            sLog_Warning("Lab disconnected from sentinel-server; the live edge is frozen until it reconnects");
    }, Qt::QueuedConnection);
    QObject::connect(client, &SentinelStreamClient::errorOccurred, context_, [this](const QString &error) {
        if (error == QStringLiteral("heatmap chunk wire version mismatch")) return; // the transport reports it
        if (connection_.load() != Connection::Connected) connection_ = Connection::Disconnected;
        sLog_DataN(5000, "Lab stream client error: " << error);
    }, Qt::QueuedConnection);
    reconnectTimer_ = new QTimer(context_);
    reconnectTimer_->setInterval(2000);
    QObject::connect(reconnectTimer_, &QTimer::timeout, context_, [this, client] {
        if (connection_.load() != Connection::Disconnected) return;
        connection_ = Connection::Connecting;
        client->disconnectFromServer();
        client->connectToServer();
    });
}

void LabData::shutdown() {
    if (!thread_) return;
    onData(context_, [this] {
        delete statsTimer_; // timers stop on their own thread
        statsTimer_ = nullptr;
        delete reconnectTimer_;
        reconnectTimer_ = nullptr;
        delete cache_;
        delete fetcher_; // before the transport it listens to
        delete transport_;
        delete client_; // after its adapter; disconnects and joins its thread
        cache_ = nullptr;
        fetcher_ = nullptr;
        transport_ = nullptr;
        client_ = nullptr;
    });
    thread_->quit();
    thread_->wait();
    delete context_; // its thread has finished
    context_ = nullptr;
    thread_.reset();
}

heatmap::HeatmapSourceController *LabData::createController(size_t gpuBytes) {
    heatmap::HeatmapSourceController *out = nullptr;
    onData(context_, [&] {
        heatmap::HeatmapSourceController::Options options;
        options.gpuBytes = gpuBytes;
        out = new heatmap::HeatmapSourceController(store_, *fetcher_, *cache_, options);
    });
    ++controllers_;
    return out;
}
void LabData::destroyController(heatmap::HeatmapSourceController *controller) {
    if (!controller) return;
    onData(context_, [controller] { delete controller; });
    --controllers_;
}

std::optional<heatmap::ChunkAvailability> LabData::availability() const {
    std::scoped_lock lock(mutex_);
    return availability_;
}

void LabData::clearCaches() { configure(root_, pinnedEndMs_); }

std::vector<std::pair<heatmap::ChunkKey, uint64_t>> LabData::reviseNewestChunks() {
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> out;
    const auto a = availability();
    if (!a) return out;
    onData(context_, [this, a, &out] {
        std::unique_ptr<recording::Hmc2Reader> reader;
        for (const auto &source : a->sources) {
            recording::BookRecorder::Watermarks marks;
            for (const auto &level : source.levels) {
                if (level.levelMs == heatmap::kMinuteMs) marks.minuteThroughMs = level.committedThroughMs;
                if (level.levelMs == heatmap::kHourMs) marks.hourThroughMs = level.committedThroughMs;
            }
            if (marks.minuteThroughMs <= 0) continue;
            const heatmap::ChunkKey key{kSymbol, source.id, heatmap::kMinuteMs,
                                        recording::floorDiv(marks.minuteThroughMs - 1, heatmap::kHourMs) * heatmap::kHourMs};
            const auto chunk = store_.cached(key);
            if (!chunk) continue;
            try {
                // Re-read and decode it (as a revised body would arrive), then
                // store it as a new version: every build that used it is stale.
                if (!reader) reader = std::make_unique<recording::Hmc2Reader>(root_);
                auto columns = std::make_shared<const heatmap::SparseColumns>(recording::buildChunk(*reader, key, marks));
                const auto stored = store_.put(key, std::move(columns),
                                               {chunk->sealed, chunk->committedThroughMs, chunk->revision + 1},
                                               chunk->contentHash + 1);
                if (stored) out.emplace_back(key, stored->generation);
                sLog_Data("Lab revised chunk source=" << key.source << " start=" << key.startMs);
            } catch (const std::exception &e) {
                sLog_Warning("Lab chunk revision failed source=" << key.source << " start=" << key.startMs
                             << " error=" << e.what());
            }
        }
    });
    return out;
}

LabData::Stats LabData::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}
void LabData::refreshStats() {
    onData(context_, [this] {
        Stats s;
        s.store = store_.stats();
        if (fetcher_) s.fetcher = fetcher_->stats();
        if (cache_) {
            s.cache = cache_->stats();
            s.committedCpuBytes = cache_->committedCpuBytes();
        }
        std::scoped_lock lock(mutex_);
        stats_ = s;
    });
}

double LabData::recentMidPrice() const {
    const auto a = availability();
    if (!a) return 0;
    try {
        recording::Hmc2Reader reader(root_); // read-only; this thread only
        for (const auto &source : a->sources) {
            recording::BookRecorder::Watermarks marks;
            for (const auto &level : source.levels) {
                if (level.levelMs == heatmap::kMinuteMs) marks.minuteThroughMs = level.committedThroughMs;
                if (level.levelMs == heatmap::kHourMs) marks.hourThroughMs = level.committedThroughMs;
            }
            if (marks.minuteThroughMs <= 0) continue;
            const int64_t newest = recording::floorDiv(marks.minuteThroughMs - 1, heatmap::kHourMs) * heatmap::kHourMs;
            for (int64_t start = newest; start >= newest - 3 * heatmap::kHourMs; start -= heatmap::kHourMs) {
                const auto columns = recording::buildChunk(reader, {kSymbol, source.id, heatmap::kMinuteMs, start}, marks);
                if (const double price = medianRecentPrice(columns); price > 0) return price;
            }
        }
    } catch (const std::exception &e) {
        sLog_Warning("Lab recent price failed: " << e.what());
    }
    return 0;
}
} // namespace lab
