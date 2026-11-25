#pragma once
// Header identity travels in typed record fields, never in opaque product metadata.
// These in-memory fields are not part of journal positions or checkpoint formats.
// A catalog lives as long as its roller worker or FlowStore symbol and spans every
// file of a product: it keeps only the header fields below, never parsed headers,
// and cursor copies share it.
#include "capture/RawCapture.hpp"
#include "InstrumentSpec.hpp"
#include <filesystem>
#include <deque>
#include <functional>
#include <memory>
#include <stop_token>
#include <stdexcept>

namespace sentinel::metrics { class MetricsRegistry; }

namespace sentinel::roller {
// An absent product history is distinct from an unreadable or corrupt journal.
class NoJournalFiles : public std::runtime_error {
public:
    explicit NoJournalFiles(const std::string& product)
        : std::runtime_error("no journal files for " + product) {}
};
// Register before starting product workers. Counts distinct damaged regions seen
// by this process, including anchor scans; replay does not count them again.
void registerJournalMetrics(metrics::MetricsRegistry&, const std::string& product);
struct JournalPos {
    std::string product, run;
    uint64_t block = 0;
    uint32_t record = 0;
    bool operator==(const JournalPos&) const = default;
};
void to_json(nlohmann::json& j, const JournalPos& p);
void from_json(const nlohmann::json& j, JournalPos& p);
// The header fields readers use. Metadata and instrument are immutable and shared
// by every file of the catalog whose value is byte-identical (normally a run).
struct JournalFile {
    std::filesystem::path path;
    std::string run;           // run_id
    uint64_t segment = 0;
    uint64_t firstBlock = 0;   // first_block_ordinal
    int64_t runStartedNs = 0;  // run_started_system_ns
    int64_t openedNs = 0;      // opened_system_ns
    int formatVersion = 1;
    Venue venue = Venue::Coinbase;
    std::shared_ptr<const nlohmann::json> metadata;   // product_metadata
    std::shared_ptr<const nlohmann::json> instrument; // header instrument, null when absent
    bool superseded = false;
};
struct JournalRecord {
    capture::Record record;
    JournalPos pos;
    nlohmann::json metadata;
    bool gapBefore = false;
    int version = 1;
    Venue venue = Venue::Coinbase; // Also the default for fanout/synthetic records.
    nlohmann::json instrument; // Exact header block, null when absent.
};
// One product, header order (run start, run id, segment), one decoded block at
// a time. Reopen at a saved position to poll an open file; EOF is not a tick.
class JournalReader {
public:
    // The test hook runs after each header read, before the next stop check.
    JournalReader(std::filesystem::path root, std::string product,
                  std::optional<JournalPos> start = {}, std::stop_token stop = {},
                  std::function<void()> afterHeaderForTest = {});
    // A cursor over a previously validated file catalog; does not walk directories.
    JournalReader(const JournalReader& catalog, std::optional<JournalPos> start);
    bool next(JournalRecord& out);
    // Consume the inclusive applied cursor, or retain a proven gap successor
    // for normal feed invalidation. Any unreported mismatch remains fatal.
    void skipApplied(const JournalPos& applied);
    // Return validated records in reverse order to replay without decoding twice.
    void putBack(JournalRecord record);
    const std::vector<JournalFile>& files() const { return *files_; }
    bool pending() const { return pending_; }
    uint64_t decodedRecords() const { return anchorDecoded_ + decoded_ + (reader_ ? reader_->result().decodedRecords : 0); }
    // Observed damage in the scanned interval: corrupt regions, a superseded file
    // with a torn tail or no closing index, a same-run ordinal/segment gap, or a
    // run entered after the start file whose first file is not segment 0.
    // Observational only; recovery does not read it. Anchor scans do not count.
    uint64_t damaged() const { return damaged_; }
    // Latest snapshot <= receive time, including earlier hours/runs for warmup.
    std::optional<JournalPos> anchor(int64_t receiveMs, std::stop_token stop = {},
                                     std::function<void()> afterRecordForTest = {}) const;
private:
    void selectStart();
    std::string product_;
    std::shared_ptr<const std::vector<JournalFile>> files_;
    size_t file_ = 0;
    std::unique_ptr<capture::RecordReader> reader_;
    std::optional<JournalPos> start_;
    std::deque<JournalRecord> buffered_;
    bool gap_ = true, pending_ = false;
    uint64_t nextBlock_ = 0, decoded_ = 0, damaged_ = 0;
    mutable uint64_t anchorDecoded_ = 0;
};
} // namespace sentinel::roller
