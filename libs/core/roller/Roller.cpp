#include "Roller.hpp"
#include "AnchorStore.hpp"
#include "LevelCheckpoint.hpp"
#include "CheckpointBytes.hpp"
#include "levelstore/LevelRecorder.hpp"
#include "metrics/MetricsRegistry.hpp"
#include <map>
#include <mutex>
#include "SentinelLogging.hpp"
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
constexpr int64_t Minute = 60'000, Quarter = 900'000, Day = 86'400'000;
constexpr int Version = 1;
std::shared_ptr<metrics::Counter> anchorFailures(const std::string& product) {
    static std::mutex mutex;
    static std::map<std::string,std::shared_ptr<metrics::Counter>> counters;
    std::lock_guard lock(mutex);
    auto& counter = counters[product];
    if (!counter) counter = std::make_shared<metrics::Counter>();
    return counter;
}
}
void registerAnchorMetrics(metrics::MetricsRegistry& registry, const std::string& product) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto name = "sentinel_roller_anchor_failures_total";
    if (registry.hasSeries(name,{{"product",product}})) return;
    const auto counter = anchorFailures(product);
    registry.counterFn(name,"Failed anchor writes, degraded skips, reads or pruning attempts.",{{"product",product}},
        [counter] { return static_cast<double>(counter->value()); });
}
void writeCheckpoint(const fs::path& path, const json& j, const std::function<void()>& midWrite) {
    // Hmc2Store has already durably created this product's parent and files.
    QSaveFile out(QString::fromStdString(path.string()));
    out.setDirectWriteFallback(false);
    auto stored = j;
    if (stored.value("rollerVersion",0) == 2) {
        stored["stateEncoding"] = "zstd-hex-v1";
        for (auto& day : stored.at("days")) {
            day["recorder"] = packCheckpointBytes(day.at("recorder").get<std::vector<uint8_t>>());
            day["level"]["state"] = packCheckpointBytes(day.at("level").at("state").get<std::vector<uint8_t>>());
        }
    }
    const auto bytes = stored.dump(2) + "\n";
    const auto half = bytes.size()/2;
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes.data(), qint64(half)) != qint64(half) || !out.flush())
        throw std::runtime_error("checkpoint temporary write failed");
    if (midWrite) midWrite();
    if (out.write(bytes.data()+half, qint64(bytes.size()-half)) != qint64(bytes.size()-half) || !out.flush())
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
namespace {
uint64_t configHash(const recording::RecorderConfig& c) {
    // Roller checkpoint identity is frozen: this literal belongs to existing
    // checkpoints and anchors, not a live setting. The one-sided policy lives
    // in replay-state policy; a change there falls back to journal replay.
    json j = {{"version",Version},{"scale",c.priceScale},{"floor",c.sizeScale.floor},
              {"codes",c.sizeScale.codesPerOctave},{"lateness",c.latenessMs},{"grace",5000}};
    for (const auto& l : c.layers) j["layers"].push_back({l.name,l.rowTickUnits,l.lowFrac,l.highMult,l.hourlyRollup});
    uint64_t h = 14695981039346656037ull;
    for (unsigned char ch : j.dump()) { h ^= ch; h *= 1099511628211ull; }
    return h;
}
uint64_t outputBytes(const fs::path& root, const std::string& product) {
    uint64_t n = 0;
    if (fs::exists(root/product)) for (auto it = fs::recursive_directory_iterator(root/product); it != fs::recursive_directory_iterator{}; ++it) {
        if (it->path() == root/product/"anchors") { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file() && it->path().extension() == ".hmc2") n += it->file_size();
    }
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
        for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator{}; ++it) {
            if (it->path() == dir/"anchors") { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file() && it->path().extension() == ".hmc2")
                throw std::runtime_error("refusing HMC2 product without roller.json: " + dir.string());
        }
}
json roll(const RollOptions& o) {
    validateOutputProduct(o.outputRoot, o.product);
    if (!o.levelLogRoot.empty() && (o.levelOutput || !o.useAnchors || !o.writeAnchors || o.dryRun))
        throw std::runtime_error("durable C requires anchors and exclusive levelLogRoot output");
    std::unique_ptr<levelstore::LevelLog> log;
    if (!o.levelLogRoot.empty()) log = std::make_unique<levelstore::LevelLog>(o.levelLogRoot,o.product,
        o.levelLogFiles ? o.levelLogFiles : std::make_shared<levelstore::LevelLogFiles>());
    auto* output = log ? log.get() : o.levelOutput;
    const auto point = [&](const char* name) { if (o.persistencePointForTest) o.persistencePointForTest(name); };
    if (o.fromMs < recording::kHmc2MinMs || o.toMs <= o.fromMs || o.toMs >= recording::kHmc2EndMs ||
        o.fromMs % Minute || o.toMs % Minute) throw std::runtime_error("roll range must contain complete UTC minutes");
    capture::validateSymbol(o.product);
    const auto started = std::chrono::steady_clock::now();
    JournalReader inventory(o.journalRoot, o.product);
    const auto cpPath = o.outputRoot/o.product/"roller.json";
    json cp = {{"rollerVersion",log ? 2 : Version},{"product",o.product},{"days",json::object()}};
    if (o.levelOutput && fs::exists(cpPath))
        throw std::runtime_error("non-durable C output cannot resume; use levelLogRoot");
    if (fs::exists(cpPath)) {
        std::ifstream in(cpPath); in >> cp;
        if (cp.at("rollerVersion") == 2) {
            if (cp.at("stateEncoding") != "zstd-hex-v1") throw std::runtime_error("C checkpoint state encoding mismatch");
            for (auto& day : cp.at("days")) {
                day["recorder"] = unpackCheckpointBytes(day.at("recorder").get<std::string>());
                day["level"]["state"] = unpackCheckpointBytes(day.at("level").at("state").get<std::string>());
            }
        }
        if ((cp.at("rollerVersion") != Version && cp.at("rollerVersion") != 2) || cp.at("product") != o.product) throw std::runtime_error("checkpoint version/product mismatch");
    }
    if (log && cp.at("rollerVersion") != 2) throw std::runtime_error("C resume requires paired C state; HMC2-only checkpoint");
    if (!log && cp.at("rollerVersion") == 2) throw std::runtime_error("C checkpoint requires durable C output");
    if (log) for (const auto& saved : cp.at("days")) {
        if (saved.at("committedThroughMs").get<int64_t>() > o.toMs)
            throw std::runtime_error("C resume range would discard later fenced output");
        const auto& c = saved.at("level");
        levelstore::LevelRecorder check(levelBinding(c.at("binding")),log.get());
        std::optional<int64_t> clock;
        if (!c.at("clock").is_null()) clock = c.at("clock").get<int64_t>();
        importLevelFence(check,c,clock,saved.at("recorder").get<std::vector<uint8_t>>());
    }
    bool logPrepared = false;
    uint64_t records = 0, bytes = 0, columns = 0, decoded = 0;
    const auto anchorRoot = o.anchorRoot.empty() ? o.outputRoot : o.anchorRoot;
    json days = json::array();
    bool pending = false, warnedDegraded = false;
    const auto anchorFailureCounter = anchorFailures(o.product);
    for (auto day = o.fromMs / Day * Day; day < o.toMs; day += Day) {
        const auto from = std::max(day,o.fromMs), end = std::min(day+Day,o.toMs);
        const auto key = std::to_string(day);
        std::optional<JournalPos> checkpoint;
        int64_t targetBoundary = day;
        if (cp["days"].contains(key)) {
            const auto& saved = cp["days"][key];
            checkpoint = saved.at("pos").get<JournalPos>();
            if (saved.contains("receiveThroughMs")) targetBoundary = saved.at("receiveThroughMs");
            else {
                // Old checkpoints have no receive high-water. A bounded cursor
                // read supplies the target time without scanning for a snapshot.
                JournalReader check(o.journalRoot,o.product,checkpoint);
                JournalRecord cursor;
                if (!check.next(cursor)) throw std::runtime_error("checkpoint journal position unavailable");
                targetBoundary = cursor.record.time.systemNs / 1'000'000;
                decoded += check.decodedRecords();
            }
        }
        // A cache miss takes the original snapshot path. Do not scan for a
        // snapshot until after candidates have been validated (bounded restart).
        std::unique_ptr<JournalReader> reader;
        JournalFeed feed(o.product);
        // Architecture C for this day: declared before the recorder, so it is
        // destroyed after it (the recorder's worker calls it until then).
        std::unique_ptr<levelstore::LevelRecorder> level;
        std::optional<Grid> dayGrid;
        std::unique_ptr<recording::BookRecorder> recorder;
        // Destroyed first: the live lead forgets this day's recorder and feed.
        struct Detach {
            const RollOptions& o;
            ~Detach() {
                if (o.onLevelRecorder) o.onLevelRecorder(nullptr);
                if (o.onRecorder) o.onRecorder(nullptr, nullptr, 0);
            }
        } detach{o};
        uint64_t hash = 0, checkpointColumns = 0, importedColumns = 0;
        bool gridReady = false, recoveryComplete = false;
        bool checkpointReplayed = !checkpoint;
        int64_t latenessMs = recording::RecorderConfig{}.latenessMs;
        int64_t savedThrough = from, lastFence = -1, lastTailFence = -1;
        json dayReport = {{"day",QDateTime::fromMSecsSinceEpoch(day,QTimeZone::UTC).toString("yyyy-MM-dd").toStdString()}};
        const auto anchorFailure = [&](const char* operation, int64_t boundary, const std::string& error, bool warn = true) {
            const std::string field = std::string("anchor") + operation + "Failures";
            dayReport[field] = dayReport.value(field,uint64_t{}) + 1;
            anchorFailureCounter->inc();
            if (warn) {
                if (std::string_view(operation) == "Write")
                    sLog_Warning("Roller anchor write failed product=" << o.product << " boundaryMs=" << boundary << " error=" << error);
                else
                    sLog_Warning("Roller anchor " << operation << " failed product=" << o.product << " boundaryMs=" << boundary << " error=" << error);
            }
            if (o.anchorFailuresFatal) throw std::runtime_error("anchor " + std::string(operation) + " failed: " + error);
        };
        JournalRecord input;
        JournalPos applied, gridSnapshot, restoreCursor;
        bool restoreCursorApplied = true;
        int64_t restoreLocal = 0;
        double referenceMid = 0;
        json gridMetadata;
        JournalBook book;
        bool bookUp = true;
        int64_t recorderFloor = from, nextBoundary = day, receiveThrough = 0;
        const auto configure = [&](const Grid& grid, int64_t floor) {
            auto cfg = grid.config(o.outputRoot);
            if (o.productWriterLease) cfg.writerProduct = o.product;
            cfg.commitFloorMs = floor;
            cfg.commitCeilingMs = o.allowGrowingEnd ? day + Day : end;
            // History writers publish committed minutes only; their state is
            // portable between offline replay and live serving.
            cfg.publication = recording::RecorderConfig::Publication::Finals;
            if (o.publisher) {
                cfg.publisher = [&, publish=o.publisher](auto record) {
                    if (record->bucketStartMs + record->header.tfMs > savedThrough) publish(std::move(record));
                };
            }
            cfg.minuteColumnsCounter = o.minuteColumnsCounter;
            cfg.writeErrorsCounter = o.writeErrorsCounter;
            cfg.sink = o.recorderSink;
            return cfg;
        };
        // Day recreation: one LevelRecorder per day's recorder, bound to the same
        // floor and ceiling the recorder commits HMC2 within and to its price scale.
        const auto makeLevel = [&](const Grid& grid, int64_t floor) -> std::unique_ptr<levelstore::LevelRecorder> {
            if (!output) return nullptr;
            return std::make_unique<levelstore::LevelRecorder>(levelstore::DayBinding{o.product, floor,
                o.allowGrowingEnd ? day + Day : end, recording::priceScaleDigits(grid.priceScale)}, output);
        };
        const auto describeGrid = [&](const Grid& grid) {
            dayReport["grid"] = {{"priceScale",grid.priceScale},{"nearTick",grid.nearTick},
                                  {"deepTick",grid.deepTick},{"sizeFloor",grid.sizeFloor}};
        };

        const auto claimProvenance = [&] {
            if ((o.productWriterLease || log) && !fs::exists(cpPath)) {
                // Claim provenance before the first asynchronous write, so
                // a first-minute crash can retry this shadow product safely.
                fs::create_directories(cpPath.parent_path());
                int error = 0;
                if (!persistence::syncDirectory(o.outputRoot, error))
                    throw std::runtime_error("shadow product directory sync failed");
                writeCheckpoint(cpPath, cp);
            }
        };
        feed.onSnapshot = [&](int64_t exchange, int64_t local, std::vector<recording::Level> levels) {
            if (!o.anchorBook) book.snapshot(exchange,levels);
            if (!gridReady) {
                double bid = 0, ask = std::numeric_limits<double>::infinity();
                for (const auto& l : levels) if (l.size > 0) {
                    if (l.isBid) bid = std::max(bid,l.price); else ask = std::min(ask,l.price);
                }
                if (!(bid > 0) || !std::isfinite(ask)) return;
                referenceMid = std::midpoint(bid,ask);
                gridSnapshot = input.pos; gridMetadata = input.metadata;
                const auto grid = deriveGrid(gridMetadata,referenceMid,o.overrides);
                auto cfg = grid.config(o.outputRoot);
                hash = configHash(cfg);
                if (cp["days"].contains(key)) {
                    const auto& saved = cp["days"][key];
                    if (saved.at("configHash") != hash || saved.at("fromMs") != from)
                        throw std::logic_error("checkpoint policy/range mismatch; use a new output root");
                    savedThrough = saved.at("committedThroughMs");
                    recorderFloor = log ? from : std::max(from,savedThrough);
                }
                cfg = configure(grid,recorderFloor);
                latenessMs = cfg.latenessMs;
                describeGrid(grid);
                gridReady = true;
                if (o.dryRun) return;
                level = makeLevel(grid,recorderFloor);
                if (level) cfg.sink = level.get();
                dayGrid = grid;
                recorder = std::make_unique<recording::BookRecorder>(std::move(cfg));
                if (o.onRecorder) o.onRecorder(recorder.get(), &feed, end);
                if (o.onLevelRecorder && level) o.onLevelRecorder(level.get());
                claimProvenance();
            }
            if (recorder) recorder->onSnapshotAt(o.product,exchange,local,std::move(levels),feed.sequence(),feed.takePriceText());
        };
        feed.onUpdates = [&](int64_t t,int64_t local,std::vector<recording::Level> levels) {
            if (!o.anchorBook) book.apply(t,levels);
            if (recorder) recorder->onUpdatesAt(o.product,t,local,std::move(levels),feed.sequence(),feed.takePriceText());
        };
        feed.onInvalid = [&](int64_t t,const std::string& reason) { if (!o.anchorBook) book.clear(); if (o.onInvalid) o.onInvalid(reason); if (recorder) recorder->onInvalid(o.product,t,reason); };
        feed.onTick = [&](int64_t t) { if (recorder) recorder->onTick(t); };
        feed.onConnection = [&](bool up) { bookUp = up; };
        if (o.onFeed) o.onFeed(feed);
        std::vector<fs::path> candidates;
        if (o.useAnchors && !o.dryRun) {
            try { candidates = AnchorStore::candidates(anchorRoot,o.product,day); }
            catch (const std::exception& e) { anchorFailure("Read",day,e.what()); }
            catch (...) { anchorFailure("Read",day,"unknown exception"); }
        }
        if (o.levelOutput && o.useAnchors && !o.dryRun && !candidates.empty())
            throw std::runtime_error("non-durable C output cannot restore anchors; use levelLogRoot");
        if (o.useAnchors && !o.dryRun) for (const auto& file : candidates) {
            // Canonical names encode UTC boundaries. Filter future candidates
            // before decompressing megabytes of recorder state.
            const auto name = file.stem().string();
            const auto time = QTime::fromString(QString::fromStdString(name),"HHmm");
            if (!time.isValid() || day + time.msecsSinceStartOfDay() > targetBoundary) continue;
            uint64_t candidateDecoded = 0;
            bool rejectedPrefix = false;
            try {
                dayReport["anchorCandidatesDecoded"] = dayReport.value("anchorCandidatesDecoded",0u) + 1;
                auto a = AnchorStore::load(file);
                if (a.boundaryMs != day + time.msecsSinceStartOfDay())
                    throw std::runtime_error("anchor filename/boundary mismatch");
                // With no checkpoint only midnight is a complete independent
                // starting point; intraday anchors require the committed prefix.
                if (!checkpoint && a.boundaryMs != day) continue;
                const auto grid = deriveGrid(a.metadata,a.referenceMid,o.overrides);
                const auto expectedHash = configHash(grid.config(o.outputRoot));
                const AnchorIdentity expected{o.product,day,from,end,expectedHash};
                if (a.identity != expected &&
                    !(o.allowGrowingEnd && a.identity.product == expected.product &&
                      a.identity.dayMs == expected.dayMs && a.identity.fromMs == expected.fromMs &&
                      a.identity.configHash == expected.configHash &&
                      a.identity.toMs >= a.boundaryMs && a.identity.toMs < end))
                    throw std::runtime_error("anchor policy/range mismatch");
                const auto state = json::from_cbor(a.feed.bytes);
                const bool before = !state.at("resumeAt").is_null();
                const auto cursorPos = before ? state.at("resumeAt").get<JournalPos>() : a.pos;
                if (before && (a.boundaryMs != day || cursorPos.product != o.product ||
                    (a.pos.run == cursorPos.run && std::pair(a.pos.block,a.pos.record) >= std::pair(cursorPos.block,cursorPos.record))))
                    throw std::runtime_error("invalid midnight resume cursor");
                if (checkpoint) {
                    const auto& saved = cp["days"][key];
                    if (saved.at("configHash") != expectedHash || saved.at("fromMs") != from)
                        throw std::runtime_error("anchor checkpoint identity mismatch");
                    // Conservative across runs: an older-run candidate falls
                    // back unless a same-run anchor is available.
                    if (cursorPos.run != checkpoint->run || std::pair(cursorPos.block,cursorPos.record) >
                            std::pair(checkpoint->block,checkpoint->record)) continue;
                }
                if (o.anchorAllowed && !o.anchorAllowed(cursorPos)) continue;
                auto candidateReader = o.anchorReaderForTest ? o.anchorReaderForTest(cursorPos) :
                    std::make_unique<JournalReader>(o.journalRoot,o.product,cursorPos);
                JournalRecord cursor;
                const bool present = candidateReader->next(cursor);
                candidateDecoded = candidateReader->decodedRecords();
                if (!present || cursor.pos != cursorPos)
                    throw std::runtime_error("anchor durable position unavailable");
                JournalFeed candidateFeed(o.product); candidateFeed.importState(state.at("feed"));
                auto candidateBook = JournalBook::fromState(state.at("book"));
                const bool candidateUp = state.at("up");
                const auto candidateReceive = state.at("receiveThroughMs").get<int64_t>();
                const auto floor = state.at("recorderFloor").get<int64_t>();
                const auto lastLocal = state.at("feed").at("lastLocal").get<int64_t>();
                if (floor < from || floor > end || (!checkpoint && floor != from) ||
                    candidateReceive < lastLocal || state.at("feed").at("run") != a.pos.run ||
                    (!before && (lastLocal != cursor.record.time.systemNs / 1'000'000 ||
                        candidateReceive < a.boundaryMs || state.at("feed").at("connection") != cursor.record.connection)) ||
                    (before && (lastLocal >= day || candidateReceive >= day)) ||
                    cursor.record.time.systemNs / 1'000'000 < a.boundaryMs)
                    throw std::runtime_error("anchor cursor/state mismatch");
                // Validate the committed overlap before importing. A newly
                // damaged block in this interval would otherwise make duplicate
                // verification fail repeatedly against the old good HMC2 prefix.
                // Keep decoded records to replay once, so the normal bound is
                // still one quarter plus the selected block (not two scans).
                std::vector<JournalRecord> overlap;
                size_t overlapBytes = 0;
                const auto retain = [&](JournalRecord r) {
                    overlapBytes += r.record.payload.size() + r.metadata.dump().size()*8 + sizeof(r);
                    if (overlapBytes > 256*1024*1024) {
                        rejectedPrefix = true;
                        throw std::runtime_error("anchor committed overlap exceeds buffer bound");
                    }
                    overlap.push_back(std::move(r));
                };
                auto checked = cursor.pos;
                if (before) {
                    cursor.gapBefore = state.at("resumeGap").get<bool>();
                    retain(std::move(cursor));
                }
                rejectedPrefix = true; // any reader/retention exception makes older candidates redundant
                while (checkpoint && checked != *checkpoint) {
                    JournalRecord r;
                    const bool present = o.anchorOverlapReadForTest ? o.anchorOverlapReadForTest(*candidateReader,r) : candidateReader->next(r);
                    candidateDecoded = candidateReader->decodedRecords();
                    if (!present || r.gapBefore || r.pos.run != checkpoint->run ||
                        std::pair(r.pos.block,r.pos.record) > std::pair(checkpoint->block,checkpoint->record)) {
                        rejectedPrefix = true;
                        throw std::runtime_error("anchor committed overlap is unavailable or damaged");
                    }
                    checked = r.pos;
                    retain(std::move(r));
                }
                rejectedPrefix = false;
                for (auto it=overlap.rbegin();it!=overlap.rend();++it)
                    candidateReader->putBack(std::move(*it));
                auto candidateLevel = makeLevel(grid,floor);
                auto candidateConfig = configure(grid,floor);
                if (candidateLevel) candidateConfig.sink = candidateLevel.get();
                auto candidate = std::make_unique<recording::BookRecorder>(std::move(candidateConfig));
                candidate->importState(a.recorder.bytes);
                if (log) {
                    importLevelFence(*candidateLevel,a.level,candidate->checkpointClock(o.product),a.recorder.bytes);
                    cutLevelFence(*log,a.level);
                    logPrepared = true;
                }
                // Everything above is validation on fresh, unpublished objects.
                // Preserve installed callbacks: importing never invokes them.
                feed.importState(state.at("feed"));
                book = std::move(candidateBook); bookUp = candidateUp;
                recorder = std::move(candidate); reader = std::move(candidateReader);
                level = std::move(candidateLevel); dayGrid = grid;
                gridReady = true; hash = expectedHash; recorderFloor = floor;
                referenceMid = a.referenceMid; gridSnapshot = a.gridSnapshot; gridMetadata = a.metadata;
                applied = a.pos; restoreCursor = cursorPos; restoreCursorApplied = !before;
                checkpointReplayed = !checkpoint || (!before && a.pos == *checkpoint);
                nextBoundary = a.boundaryMs + Quarter;
                receiveThrough = candidateReceive; restoreLocal = lastLocal;
                if (checkpoint) savedThrough = cp["days"][key].at("committedThroughMs");
                checkpointColumns = recorder->stats().columnsWritten;
                importedColumns = checkpointColumns;
                describeGrid(grid);
                dayReport["anchorBoundaryMs"] = a.boundaryMs;
                dayReport["anchorPos"] = a.pos;
            } catch (const std::exception& e) {
                if (log) throw;
                decoded += candidateDecoded;
                dayReport["anchorRejection"] = e.what();
                if (rejectedPrefix) break; // older candidates span the same bad overlap
                continue;
            } catch (...) {
                if (log) throw;
                decoded += candidateDecoded;
                dayReport["anchorRejection"] = "unknown candidate exception";
                if (rejectedPrefix) break;
                continue;
            }
            // Consumer installation errors must propagate, never re-enter fallback
            // with partially attached external consumers.
            claimProvenance();
            sLog_Data("Roller anchor restored product=" << o.product
                << " boundaryMs=" << dayReport.at("anchorBoundaryMs").get<int64_t>()
                << " cursorRun=" << restoreCursor.run << " cursorBlock=" << restoreCursor.block
                << " cursorRecord=" << restoreCursor.record << " cursorApplied=" << restoreCursorApplied);
            if (o.onAnchorRestore) o.onAnchorRestore(restoreCursor,book,bookUp,restoreCursorApplied,restoreLocal);
            if (o.onRecorder) o.onRecorder(recorder.get(), &feed, end);
            if (o.onLevelRecorder && level) o.onLevelRecorder(level.get());
            if (log && checkpointReplayed && checkpoint && receiveThrough >= targetBoundary) {
                point("recovery-replayed"); recoveryComplete = true;
            }
            // A restore exactly at the checkpoint can reach the socket tip
            // without processing another record. Reaffirm that durable progress
            // so serving readiness need not wait for a future minute commit.
            if (checkpoint && savedThrough > from && o.onCommitted &&
                recorder->watermarks(o.product,"near").minuteThroughMs >= savedThrough &&
                recorder->watermarks(o.product,"deep").minuteThroughMs >= savedThrough)
                o.onCommitted(savedThrough);
            break;
        }
        if (!reader) {
            if (log) {
                for (const auto& file : candidates)
                    if (AnchorStore::load(file).level.is_null())
                        throw std::runtime_error("C resume requires paired C state; HMC2-only anchor");
                // Before the first quarter fence, replay from the original floor.
                // Preserve only an explicitly fenced preceding day's output.
                if (!logPrepared) {
                    json previous = nullptr;
                    int64_t previousDay = -1;
                    for (const auto& [date,saved] : cp.at("days").items()) {
                        const auto d = std::stoll(date);
                        if (d < day && d > previousDay) { previous = saved.at("level"); previousDay = d; }
                    }
                    cutLevelFence(*log,previous);
                }
                logPrepared = true;
            }
            if (dayReport.contains("anchorRejection"))
                sLog_Warning("Roller anchor fallback product=" << o.product << " dayMs=" << day
                    << " reason=" << dayReport.at("anchorRejection").get<std::string>());
            if (checkpoint) {
                JournalReader check(o.journalRoot,o.product,checkpoint);
                JournalRecord saved;
                if (!check.next(saved)) throw std::runtime_error("checkpoint journal position unavailable");
                decoded += check.decodedRecords();
            }
            reader = std::make_unique<JournalReader>(o.journalRoot,o.product,inventory.anchor(day));
        }
        // In-process checkpoint (RollOptions::restoreAfterRecordForTest): the recorder
        // state was exported at this drained position; C exports against the same clock
        // and bytes, both are destroyed and fresh ones import before the next record.
        const auto restoreInProcess = [&](const std::vector<uint8_t>& recorderBytes, int64_t boundary) {
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();
            const auto checkpoint = recorder->checkpointClock(o.product);
            std::vector<uint8_t> levelBytes;
            if (level) levelBytes = level->exportState(checkpoint,recorderBytes);
            const auto t1 = clock::now();
            const size_t levelMemory = level ? level->memoryBytes() : 0;
            if (o.onLevelRecorder && level) o.onLevelRecorder(nullptr);
            if (o.onRecorder) o.onRecorder(nullptr, nullptr, 0);
            recorder.reset(); // drained: drops only what the bytes hold
            level.reset();
            auto freshLevel = makeLevel(*dayGrid,recorderFloor);
            auto cfg = configure(*dayGrid,recorderFloor);
            if (freshLevel) cfg.sink = freshLevel.get();
            auto fresh = std::make_unique<recording::BookRecorder>(std::move(cfg));
            const auto t2 = clock::now();
            fresh->importState(recorderBytes);
            const auto t3 = clock::now();
            levelstore::LevelRecorder::ImportResult imported{true, {}};
            if (freshLevel) imported = freshLevel->importState(levelBytes,fresh->checkpointClock(o.product),recorderBytes);
            if (freshLevel) freshLevel->throwIfFailed();
            const auto t4 = clock::now();
            level = std::move(freshLevel); recorder = std::move(fresh);
            if (o.onRecorder) o.onRecorder(recorder.get(), &feed, end);
            if (o.onLevelRecorder && level) o.onLevelRecorder(level.get());
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
            if (o.onInProcessRestoreForTest)
                o.onInProcessRestoreForTest({{"boundaryMs",boundary},{"pos",applied},{"clock",checkpoint ? json(*checkpoint) : json()},
                    {"recorderBytes",recorderBytes.size()},{"levelBytes",levelBytes.size()},{"levelMemoryBytes",levelMemory},
                    {"levelExportMs",ms(t0,t1)},{"recorderImportMs",ms(t2,t3)},{"levelImportMs",ms(t3,t4)},
                    {"levelImported",imported.ok},{"levelImportReason",imported.reason}});
        };
        const auto saveAnchor = [&](int64_t boundary, const JournalRecord* resume = nullptr) {
            // Snapshot fallback may be warming up below an existing output
            // floor. Such a state cannot replace a standalone midnight anchor
            // or an older cache with a different committed prefix.
            if (!o.writeAnchors || !recorder || boundary < savedThrough ||
                (boundary == day && recorderFloor != from)) return;
            if (level) {
                recorder->drain();
                level->throwIfFailed(); // must not be swallowed by optional-anchor error handling
            }
            bool degraded = false;
            std::vector<uint8_t> exported;
            try {
                const auto start = std::chrono::steady_clock::now();
                ReplayAnchor a;
                // Export drains and reports degraded state without serializing it.
                a.recorder.bytes = o.anchorExportForTest ? o.anchorExportForTest(*recorder) : recorder->exportState();
                if (o.restoreAtAnchorForTest) exported = a.recorder.bytes;
                if (a.recorder.bytes.empty()) {
                    degraded = true;
                    throw std::runtime_error("degraded recorder: diskErrors");
                }
                if (o.anchorBook) {
                    auto state = o.anchorBook(); book = std::move(state.first); bookUp = state.second;
                }
                a.identity = {o.product,day,from,end,hash}; a.boundaryMs = boundary; a.pos = applied;
                a.gridSnapshot = gridSnapshot; a.referenceMid = referenceMid; a.metadata = gridMetadata;
                a.feed.bytes = json::to_cbor({{"feed",feed.exportState()},{"book",book.exportState()},
                    {"up",bookUp},{"recorderFloor",recorderFloor},{"receiveThroughMs",receiveThrough},
                    {"resumeAt",resume ? json(resume->pos) : json()},{"resumeGap",resume && resume->gapBefore}});
                const auto exportMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                if (log) {
                    a.level = levelFence(*level,*log,recorder->checkpointClock(o.product),a.recorder.bytes,o.levelTiming,"anchor-sync");
                    point("anchor-synced");
                }
                const auto written = AnchorStore::write(anchorRoot,a,[&] { point("anchor-mid-write"); });
                dayReport["anchorBytes"] = written.encodedBytes;
                sLog_Data("Roller anchor written product=" << o.product << " boundaryMs=" << boundary
                    << " exportMs=" << exportMs << " encodeMs=" << written.encodeMs
                    << " encodedBytes=" << written.encodedBytes << " writeMs=" << written.writeMs);
            } catch (const std::exception& e) {
                if (log) throw;
                const bool warn = !degraded || !warnedDegraded;
                warnedDegraded = warnedDegraded || degraded;
                anchorFailure("Write",boundary,e.what(),warn);
                return;
            } catch (...) {
                if (log) throw;
                anchorFailure("Write",boundary,"unknown exception");
                return;
            }
            if (o.afterAnchorForTest) o.afterAnchorForTest(boundary);
            if (o.restoreAtAnchorForTest && o.restoreAtAnchorForTest(boundary)) restoreInProcess(exported,boundary);
        };

        const auto fence = [&] {
            if (!recorder) return;
            const auto drainStart = std::chrono::steady_clock::now();
            recorder->drain();
            if (log && o.levelTiming) o.levelTiming("worker-drain",
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-drainStart).count());
            if (level) level->throwIfFailed();
            const auto stats = recorder->stats();
            if (stats.diskErrors) throw std::runtime_error("roller recorder failed; checkpoint not advanced");
            if (log && checkpointReplayed && checkpoint && !recoveryComplete && receiveThrough >= targetBoundary) {
                point("recovery-replayed"); recoveryComplete = true;
            }
            if (log && !checkpointReplayed) return;
            if (!o.productWriterLease && stats.columnsWritten == checkpointColumns) return;
            auto through = end;
            for (const auto* layer : {"near","deep"})
                through = std::min(through,recorder->watermarks(o.product,layer).minuteThroughMs);
            if (through < savedThrough ||
                (through == savedThrough && stats.columnsWritten == checkpointColumns)) return;
            cp["days"][key] = {{"pos",applied},{"committedThroughMs",through},{"configHash",hash},{"fromMs",from},{"receiveThroughMs",receiveThrough}};
            // Also expose the newest checkpoint in the plan's flat schema.
            cp["pos"] = applied; cp["committedThroughMs"] = through; cp["configHash"] = hash;
            if (log) {
                const auto recorderBytes = recorder->exportState();
                cp["days"][key]["recorder"] = recorderBytes;
                cp["days"][key]["level"] = levelFence(*level,*log,recorder->checkpointClock(o.product),recorderBytes,o.levelTiming,"checkpoint-sync");
                point("checkpoint-synced");
            }
            writeCheckpoint(cpPath,cp,[&] { point("checkpoint-mid-write"); });
            savedThrough = through; checkpointColumns = stats.columnsWritten;
            if (o.onCommitted) o.onCommitted(through);
        };
        const auto needsCReplay = [&] {
            return log && (!checkpointReplayed || !recorder ||
                recorder->watermarks(o.product,"near").minuteThroughMs < end ||
                recorder->watermarks(o.product,"deep").minuteThroughMs < end);
        };
        while ((!recorder || needsCReplay() || o.nextRecord || savedThrough < end) && (!o.cancelled || !o.cancelled()) &&
               (o.nextRecord ? o.nextRecord(*reader, input) : reader->next(input))) {
            // No synthetic EOF tick: only actual journal records prove elapsed time.
            // Warmup is read in journal order; receipt clock need not be monotonic.
            if (recorder && nextBoundary == day && input.record.time.systemNs / 1'000'000 >= day) {
                // Midnight is the state before today's first record. Its exact
                // inclusive position may be in yesterday, but resumeAt lives in
                // today's file and is replayed once. This also handles a long
                // silent interval across midnight without losing its minutes.
                saveAnchor(day,&input); nextBoundary += Quarter;
            }
            feed.apply(input); applied = input.pos; ++records;
            const bool reachedCheckpoint = log && checkpoint && applied == *checkpoint;
            if (reachedCheckpoint) checkpointReplayed = true;
            receiveThrough = std::max(receiveThrough,input.record.time.systemNs / 1'000'000);
            if (o.onApplied) o.onApplied(input);
            if (input.record.kind == capture::Kind::Frame) bytes += input.record.payload.size();
            if (recorder && input.record.time.systemNs / 1'000'000 >= nextBoundary) {
                // First durable record at/after the UTC fence. No host-clock tick
                // is invented, and the position identifies precisely this state.
                while (nextBoundary < end && input.record.time.systemNs / 1'000'000 >= nextBoundary) {
                    saveAnchor(nextBoundary); nextBoundary += Quarter;
                }
            }
            if (recorder && o.restoreAfterRecordForTest && o.restoreAfterRecordForTest(input)) {
                const auto bytes = recorder->exportState();
                if (!bytes.empty()) restoreInProcess(bytes,-1);
            }
            if (o.afterRecordForTest) o.afterRecordForTest(records);
            const auto minute = input.record.time.systemNs / 1'000'000 / Minute;
            const auto second = input.record.time.systemNs / 1'000'000 / 1000;
            // The lateness tail normally closes seconds after the range. Waiting
            // for the next minute fence adds an unnecessary minute to restart.
            const bool tailFence = second >= end / 1000 && second != lastTailFence;
            if (minute != lastFence || tailFence || reachedCheckpoint) {
                fence(); lastFence = minute;
                if (tailFence) lastTailFence = second;
            }
            // Receive time can lead the recorder's envelope-based integration
            // clock. Only a drained, committed watermark proves range completion.
            if (recorder && savedThrough >= end && !needsCReplay()) break;
            // Dry runs retain their receive-time scan bound, without claiming commits.
            if (o.dryRun && input.record.time.systemNs / 1'000'000 >= end + latenessMs) break;
        }
        if (log && !checkpointReplayed)
            throw std::runtime_error("C checkpoint journal position was not replayed");
        fence();
        if (o.allowGrowingEnd && savedThrough >= end && end < day + Day)
            saveAnchor(end);
        if (recorder) columns += recorder->stats().columnsWritten - importedColumns;
        dayReport["committedThroughMs"] = savedThrough;
        decoded += reader->decodedRecords();
        if (o.writeAnchors && !o.dryRun) {
            try {
                const auto error = AnchorStore::prune(anchorRoot,o.product,receiveThrough);
                if (error) throw std::system_error(error,"anchor prune");
            } catch (const std::exception& e) { anchorFailure("Prune",receiveThrough,e.what()); }
            catch (...) { anchorFailure("Prune",receiveThrough,"unknown exception"); }
        }
        days.push_back(std::move(dayReport)); pending = pending || reader->pending();

    }
    decoded += inventory.decodedRecords();
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    return {{"product",o.product},{"dryRun",o.dryRun},{"records",records},{"jsonBytes",bytes},{"decodedRecords",decoded},
            {"wallSeconds",seconds},{"recordsPerSecond",records/seconds},{"jsonMBPerSecond",bytes/seconds/1e6},
            {"columnsWritten",columns},{"outputBytes",o.dryRun ? 0 : outputBytes(o.outputRoot,o.product)},
            {"pendingTail",pending},{"days",days}};
}
} // namespace sentinel::roller
