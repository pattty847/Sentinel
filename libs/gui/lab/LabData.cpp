#include "LabData.hpp"
#include "heatmap/LocalChunkTransport.hpp"
#include "LabSources.hpp"
#include "SentinelLogging.hpp"
#include "protocol/SentinelStreamClient.hpp"
#include "protocol/SentinelStreamClientTransport.hpp"
#include "servermodel/RecordingChunks.hpp"
#include <QCoreApplication>
#include <QTimer>

namespace lab {
uint64_t processFootprintBytes() { return heatmap::processFootprintBytes(); }
namespace {
std::mutex instanceMutex;
std::unique_ptr<LabData> current;
std::string configuredRoot;
int64_t configuredPin = 0;
heatmap::HeatmapBudgets configuredBudgets;
std::optional<LabData::Server> configuredServer;
std::atomic<bool> queueConnectedOnShutdown{false};
std::atomic<int> lateConnectionCallbacks{0};
bool postRoutine = false;

void shutdownInstance() {
    std::unique_ptr<LabData> dying;
    {
        std::scoped_lock lock(instanceMutex);
        dying = std::move(current);
    }
    dying.reset(); // joins the data thread
}

} // namespace

LabData &LabData::instance() {
    std::scoped_lock lock(instanceMutex);
    if (!current) {
        current.reset(new LabData(configuredRoot.empty() ? recordingRoot() : configuredRoot, configuredPin,
                                  configuredServer, configuredBudgets));
        current->start();
        if (!postRoutine) {
            qAddPostRoutine(shutdownInstance); // before the QCoreApplication goes
            postRoutine = true;
        }
    }
    return *current;
}

void LabData::configure(const std::string &root, int64_t pinnedEndMs, heatmap::HeatmapBudgets budgets) {
    std::unique_ptr<LabData> old;
    {
        std::scoped_lock lock(instanceMutex);
        if (current && current->service_->controllerCount() > 0)
            sLog_Warning("Lab data path reconfigured while " << current->service_->controllerCount() << " charts still exist");
        old = std::move(current);
        configuredRoot = root;
        configuredPin = std::max<int64_t>(pinnedEndMs, 0);
        configuredBudgets = budgets;
    }
    old.reset(); // the next instance() starts afresh
}
void LabData::queueConnectedOnShutdownForTest(bool queue) { queueConnectedOnShutdown = queue; }
int LabData::lateConnectionCallbacksForTest() { return lateConnectionCallbacks.load(); }

void LabData::configureServer(const std::optional<Server> &server) {
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

LabData::LabData(std::string root, int64_t pinnedEndMs, std::optional<Server> server, heatmap::HeatmapBudgets budgets)
    : budgets_(budgets), root_(std::move(root)), pinnedEndMs_(pinnedEndMs), server_(std::move(server)) {}
LabData::~LabData() { shutdown(); }

void LabData::start() {
    service_ = std::make_unique<heatmap::HeatmapDataService>([this](QObject *context) -> heatmap::ChunkTransport * {
        if (server_) return createServerTransport(context);
        heatmap::LocalChunkTransport::TestHooks hooks;
        hooks.pinnedEndMs = pinnedEndMs_;
        return new heatmap::LocalChunkTransport(root_, hooks);
    }, budgets_, [this](heatmap::ChunkTransport &transport) {
        if (auto *local = qobject_cast<heatmap::LocalChunkTransport *>(&transport)) local->start({kSymbol});
        else {
            connection_ = Connection::Connecting;
            static_cast<SentinelStreamClient *>(client_)->connectToServer();
            reconnectTimer_->start();
        }
    }, [this] {
        tearingDown_ = true;
        if (queueConnectedOnShutdown && client_) emit static_cast<SentinelStreamClient *>(client_)->connected();
    });
    if (server_)
        sLog_App("Lab heatmap data path started server=" << server_->host << ":" << server_->port);
    else
        sLog_App("Lab heatmap data path started root=" << root_ << " pinnedEndMs=" << pinnedEndMs_);
}

// Data thread. The stream client has no reconnect of its own: retry every 2 s
// while not connected. Availability is pushed on "subscribe" (and on change),
// so subscribe on every connect; the fetcher then (re)subscribes the live edge.
// Every callback uses the client itself as its context: a delivery queued while
// the data path shuts down is dropped with the client, never run on a dead one.
heatmap::ChunkTransport *LabData::createServerTransport(QObject *context) {
    auto *client = new SentinelStreamClient(server_->host, server_->port, server_->caFile);
    client->setParent(context);
    client_ = client;
    auto *transport = new protocol::SentinelStreamClientTransport(*client);
    QObject::connect(client, &SentinelStreamClient::connected, client, [this, client] {
        if (tearingDown_) ++lateConnectionCallbacks;
        connection_ = Connection::Connected;
        client->subscribe(kSymbol);
        sLog_App("Lab connected to sentinel-server " << server_->host << ":" << server_->port);
    }, Qt::QueuedConnection);
    QObject::connect(client, &SentinelStreamClient::disconnected, client, [this] {
        if (tearingDown_) ++lateConnectionCallbacks;
        if (connection_.exchange(Connection::Disconnected) != Connection::Disconnected)
            sLog_Warning("Lab disconnected from sentinel-server; the live edge is frozen until it reconnects");
    }, Qt::QueuedConnection);
    QObject::connect(client, &SentinelStreamClient::errorOccurred, client, [this](const QString &error) {
        if (tearingDown_) ++lateConnectionCallbacks;
        if (error == QStringLiteral("heatmap chunk wire version mismatch")) return; // the transport reports it
        if (connection_.load() != Connection::Connected) connection_ = Connection::Disconnected;
        sLog_DataN(5000, "Lab stream client error: " << error);
    }, Qt::QueuedConnection);
    reconnectTimer_ = new QTimer(client);
    reconnectTimer_->setInterval(2000);
    QObject::connect(reconnectTimer_, &QTimer::timeout, client, [this, client] {
        if (connection_.load() != Connection::Disconnected) return;
        connection_ = Connection::Connecting;
        client->disconnectFromServer();
        client->connectToServer();
    });
    return transport;
}

void LabData::shutdown() {
    if (!service_) return;
    service_.reset();
}

heatmap::HeatmapSourceController *LabData::createController(size_t gpuBytes) {
    return service_->createController(gpuBytes);
}
void LabData::destroyController(heatmap::HeatmapSourceController *controller) {
    service_->destroyController(controller);
}
std::optional<heatmap::ChunkAvailability> LabData::availability() const {
    return service_->availability(kSymbol);
}
void LabData::clearCaches() { configure(root_, pinnedEndMs_, budgets_); }

std::vector<std::pair<heatmap::ChunkKey, uint64_t>> LabData::reviseNewestChunks() {
    std::vector<std::pair<heatmap::ChunkKey, uint64_t>> out;
    const auto a = availability();
    if (!a) return out;
    service_->onData([this, a, &out] {
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
            const auto chunk = store().cached(key);
            if (!chunk) continue;
            try {
                // Re-read and decode it (as a revised body would arrive), then
                // store it as a new version: every build that used it is stale.
                if (!reader) reader = std::make_unique<recording::Hmc2Reader>(root_);
                auto columns = std::make_shared<const heatmap::SparseColumns>(recording::buildChunk(*reader, key, marks));
                const auto stored = store().put(key, std::move(columns),
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

LabData::Stats LabData::stats() const { return service_->stats(); }
void LabData::refreshStats() { service_->refreshStats(); }

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
