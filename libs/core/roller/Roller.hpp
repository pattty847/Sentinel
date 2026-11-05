#pragma once
#include "Grid.hpp"
#include "JournalFeed.hpp"

namespace sentinel::levelstore { class LevelOutput; class LevelRecorder; class LevelLogFiles; }

namespace sentinel::roller {
struct RollOptions {
    std::filesystem::path journalRoot, outputRoot;
    std::string product;
    int64_t fromMs = 0, toMs = 0; // [from,to), complete UTC minutes
    nlohmann::json overrides = nlohmann::json::object();
    bool dryRun = false;
    // Optional process counters retained across daily recorders and retries.
    sentinel::metrics::Counter* minuteColumnsCounter = nullptr;
    sentinel::metrics::Counter* writeErrorsCounter = nullptr;
    // Crash/lifecycle seam: invoked after a processed record, before a possible
    // checkpoint fence. Throw to interrupt; the next run must recover identically.
    std::function<void(uint64_t)> afterRecordForTest;
    // Shadow-only source: returns durable journal records, including warmup.
    // A false return is cancellation/EOF, never a synthetic tick.
    std::function<bool(JournalReader&, JournalRecord&)> nextRecord;
    std::function<void(int64_t)> onCommitted;
    // A cancelled durable-C day omits its report and pruning: a cut/replayed prefix
    // may not yet match the saved checkpoint. Defer progress reporting and anchor
    // maintenance until a subsequent roll has validated that checkpoint again.
    std::function<bool()> cancelled;
    bool productWriterLease = false;
    std::function<void(const std::string&)> onInvalid;
    std::function<void(const JournalRecord&)> onApplied;
    // History publication: committed minutes only (Publication::Finals). Not
    // part of the checkpoint policy hash.
    std::function<void(std::shared_ptr<const recording::Hmc2Record>)> publisher;
    // History recorder decision feed (RecorderSink); must outlive roll(). Not policy.
    recording::RecorderSink* recorderSink = nullptr;
    // Architecture C (libs/core/levelstore). When set, roll() gives each day's history
    // recorder its own LevelRecorder as its sink (recorderSink is then not used), bound
    // to that day's floor, ceiling and price scale, created before and destroyed after
    // that recorder. Its output must outlive roll(). Internal failure aborts roll.
    // Arbitrary outputs support fresh replay only; durable recovery uses levelLogRoot.
    levelstore::LevelOutput* levelOutput = nullptr;
    // Opt-in durable C output, mutually exclusive with levelOutput. Each roll owns a
    // fresh writer and restores the paired quarter anchor before replay. Calling roll
    // again after failure is the same restart path as a new process; no automatic retry.
    // A durable C call must stay within a single UTC day.
    std::filesystem::path levelLogRoot;
    std::shared_ptr<levelstore::LevelLogFiles> levelLogFiles;
    uint32_t levelRepresentationVersionForTest = 0;
    bool levelSeals = true;
    int64_t levelLogRetentionMs = 172800000;
    uint32_t levelScaleCapForTest = UINT32_MAX;
    // Caller-thread stop seam after explicit checkpoint/anchor writes, syncs and
    // renames (including initial provenance). Throw to unwind at that boundary.
    std::function<void(const char*)> persistencePointForTest;
    // Optional drained-fence timing observer, milliseconds on the calling thread.
    std::function<void(const char*, double)> levelTiming;
    // The day's LevelRecorder once its recorder exists (after onRecorder), and nullptr
    // before it is destroyed. Calling thread.
    std::function<void(levelstore::LevelRecorder*)> onLevelRecorder;
    // In-process checkpoint seam (tests and evidence only). Return true to export the
    // day's recorder and LevelRecorder at this drained position, destroy both and
    // continue with fresh ones imported from those bytes before the next record:
    // after a quarter or midnight anchor export (its boundary), or after any processed
    // record. onInProcessRestore reports each restore (sizes, times, import result).
    std::function<bool(int64_t boundaryMs)> restoreAtAnchorForTest;
    std::function<bool(const JournalRecord&)> restoreAfterRecordForTest;
    std::function<void(const nlohmann::json&)> onInProcessRestoreForTest;
    // Live-lead seam, on the calling thread: the day's recorder and journal feed
    // once both exist, then (nullptr, nullptr, 0) before either is destroyed.
    std::function<void(recording::BookRecorder*, const JournalFeed*, int64_t dayEndMs)> onRecorder;
    // Model-tap seam (Journal live feed), on the calling thread: the
    // day's feed after roll() installed its callbacks, before the first record.
    // The tap may chain them (observing levels before the recorder takes them).
    // The feed lives until onRecorder(nullptr, nullptr, 0). Not policy.
    std::function<void(JournalFeed&)> onFeed;
    bool useAnchors = true, writeAnchors = true;
    bool allowGrowingEnd = false; // hourly oracle keeps day policy and anchors at each completed end
    std::filesystem::path anchorRoot; // empty means outputRoot; rebuild uses scratch HMC2
    // LiveSource validates against its handshake ceiling before any installation.
    std::function<bool(const JournalPos&)> anchorAllowed;
    std::function<void(const JournalPos&, const JournalBook&, bool, bool cursorApplied, int64_t receiveMs)> onAnchorRestore;
    // Reuse the model tap's durable book; batch replay maintains its own otherwise.
    std::function<std::pair<JournalBook,bool>()> anchorBook;
    std::function<void(int64_t)> afterAnchorForTest;
    bool anchorFailuresFatal = false; // rebuild-anchors: sidecars are the requested product
    // Boundary/error-path injection only; never used on the journal record hot path.
    std::function<std::vector<uint8_t>(recording::BookRecorder&)> anchorExportForTest;
    std::function<std::unique_ptr<JournalReader>(const JournalPos&)> anchorReaderForTest;
    std::function<bool(JournalReader&, JournalRecord&)> anchorOverlapReadForTest;

};
void registerAnchorMetrics(metrics::MetricsRegistry&, const std::string& product);
// Atomically replace and fsync a checkpoint in an already durable parent.
void writeCheckpoint(const std::filesystem::path&, const nlohmann::json&, const std::function<void()>& midWrite = {},
        const std::function<void(const char*)>& persistencePoint = {});
void validateOutputProduct(const std::filesystem::path& root, const std::string& product);
nlohmann::json roll(const RollOptions& options);
nlohmann::json diff(const std::filesystem::path& a, const std::filesystem::path& b,
                    const std::string& product, const std::string& layer, int64_t from, int64_t to,
                    int64_t tfMs = 60'000, bool strictJournal = false);
int64_t parseTime(const std::string& text);
int rollMain(int argc, char** argv);
int diffMain(int argc, char** argv);
} // namespace sentinel::roller
