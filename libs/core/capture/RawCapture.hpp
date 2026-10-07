#pragma once

#include <QFile>
#include <QString>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sentinel::capture {

constexpr uint32_t MaxRecordBytes = 16 * 1024 * 1024;
constexpr uint32_t MaxBlockBytes = MaxRecordBytes + 1024;
constexpr uint32_t MaxIndexEntries = 65536;
constexpr size_t MaxProducts = 32;

enum class Kind : uint32_t {
    Frame = 1, TransportUp, TransportDown, BookInvalidated, ResyncRequested,
    CaptureStarted, CaptureStopped, EngineError, FrameReference
};
struct Stamp {
    int64_t systemNs = 0;
    int64_t steadyNs = 0;
    static Stamp now();
    bool operator==(const Stamp&) const = default;
};
struct Record {
    Kind kind = Kind::Frame;
    Stamp time;
    uint64_t connection = 0;
    // Frame: unmodified WebSocket text. Events: JSON with product/reason.
    std::string payload;
    bool operator==(const Record&) const = default;
};
struct RecordLocation {
    Stamp time;
    uint64_t connection = 0;
    Kind kind = Kind::Frame;
};
enum class JournalEventKind { Record, Durable, Retract };
// Views are valid only during the callback. Record bytes include RAWL2 framing.
// Durable/Retraction use the last record of the successful prefix; hasPosition
// false on a retraction means this run has no durable prefix at all.
struct JournalEvent {
    JournalEventKind kind;
    std::string_view runId;
    uint64_t block = 0;
    uint32_t record = 0;
    bool hasPosition = true;
    std::string_view bytes;
};
using JournalObserver = std::function<void(const JournalEvent&)>;
struct WriterConfig {
    QString root = "/Volumes/T7/sentinel-data/raw-l2";
    std::string symbol = "BTC-USD";
    uint32_t blockBytes = 1024 * 1024;
    std::chrono::milliseconds blockInterval{1000};
    uint32_t fsyncBlocks = 1; // 0 = only on file close
    int compressionLevel = 3;
    JournalObserver onJournal; // append + flush + failure; observer must not throw or block
};
struct BlockIndex {
    uint64_t offset = 0;
    int64_t firstSystemNs = 0;
    int64_t lastSystemNs = 0;
    uint64_t ordinal = 0;
    uint32_t records = 0;
    uint32_t compressedBytes = 0;
    uint32_t rawBytes = 0;
    bool operator==(const BlockIndex&) const = default;
};
struct WriterStats {
    uint64_t records = 0, frames = 0, frameBytes = 0, blocks = 0, fileBytes = 0, files = 0;
};

// Validates the mounted volume and canonical path BEFORE creating any directories.
// In particular an absent /Volumes/T7 must never fall back to the system disk.
QString validateRoot(const QString& root);
QString prepareDirectory(const QString& directory); // validate, create, fsync new directory entries
void validateSymbol(const std::string& symbol);
// Frozen v2 frame identity used for routing and range digests (v2 verification
// and the test-only v2 fixture writer); not an on-disk record.
// Unknown/control/malformed envelopes are broadcast, never discarded.
nlohmann::json frameReceipt(std::string_view payload, const std::vector<std::string>& products);


// Single-thread owner. Append-only, exclusive-create segments; never opens an old
// file for writing. Destructor retracts pending records and closes the fd;
// call close() to commit/index.
// Writes RAWL2 v1 only (one product per file and per connection). RAWL2 v2
// (one connection, several products, routing receipts) is read-only since
// 2026-10-02: metadata naming connection_products/routing is refused.
class Writer {
public:
    Writer(WriterConfig config, nlohmann::json metadata);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    void append(const Record& record);
    void flush();
    void flushDue(int64_t steadyNs);
    void close();
    void sealSegment(); // seal current file; allow append() to open a new segment
    // Used only after an I/O failure: leave the damaged segment untouched and
    // create a fresh segment for the reserved failure marker.
    void abandonSegment();
    void retract() noexcept; // idempotent failure notification (also used by Session fault hooks)
    std::optional<RecordLocation> firstUncommitted() const;
    const WriterStats& stats() const { return m_stats; }
    const QString& currentPath() const { return m_path; }
private:
    // Test-only: tests/capture/legacy_v2_fixture.cpp builds v2 files so the
    // verifier's v2 reader stays covered. No production code defines this.
    friend struct LegacyV2FixtureWriter;
    Writer(WriterConfig config, nlohmann::json metadata, bool legacyV2);
    void open(Stamp time);
    void seal();
    void write(const std::string& bytes);
    void sync();
    WriterConfig m_config;
    nlohmann::json m_metadata;
    QFile m_file;
    QString m_path;
    int64_t m_hour = -1;
    uint64_t m_segment = 0, m_ordinal = 0;
    std::string m_block;
    uint32_t m_count = 0;
    Stamp m_first{}, m_last{};
    bool m_closed = false;
    std::vector<BlockIndex> m_index;
    WriterStats m_stats;
    std::optional<std::pair<uint64_t, uint32_t>> m_durable;
    std::optional<uint64_t> m_appendedBlock;
    bool m_retracted = false;
    std::optional<RecordLocation> m_uncommittedRecord, m_uncommittedFrame;
    struct CompressionState;
    std::unique_ptr<CompressionState> m_compression;
};

struct ScanResult {
    nlohmann::json header;
    std::vector<BlockIndex> index; // always rebuilt by scanning, compared with footer
    bool indexed = false;
    bool tornTail = false;
    bool pendingTail = false;
    uint64_t recordOrdinal = 0;
    uint32_t recordIndex = 0;
    uint64_t fileBytes = 0;
    uint64_t validBytes = 0;
};
// Opt-in journal recovery: validated index entries or bounded CRC-protected
// successor-header scans supply boundaries past damaged framing. Unknown skipped
// framing uses only BlockIndex offset/ordinal; other fields may be zero. Empty
// callback retains strict audit behavior. No files are modified.
using CorruptBlockVisitor = std::function<void(const BlockIndex&, const char* reason)>;
// Incremental, bounded reader used for cross-stream crash-prefix verification.
class RecordReader {
public:
    explicit RecordReader(const QString& path, bool pendingTailAllowed = false,
                          CorruptBlockVisitor onCorruptBlock = {});
    ~RecordReader();
    bool next(Record& record);
    const ScanResult& result() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
using RecordVisitor = std::function<void(const Record&)>;
nlohmann::json readHeader(const QString& path);
// Complete CRC failures/interior corruption throw. Incomplete/unframed terminal data
// is skipped. No file is repaired, truncated or rewritten by the reader.
ScanResult scan(const QString& path, const RecordVisitor& visitor = {});

} // namespace sentinel::capture
