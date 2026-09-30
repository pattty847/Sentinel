#include "LocalChunkTransport.hpp"
#include "../servermodel/RecordingChunks.hpp"
#include "../SentinelLogging.hpp"
#include <algorithm>
#include <stdexcept>

namespace heatmap {
struct LocalChunkTransport::Worker : QObject {
    explicit Worker(std::filesystem::path path) : root(std::move(path)) {}
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
ChunkAvailability readAvailability(recording::Hmc2Reader &reader, const std::string &symbol) {
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
    : ChunkTransport(parent), worker_(new Worker(std::move(root))), poll_(new QTimer(this)) {
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
    workerThread_.quit();
    workerThread_.wait(); // all lambdas capturing this retire before QObject destruction
}
void LocalChunkTransport::start(std::vector<std::string> symbols) {
    symbols_ = std::move(symbols);
    emit connected();
    for (const auto &symbol : symbols_) refreshAvailability(symbol);
    poll_->start();
    sLog_Data("Local chunk transport started symbols=" << symbols_.size());
}
void LocalChunkTransport::refreshAvailability(const std::string &symbol) {
    QMetaObject::invokeMethod(worker_, [this, symbol] {
        try {
            auto &reader = worker_->reader();
            emit availability(readAvailability(reader, symbol));
        } catch (const std::exception &e) {
            sLog_Warning("Local chunk availability failed symbol=" << symbol << " error=" << e.what());
            emit failed(0, {}, QStringLiteral("unavailable"), QString::fromUtf8(e.what()));
        }
    }, Qt::QueuedConnection);
}
quint64 LocalChunkTransport::request(const std::string &symbol, const std::string &source,
    int64_t levelMs, std::vector<int64_t> starts, std::vector<std::optional<uint64_t>> haveHash) {
    const auto id = ++nextRequest_;
    QMetaObject::invokeMethod(worker_, [this, id, symbol, source, levelMs,
                                      starts = std::move(starts), hashes = std::move(haveHash)] {
        if (starts.empty() || starts.size() > protocol::chunkwire::kMaxStarts ||
            (!hashes.empty() && hashes.size() != starts.size())) {
            emit failed(id, {}, QStringLiteral("invalid_request"), QStringLiteral("invalid starts/hashes"));
            return;
        }
        try {
            auto &reader = worker_->reader();
            const auto a = readAvailability(reader, symbol);
            const auto marks = watermarks(a, source);
            for (size_t i = 0; i < starts.size(); ++i) {
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
                    frame->columns = recording::buildChunk(reader, key, marks);
                    frame->contentHash = chunkContentHash(encodeChunk(*frame));
                    if (!hashes.empty() && hashes[i] == frame->contentHash) {
                        frame->kind = ChunkKind::NotModified;
                        frame->columns = {};
                    }
                    sLog_Probe("chunks.fetch.local", "req=" << id << " start=" << key.startMs
                               << " sealed=" << frame->state.sealed);
                    emit received(id, std::move(frame));
                } catch (const std::invalid_argument &e) {
                    emit failed(id, key, QStringLiteral("invalid_request"), QString::fromUtf8(e.what()));
                } catch (const std::exception &e) {
                    emit failed(id, key, QStringLiteral("build_failed"), QString::fromUtf8(e.what()));
                }
            }
        } catch (const std::exception &e) {
            emit failed(id, {}, QStringLiteral("build_failed"), QString::fromUtf8(e.what()));
        }
    }, Qt::QueuedConnection);
    return id;
}
} // namespace heatmap
