#include "LocalChunkTransport.hpp"
#include "../servermodel/RecordingChunks.hpp"
#include "../SentinelLogging.hpp"
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <map>

namespace heatmap {
struct LocalChunkTransport::Worker : QObject {
    Worker(std::filesystem::path path, TestHooks h) : hooks(std::move(h)), root(std::move(path)) {}
    TestHooks hooks;
    std::map<std::string, ChunkAvailability> availability;
    sentinel::log_throttle::Site warnings;
    std::filesystem::path root;
    std::unique_ptr<recording::Hmc2Reader> session;
    recording::Hmc2Reader &reader() {
        // Lazy construction and QObject::deleteLater destruction both occur on
        // this worker's thread. Reuse the reader's bounded metadata cache.
        if (!session) session = std::make_unique<recording::Hmc2Reader>(root);
        return *session;
    }
};
namespace {
ChunkAvailability readAvailability(recording::Hmc2Reader &reader, const std::string &symbol, std::stop_token stop) {
    ChunkAvailability out;
    out.symbol = symbol;
    out.chunkWireVersion = kChunkWireVersion;
    for (const auto &source : kChunkSources) {
        protocol::chunkwire::SourceInfo info;
        info.id = source.id;
        info.migrationOnly = true;
        for (const auto level : {kMinuteMs, kHourMs}) {
            const auto span = chunkSpanMs(source.id, level);
            if (!span) continue;
            recording::ReadControl control;
            control.stop = stop;
            const auto a = reader.availability(symbol, std::string(source.hmc2Layer), level, control);
            if (control.status != recording::ReadStatus::Complete)
                throw std::runtime_error("local chunk availability scan incomplete");
            if (!a.oldestMs || !a.latestMs) continue;
            info.levels.push_back({level, span, *a.latestMs + level, *a.oldestMs, *a.latestMs});
            if (level == kMinuteMs && a.latestHeader) {
                const auto &h = *a.latestHeader;
                info.latestGrid = protocol::chunkwire::GridInfo{h.configHash, h.rowTickUnits, h.priceScale,
                                                               h.sizeScale.floor, h.sizeScale.codesPerOctave};
            }
        }
        if (!info.levels.empty()) out.sources.push_back(std::move(info));
    }
    return out;
}
bool sameAvailability(const ChunkAvailability &a, const ChunkAvailability &b) {
    if (a.symbol != b.symbol || a.chunkWireVersion != b.chunkWireVersion || a.sources.size() != b.sources.size()) return false;
    for (size_t i = 0; i < a.sources.size(); ++i) {
        const auto &x = a.sources[i], &y = b.sources[i];
        if (x.id != y.id || x.migrationOnly != y.migrationOnly || x.levels.size() != y.levels.size() ||
            x.latestGrid.has_value() != y.latestGrid.has_value()) return false;
        if (x.latestGrid) {
            const auto &g = *x.latestGrid, &h = *y.latestGrid;
            if (std::tie(g.configHash, g.rowTickUnits, g.priceScale, g.sizeFloor, g.codesPerOctave) !=
                std::tie(h.configHash, h.rowTickUnits, h.priceScale, h.sizeFloor, h.codesPerOctave)) return false;
        }
        for (size_t j = 0; j < x.levels.size(); ++j) {
            const auto &l = x.levels[j], &r = y.levels[j];
            if (std::tie(l.levelMs, l.chunkSpanMs, l.committedThroughMs, l.oldestMs, l.latestMs) !=
                std::tie(r.levelMs, r.chunkSpanMs, r.committedThroughMs, r.oldestMs, r.latestMs)) return false;
        }
    }
    return true;
}
recording::BookRecorder::Watermarks watermarks(const ChunkAvailability &a, const std::string &source) {
    recording::BookRecorder::Watermarks out;
    for (const auto &s : a.sources) {
        if (s.id != source) continue;
        for (const auto &l : s.levels) {
            if (l.levelMs == kMinuteMs) out.minuteThroughMs = l.committedThroughMs;
            if (l.levelMs == kHourMs) out.hourThroughMs = l.committedThroughMs;
        }
    }
    return out;
}
} // namespace
LocalChunkTransport::LocalChunkTransport(std::filesystem::path root, QObject *parent)
    : LocalChunkTransport(std::move(root), TestHooks{}, parent) {}
LocalChunkTransport::LocalChunkTransport(std::filesystem::path root, TestHooks hooks, QObject *parent)
    : ChunkTransport(parent), worker_(new Worker(std::move(root), std::move(hooks))), poll_(new QTimer(this)) {
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    workerThread_.setObjectName(QStringLiteral("local-chunks"));
    workerThread_.start();
    poll_->setInterval(1000);
    connect(poll_, &QTimer::timeout, this, [this] {
        for (const auto &symbol : symbols_) refreshAvailability(symbol);
    });
}
LocalChunkTransport::~LocalChunkTransport() {
    poll_->stop();
    stop_.request_stop();
    workerThread_.quit();
    workerThread_.wait(); // all lambdas capturing this retire before QObject destruction
}
void LocalChunkTransport::start(std::vector<std::string> symbols) {
    symbols_ = std::move(symbols);
    emit connected();
    for (const auto &symbol : symbols_) refreshAvailability(symbol, true);
    poll_->start();
    sLog_Data("Local chunk transport started symbols=" << symbols_.size());
}
void LocalChunkTransport::refreshAvailability(const std::string &symbol) {
    refreshAvailability(symbol, false);
}
void LocalChunkTransport::refreshAvailability(const std::string &symbol, bool force) {
    QMetaObject::invokeMethod(worker_, [this, symbol, force] {
        const auto stop = stop_.get_token();
        if (stop.stop_requested()) return;
        try {
            auto &reader = worker_->reader();
            if (worker_->hooks.beforeAvailability) worker_->hooks.beforeAvailability(reader, stop);
            auto next = readAvailability(reader, symbol, stop);
            if (stop.stop_requested()) return;
            const auto previous = worker_->availability.find(symbol);
            const bool changed = previous == worker_->availability.end() || !sameAvailability(previous->second, next);
            worker_->availability[symbol] = next;
            // A new connection always needs a fresh message, even if unchanged.
            if (force || changed) emit availability(std::move(next));
        } catch (const std::exception &e) {
            if (stop.stop_requested()) return;
            uint32_t suppressed = 0;
            if (worker_->warnings.admit(5000, sentinel::log_throttle::nowMs(), suppressed))
                sLog_Warning("Local chunk availability failed symbol=" << symbol << " error=" << e.what()
                             << sentinel::log_throttle::Suppressed{suppressed});
            // This scan has no request identity. Keep the last good snapshot;
            // request failures are reserved for work admitted by request().
        }
    }, Qt::QueuedConnection);
}
quint64 LocalChunkTransport::request(const std::string &symbol, const std::string &source,
    int64_t levelMs, std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) {
    const auto id = ++nextRequest_;
    QMetaObject::invokeMethod(worker_, [this, id, symbol, source, levelMs,
                                      starts = std::move(starts), hashes = std::move(haveHash)] {
        const auto stop = stop_.get_token();
        if (stop.stop_requested()) return;
        if (starts.empty() || starts.size() > protocol::chunkwire::kMaxStarts ||
            (!hashes.empty() && hashes.size() != starts.size())) {
            emit failed(id, {}, QStringLiteral("invalid_request"), QStringLiteral("invalid starts/hashes"));
            return;
        }
        try {
            auto &reader = worker_->reader();
            const auto a = worker_->availability.find(symbol);
            if (a == worker_->availability.end()) {
                emit failed(id, {}, QStringLiteral("unavailable"), QStringLiteral("local availability not loaded"));
                return;
            }
            const auto marks = watermarks(a->second, source);
            for (size_t i = 0; i < starts.size(); ++i) {
                if (stop.stop_requested()) return;
                const ChunkKey key{symbol, source, levelMs, starts[i]};
                try {
                    const auto end = recording::chunkEndMs(key); // validate before arithmetic
                    const auto through = levelMs == kMinuteMs ? marks.minuteThroughMs : marks.hourThroughMs;
                    if (!through) {
                        emit failed(id, key, QStringLiteral("unavailable"), QStringLiteral("no local recording level"));
                        continue;
                    }
                    auto frame = std::make_shared<ChunkFrame>();
                    frame->key = key;
                    const auto complete = std::max<int64_t>(0, (std::min(through, end) - key.startMs) / levelMs);
                    frame->state = recording::chunkState(key, marks, uint64_t(complete) + 1);
                    if (worker_->hooks.beforeBuild) worker_->hooks.beforeBuild(reader, stop);
                    recording::ReadControl control;
                    control.stop = stop;
                    frame->columns = recording::buildChunk(reader, key, marks, control);
                    if (stop.stop_requested()) return;
                    frame->contentHash = chunkContentHash(encodeChunk(*frame));
                    if (stop.stop_requested()) return;
                    if (!hashes.empty() && hashes[i] == frame->contentHash) {
                        frame->kind = ChunkKind::NotModified;
                        frame->columns = {};
                    }
                    sLog_Probe("chunks.fetch.local", "req=" << id << " start=" << key.startMs
                               << " sealed=" << frame->state.sealed);
                    emit received(id, std::move(frame));
                } catch (const std::invalid_argument &e) {
                    if (stop.stop_requested()) return;
                    emit failed(id, key, QStringLiteral("invalid_request"), QString::fromUtf8(e.what()));
                } catch (const std::exception &e) {
                    if (stop.stop_requested()) return;
                    emit failed(id, key, QStringLiteral("build_failed"), QString::fromUtf8(e.what()));
                }
            }
        } catch (const std::exception &e) {
            if (stop.stop_requested()) return;
            emit failed(id, {}, QStringLiteral("build_failed"), QString::fromUtf8(e.what()));
        }
    }, Qt::QueuedConnection);
    return id;
}
} // namespace heatmap
