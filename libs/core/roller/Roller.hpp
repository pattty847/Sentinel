#pragma once
#include "Grid.hpp"
#include "JournalFeed.hpp"

namespace sentinel::roller {
struct RollOptions {
    std::filesystem::path journalRoot, outputRoot;
    std::string product;
    int64_t fromMs = 0, toMs = 0; // [from,to), complete UTC minutes
    nlohmann::json overrides = nlohmann::json::object();
    bool dryRun = false;
    // Crash/lifecycle seam: invoked after a processed record, before a possible
    // checkpoint fence. Throw to interrupt; the next run must recover identically.
    std::function<void(uint64_t)> afterRecordForTest;
    // Shadow-only source: returns durable journal records, including warmup.
    // A false return is cancellation/EOF, never a synthetic tick.
    std::function<bool(JournalReader&, JournalRecord&)> nextRecord;
    std::function<void(int64_t)> onCommitted;
    std::function<bool()> cancelled;
    bool productWriterLease = false;
    std::function<void(const std::string&)> onInvalid;
    std::function<void(const JournalRecord&)> onApplied;
    // History publication: committed minutes only (Publication::Finals). Not
    // part of the checkpoint policy hash.
    std::function<void(std::shared_ptr<const recording::Hmc2Record>)> publisher;
    // Live-lead seam, on the calling thread: the day's recorder and journal feed
    // once both exist, then (nullptr, nullptr, 0) before either is destroyed.
    std::function<void(recording::BookRecorder*, const JournalFeed*, int64_t dayEndMs)> onRecorder;
    // Model-tap seam (recording.live_feed: journal), on the calling thread: the
    // day's feed after roll() installed its callbacks, before the first record.
    // The tap may chain them (observing levels before the recorder takes them).
    // The feed lives until onRecorder(nullptr, nullptr, 0). Not policy.
    std::function<void(JournalFeed&)> onFeed;
    bool useAnchors = true, writeAnchors = true;
    std::filesystem::path anchorRoot; // empty means outputRoot; rebuild uses scratch HMC2
    // LiveSource validates against its handshake ceiling before any installation.
    std::function<bool(const JournalPos&)> anchorAllowed;
    std::function<void(const JournalPos&, const JournalBook&, bool, bool cursorApplied, int64_t receiveMs)> onAnchorRestore;
    // Reuse the model tap's durable book; batch replay maintains its own otherwise.
    std::function<std::pair<JournalBook,bool>()> anchorBook;
    std::function<void(int64_t)> afterAnchorForTest;
    bool anchorFailuresFatal = false; // rebuild-anchors: sidecars are the requested product
    // Boundary/error-path injection only; never used on the primary record hot path.
    std::function<std::vector<uint8_t>(recording::BookRecorder&)> anchorExportForTest;
    std::function<bool(JournalReader&, JournalRecord&)> anchorOverlapReadForTest;

};
void registerAnchorMetrics(metrics::MetricsRegistry&, const std::string& product);
// Atomically replace and fsync a checkpoint in an already durable parent.
void writeCheckpoint(const std::filesystem::path&, const nlohmann::json&);
void validateOutputProduct(const std::filesystem::path& root, const std::string& product);
nlohmann::json roll(const RollOptions& options);
nlohmann::json diff(const std::filesystem::path& a, const std::filesystem::path& b,
                    const std::string& product, const std::string& layer, int64_t from, int64_t to,
                    int64_t tfMs = 60'000, bool strictJournal = false);
int64_t parseTime(const std::string& text);
int rollMain(int argc, char** argv);
int diffMain(int argc, char** argv);
} // namespace sentinel::roller
