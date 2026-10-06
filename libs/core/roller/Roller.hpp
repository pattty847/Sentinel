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
};
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
