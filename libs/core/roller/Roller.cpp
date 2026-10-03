#include "Roller.hpp"
#include "servermodel/PersistenceIo.hpp"
#include <QDateTime>
#include <QTimeZone>
#include <QSaveFile>
#include <fstream>
#include <numeric>
#include <chrono>

namespace sentinel::roller {
using nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr int64_t Minute = 60'000, Day = 86'400'000;
constexpr int Version = 1;
void checkpoint(const fs::path& path, const json& j) {
    // Hmc2Store has already durably created this product's parent and files.
    QSaveFile out(QString::fromStdString(path.string()));
    out.setDirectWriteFallback(false);
    const auto bytes = j.dump(2) + "\n";
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !out.flush())
        throw std::runtime_error("checkpoint temporary write failed");
#ifndef _WIN32
    int error = 0;
    if (!persistence::syncFileDescriptor(out.handle(), error)) throw std::runtime_error("checkpoint fsync failed");
#endif
    if (!out.commit()) throw std::runtime_error("checkpoint rename failed");
    int error2 = 0;
    if (!persistence::syncFilePath(path, error2) || !persistence::syncDirectory(path.parent_path(), error2))
        throw std::runtime_error("checkpoint durable rename failed");
}
uint64_t configHash(const recording::RecorderConfig& c) {
    // Stable semantic hash, with the complete policy/version in the checkpoint.
    json j = {{"version",Version},{"scale",c.priceScale},{"floor",c.sizeScale.floor},
              {"codes",c.sizeScale.codesPerOctave},{"lateness",c.latenessMs},{"grace",c.oneSidedGraceMs}};
    for (const auto& l : c.layers) j["layers"].push_back({l.name,l.rowTickUnits,l.lowFrac,l.highMult,l.hourlyRollup});
    uint64_t h = 14695981039346656037ull;
    for (unsigned char ch : j.dump()) { h ^= ch; h *= 1099511628211ull; }
    return h;
}
uint64_t outputBytes(const fs::path& root, const std::string& product) {
    uint64_t n = 0;
    if (fs::exists(root/product)) for (const auto& e : fs::recursive_directory_iterator(root/product))
        if (e.is_regular_file() && e.path().extension() == ".hmc2") n += e.file_size();
    return n;
}
}
int64_t parseTime(const std::string& text) {
    if (text.size() == 10) return parseTime(text + "T00:00:00Z");
    auto dt = QDateTime::fromString(QString::fromStdString(text), Qt::ISODateWithMs);
    if (!dt.isValid() || (!text.ends_with('Z') && text.find('+',10) == std::string::npos))
        throw std::runtime_error("expected UTC date or ISO timestamp: " + text);
    const auto ms = dt.toMSecsSinceEpoch();
    if (ms < recording::kHmc2MinMs || ms >= recording::kHmc2EndMs) throw std::runtime_error("time out of range");
    return ms;
}
void validateOutputProduct(const fs::path& root, const std::string& product) {
    capture::validateSymbol(product);
    const auto dir = root/product;
    if (fs::exists(dir) && !fs::is_regular_file(dir/"roller.json"))
        for (const auto& entry : fs::recursive_directory_iterator(dir))
            if (entry.is_regular_file() && entry.path().extension() == ".hmc2")
                throw std::runtime_error("refusing HMC2 product without roller.json: " + dir.string());
}
json roll(const RollOptions& o) {
    validateOutputProduct(o.outputRoot, o.product);
    if (o.fromMs < recording::kHmc2MinMs || o.toMs <= o.fromMs || o.toMs >= recording::kHmc2EndMs ||
        o.fromMs % Minute || o.toMs % Minute) throw std::runtime_error("roll range must contain complete UTC minutes");
    capture::validateSymbol(o.product);
    const auto started = std::chrono::steady_clock::now();
    JournalReader inventory(o.journalRoot, o.product);
    const auto cpPath = o.outputRoot/o.product/"roller.json";
    json cp = {{"rollerVersion",Version},{"product",o.product},{"days",json::object()}};
    if (fs::exists(cpPath)) {
        std::ifstream in(cpPath); in >> cp;
        if (cp.at("rollerVersion") != Version || cp.at("product") != o.product) throw std::runtime_error("checkpoint version/product mismatch");
    }
    uint64_t records = 0, bytes = 0, columns = 0;
    json days = json::array();
    bool pending = false;
    for (auto day = o.fromMs / Day * Day; day < o.toMs; day += Day) {
        const auto from = std::max(day,o.fromMs), end = std::min(day+Day,o.toMs);
        const auto key = std::to_string(day);
        if (cp["days"].contains(key)) {
            JournalReader check(o.journalRoot, o.product, cp["days"][key].at("pos").get<JournalPos>());
            JournalRecord saved;
            if (!check.next(saved)) throw std::runtime_error("checkpoint journal position unavailable");
        }
        const auto anchor = inventory.anchor(day);
        JournalReader reader(o.journalRoot,o.product,anchor);
        JournalFeed feed(o.product);
        std::unique_ptr<recording::BookRecorder> recorder;
        uint64_t hash = 0, checkpointColumns = 0;
        bool gridReady = false;
        int64_t latenessMs = recording::RecorderConfig{}.latenessMs;
        int64_t savedThrough = from, lastFence = -1;
        json dayReport = {{"day",QDateTime::fromMSecsSinceEpoch(day,QTimeZone::UTC).toString("yyyy-MM-dd").toStdString()}};
        JournalRecord input;
        JournalPos applied;
        feed.onSnapshot = [&](int64_t exchange, int64_t local, std::vector<recording::Level> levels) {
            if (!gridReady) {
                double bid = 0, ask = std::numeric_limits<double>::infinity();
                for (const auto& l : levels) if (l.size > 0) {
                    if (l.isBid) bid = std::max(bid,l.price); else ask = std::min(ask,l.price);
                }
                if (!(bid > 0) || !std::isfinite(ask)) return;
                const auto grid = deriveGrid(input.metadata,std::midpoint(bid,ask),o.overrides);
                auto cfg = grid.config(o.outputRoot);
                hash = configHash(cfg);
                latenessMs = cfg.latenessMs;
                cfg.commitFloorMs = from; cfg.commitCeilingMs = end;
                if (cp["days"].contains(key)) {
                    const auto& saved = cp["days"][key];
                    if (saved.at("configHash") != hash || saved.at("fromMs") != from)
                        throw std::logic_error("checkpoint policy/range mismatch; use a new output root");
                    savedThrough = saved.at("committedThroughMs");
                    cfg.commitFloorMs = std::max(from,savedThrough);
                }
                dayReport["grid"] = {{"priceScale",grid.priceScale},{"nearTick",grid.nearTick},
                                      {"deepTick",grid.deepTick},{"sizeFloor",grid.sizeFloor}};
                gridReady = true;
                if (o.dryRun) return;
                recorder = std::make_unique<recording::BookRecorder>(std::move(cfg));
            }
            if (recorder) recorder->onSnapshotAt(o.product,exchange,local,std::move(levels));
        };
        feed.onUpdates = [&](int64_t t,int64_t local,std::vector<recording::Level> levels) {
            if (recorder) recorder->onUpdatesAt(o.product,t,local,std::move(levels));
        };
        feed.onInvalid = [&](int64_t t,const std::string& reason) { if (recorder) recorder->onInvalid(o.product,t,reason); };
        feed.onTick = [&](int64_t t) { if (recorder) recorder->onTick(t); };
        const auto fence = [&] {
            if (!recorder) return;
            recorder->drain();
            const auto stats = recorder->stats();
            if (stats.diskErrors || stats.queueDrops) throw std::runtime_error("roller recorder failed; checkpoint not advanced");
            if (stats.columnsWritten == checkpointColumns) return;
            auto through = end;
            for (const auto* layer : {"near","deep"})
                through = std::min(through,recorder->watermarks(o.product,layer).minuteThroughMs);
            if (through < savedThrough) return;
            cp["days"][key] = {{"pos",applied},{"committedThroughMs",through},{"configHash",hash},{"fromMs",from}};
            // Also expose the newest checkpoint in the plan's flat schema.
            cp["pos"] = applied; cp["committedThroughMs"] = through; cp["configHash"] = hash;
            checkpoint(cpPath,cp);
            savedThrough = through; checkpointColumns = stats.columnsWritten;
        };
        while (reader.next(input)) {
            // No synthetic EOF tick: only actual journal records prove elapsed time.
            // Warmup is read in journal order; receipt clock need not be monotonic.
            feed.apply(input); applied = input.pos; ++records;
            if (input.record.kind == capture::Kind::Frame) bytes += input.record.payload.size();
            if (o.afterRecordForTest) o.afterRecordForTest(records);
            const auto minute = input.record.time.systemNs / 1'000'000 / Minute;
            if (minute != lastFence) { fence(); lastFence = minute; }
            // Receive time can lead the recorder's envelope-based integration
            // clock. Only a drained, committed watermark proves range completion.
            if (recorder && savedThrough >= end) break;
            // Dry runs retain their receive-time scan bound, without claiming commits.
            if (o.dryRun && input.record.time.systemNs / 1'000'000 >= end + latenessMs) break;
        }
        fence();
        if (recorder) columns += recorder->stats().columnsWritten;
        dayReport["committedThroughMs"] = savedThrough;
        days.push_back(std::move(dayReport)); pending = pending || reader.pending();
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    return {{"product",o.product},{"dryRun",o.dryRun},{"records",records},{"jsonBytes",bytes},
            {"wallSeconds",seconds},{"recordsPerSecond",records/seconds},{"jsonMBPerSecond",bytes/seconds/1e6},
            {"columnsWritten",columns},{"outputBytes",o.dryRun ? 0 : outputBytes(o.outputRoot,o.product)},
            {"pendingTail",pending},{"days",days}};
}
} // namespace sentinel::roller
