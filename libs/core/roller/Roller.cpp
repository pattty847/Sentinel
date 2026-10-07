#include "Roller.hpp"
#include "AnchorStore.hpp"
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
}
void writeCheckpoint(const fs::path& path, const json& j) {
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
namespace {
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
    uint64_t records = 0, bytes = 0, columns = 0, decoded = 0;
    const auto anchorRoot = o.anchorRoot.empty() ? o.outputRoot : o.anchorRoot;
    json days = json::array();
    bool pending = false;
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
        std::unique_ptr<recording::BookRecorder> recorder;
        // Destroyed first: the live lead forgets this day's recorder and feed.
        struct Detach {
            const RollOptions& o;
            ~Detach() { if (o.onRecorder) o.onRecorder(nullptr, nullptr, 0); }
        } detach{o};
        uint64_t hash = 0, checkpointColumns = 0, importedColumns = 0;
        bool gridReady = false;
        int64_t latenessMs = recording::RecorderConfig{}.latenessMs;
        int64_t savedThrough = from, lastFence = -1, lastTailFence = -1;
        json dayReport = {{"day",QDateTime::fromMSecsSinceEpoch(day,QTimeZone::UTC).toString("yyyy-MM-dd").toStdString()}};
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
            cfg.commitFloorMs = floor; cfg.commitCeilingMs = end;
            // All roll recorders are history writers. With no publisher Finals
            // is behaviorally identical to All, and rebuilds must be portable
            // into the live history writer without relaxing import validation.
            cfg.publication = recording::RecorderConfig::Publication::Finals;
            if (o.publisher) {
                cfg.publisher = [&, publish=o.publisher](auto record) {
                    if (record->bucketStartMs + record->header.tfMs > savedThrough) publish(std::move(record));
                };
            }
            return cfg;
        };
        const auto describeGrid = [&](const Grid& grid) {
            dayReport["grid"] = {{"priceScale",grid.priceScale},{"nearTick",grid.nearTick},
                                  {"deepTick",grid.deepTick},{"sizeFloor",grid.sizeFloor}};
        };

        const auto claimProvenance = [&] {
            if (o.productWriterLease && !fs::exists(cpPath)) {
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
                    recorderFloor = std::max(from,savedThrough);
                }
                cfg = configure(grid,recorderFloor);
                latenessMs = cfg.latenessMs;
                describeGrid(grid);
                gridReady = true;
                if (o.dryRun) return;
                recorder = std::make_unique<recording::BookRecorder>(std::move(cfg));
                if (o.onRecorder) o.onRecorder(recorder.get(), &feed, end);
                claimProvenance();
            }
            if (recorder) recorder->onSnapshotAt(o.product,exchange,local,std::move(levels));
        };
        feed.onUpdates = [&](int64_t t,int64_t local,std::vector<recording::Level> levels) {
            if (!o.anchorBook) book.apply(t,levels);
            if (recorder) recorder->onUpdatesAt(o.product,t,local,std::move(levels));
        };
        feed.onInvalid = [&](int64_t t,const std::string& reason) { if (!o.anchorBook) book.clear(); if (o.onInvalid) o.onInvalid(reason); if (recorder) recorder->onInvalid(o.product,t,reason); };
        feed.onTick = [&](int64_t t) { if (recorder) recorder->onTick(t); };
        feed.onConnection = [&](bool up) { bookUp = up; };
        if (o.onFeed) o.onFeed(feed);
        if (o.useAnchors && !o.dryRun) for (const auto& file : AnchorStore::candidates(anchorRoot,o.product,day)) {
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
                if (a.identity != expected) throw std::runtime_error("anchor policy/range mismatch");
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
                auto candidateReader = std::make_unique<JournalReader>(o.journalRoot,o.product,cursorPos);
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
                while (checkpoint && checked != *checkpoint) {
                    JournalRecord r;
                    const bool present = candidateReader->next(r);
                    candidateDecoded = candidateReader->decodedRecords();
                    if (!present || r.gapBefore || r.pos.run != checkpoint->run ||
                        std::pair(r.pos.block,r.pos.record) > std::pair(checkpoint->block,checkpoint->record)) {
                        rejectedPrefix = true;
                        throw std::runtime_error("anchor committed overlap is unavailable or damaged");
                    }
                    checked = r.pos;
                    retain(std::move(r));
                }
                for (auto it=overlap.rbegin();it!=overlap.rend();++it)
                    candidateReader->putBack(std::move(*it));
                auto candidate = std::make_unique<recording::BookRecorder>(configure(grid,floor));
                candidate->importState(a.recorder.bytes);
                // Everything above is validation on fresh, unpublished objects.
                // Preserve installed callbacks: importing never invokes them.
                feed.importState(state.at("feed"));
                book = std::move(candidateBook); bookUp = candidateUp;
                recorder = std::move(candidate); reader = std::move(candidateReader);
                gridReady = true; hash = expectedHash; recorderFloor = floor;
                referenceMid = a.referenceMid; gridSnapshot = a.gridSnapshot; gridMetadata = a.metadata;
                applied = a.pos; restoreCursor = cursorPos; restoreCursorApplied = !before;
                nextBoundary = a.boundaryMs + Quarter;
                receiveThrough = candidateReceive; restoreLocal = lastLocal;
                if (checkpoint) savedThrough = cp["days"][key].at("committedThroughMs");
                checkpointColumns = recorder->stats().columnsWritten;
                importedColumns = checkpointColumns;
                describeGrid(grid);
                dayReport["anchorBoundaryMs"] = a.boundaryMs;
                dayReport["anchorPos"] = a.pos;
            } catch (const std::exception& e) {
                decoded += candidateDecoded;
                dayReport["anchorRejection"] = e.what();
                if (rejectedPrefix) break; // older candidates span the same bad overlap
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
        const auto saveAnchor = [&](int64_t boundary, const JournalRecord* resume = nullptr) {
            // Snapshot fallback may be warming up below an existing output
            // floor. Such a state cannot replace a standalone midnight anchor
            // or an older cache with a different committed prefix.
            if (!o.writeAnchors || !recorder || boundary < savedThrough ||
                (boundary == day && recorderFloor != from)) return;
            if (o.anchorBook) {
                auto state = o.anchorBook(); book = std::move(state.first); bookUp = state.second;
            }
            ReplayAnchor a;
            a.identity = {o.product,day,from,end,hash}; a.boundaryMs = boundary; a.pos = applied;
            a.gridSnapshot = gridSnapshot; a.referenceMid = referenceMid; a.metadata = gridMetadata;
            a.feed.bytes = json::to_cbor({{"feed",feed.exportState()},{"book",book.exportState()},
                {"up",bookUp},{"recorderFloor",recorderFloor},{"receiveThroughMs",receiveThrough},
                {"resumeAt",resume ? json(resume->pos) : json()},{"resumeGap",resume && resume->gapBefore}});
            a.recorder.bytes = recorder->exportState();
            AnchorStore::write(anchorRoot,a);
            dayReport["anchorBytes"] = fs::file_size(AnchorStore::path(anchorRoot,a));
            if (o.afterAnchorForTest) o.afterAnchorForTest(boundary);
        };

        const auto fence = [&] {
            if (!recorder) return;
            recorder->drain();
            const auto stats = recorder->stats();
            if (stats.diskErrors || stats.queueDrops) throw std::runtime_error("roller recorder failed; checkpoint not advanced");
            if (!o.productWriterLease && stats.columnsWritten == checkpointColumns) return;
            auto through = end;
            for (const auto* layer : {"near","deep"})
                through = std::min(through,recorder->watermarks(o.product,layer).minuteThroughMs);
            if (through < savedThrough ||
                (through == savedThrough && stats.columnsWritten == checkpointColumns)) return;
            cp["days"][key] = {{"pos",applied},{"committedThroughMs",through},{"configHash",hash},{"fromMs",from},{"receiveThroughMs",receiveThrough}};
            // Also expose the newest checkpoint in the plan's flat schema.
            cp["pos"] = applied; cp["committedThroughMs"] = through; cp["configHash"] = hash;
            writeCheckpoint(cpPath,cp);
            savedThrough = through; checkpointColumns = stats.columnsWritten;
            if (o.onCommitted) o.onCommitted(through);
        };
        while ((!recorder || o.nextRecord || savedThrough < end) && (!o.cancelled || !o.cancelled()) &&
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
            if (o.afterRecordForTest) o.afterRecordForTest(records);
            const auto minute = input.record.time.systemNs / 1'000'000 / Minute;
            const auto second = input.record.time.systemNs / 1'000'000 / 1000;
            // The lateness tail normally closes seconds after the range. Waiting
            // for the next minute fence adds an unnecessary minute to restart.
            const bool tailFence = second >= end / 1000 && second != lastTailFence;
            if (minute != lastFence || tailFence) {
                fence(); lastFence = minute;
                if (tailFence) lastTailFence = second;
            }
            // Receive time can lead the recorder's envelope-based integration
            // clock. Only a drained, committed watermark proves range completion.
            if (recorder && savedThrough >= end) break;
            // Dry runs retain their receive-time scan bound, without claiming commits.
            if (o.dryRun && input.record.time.systemNs / 1'000'000 >= end + latenessMs) break;
        }
        fence();
        if (recorder) columns += recorder->stats().columnsWritten - importedColumns;
        dayReport["committedThroughMs"] = savedThrough;
        decoded += reader->decodedRecords();
        days.push_back(std::move(dayReport)); pending = pending || reader->pending();
        if (o.writeAnchors && !o.dryRun) AnchorStore::prune(anchorRoot,o.product,receiveThrough);

    }
    decoded += inventory.decodedRecords();
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    return {{"product",o.product},{"dryRun",o.dryRun},{"records",records},{"jsonBytes",bytes},{"decodedRecords",decoded},
            {"wallSeconds",seconds},{"recordsPerSecond",records/seconds},{"jsonMBPerSecond",bytes/seconds/1e6},
            {"columnsWritten",columns},{"outputBytes",o.dryRun ? 0 : outputBytes(o.outputRoot,o.product)},
            {"pendingTail",pending},{"days",days}};
}
} // namespace sentinel::roller
