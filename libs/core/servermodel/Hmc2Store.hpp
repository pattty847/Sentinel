#pragma once
#include "RecordingCodec.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <limits>
#include <chrono>
#include <string>
#include <vector>

namespace recording {
inline constexpr uint32_t kHmc2Magic = 0x32434d48;       // bytes HMC2
inline constexpr uint32_t kHmc2RecordMagic = 0x32524348; // bytes HCR2
inline constexpr uint32_t kHmc2MaxRawLen = 16 * 1024 * 1024;
// Supported UTC calendar years: 2000 through 2200 inclusive.
inline constexpr int64_t kHmc2MinMs = 946684800000LL;
inline constexpr int64_t kHmc2EndMs = 7289654400000LL; // 2201-01-01, exclusive

struct Hmc2Header {
    std::string symbol, layer;
    int64_t tfMs = 60'000;
    double priceScale = 100;
    int64_t rowTickUnits = 100;
    SizeScale sizeScale;
    uint64_t configHash = 0; // semantic config, including window policy
};
struct Hmc2Entry {
    int64_t row = 0;
    bool isAsk = false;
    uint16_t twapCode = 0, peakCode = 0; // magnitudes; side separate in memory
    uint32_t coveredMs = 0; // schema 4 hours; legacy/minutes use observedMs
};
struct CoverageRun {
    int64_t lo = 0, hi = 0; // inclusive native rows; sorted/disjoint per side
    bool isAsk = false;
    uint32_t coveredMs = 0;
};
struct Hmc2Record {
    Hmc2Header header;
    int64_t bucketStartMs = 0;
    uint32_t observedMs = 0, flags = 0;
    // Inclusive bounds; lo > hi means no coverage on that side.
    int64_t bidRowLo = 1, bidRowHi = 0, askRowLo = 1, askRowHi = 0;
    double midOpen = 0, midClose = 0, midMin = 0, midMax = 0;
    std::vector<Hmc2Entry> entries;
    std::vector<CoverageRun> coverage; // hours, including covered zero rows
    int64_t committedThroughMs = 0; // publication only: exclusive closed/committed watermark; never serialized
};

using StopToken = std::stop_token;
enum class ReadStatus { Complete, Budget, Cancelled, IoError };
struct ReadLimits {
    uint64_t maxSourceRecords = std::numeric_limits<uint64_t>::max();
    uint64_t maxEntriesVisited = std::numeric_limits<uint64_t>::max();
    uint64_t maxWallMs = std::numeric_limits<uint64_t>::max();
};
// Share one control across availability and all scans in a request. Entry work
// includes inherited delta entries plus changes (a conservative reconstruction bound).
// Index discovery charges source records too, but parses no entries. It shares
// the deadline; cold discovery resumes from its cached cursor on later requests.
// Minimum limits are 1 source record, 1 entry, 10 ms. The first selected record
// (plus its bounded delta chain) is admitted atomically, so counters may exceed
// those soft limits. Cancellation is never deferred.
struct ReadControl {
    ReadLimits limits;
    StopToken stop;
    uint64_t sourceRecords = 0, entriesVisited = 0;
    ReadStatus status = ReadStatus::Complete;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    uint64_t deliveredRecords = 0;
    unsigned deadlineDepth = 0, admissionDepth = 0;
    // A first record + its <=15-record chain is an atomic admission unit.
    // Builders defer only the deadline through first-column projection.
    struct WorkScope {
        ReadControl &control;
        bool admission;
        WorkScope(ReadControl &c, bool admit) : control(c), admission(admit) {
            ++control.deadlineDepth;
            if (admission) ++control.admissionDepth;
        }
        WorkScope(const WorkScope &) = delete;
        WorkScope &operator=(const WorkScope &) = delete;
        ~WorkScope() {
            --control.deadlineDepth;
            if (admission) --control.admissionDepth;
        }
    };
    bool poll();
    bool charge(uint64_t entries);
};
struct ScanResult {
    int64_t scannedStartMs = 0, scannedEndMs = 0; // proven [start,end), empty on no progress
    ReadStatus status = ReadStatus::Complete;
};
struct SeriesAvailability {
    std::optional<int64_t> oldestMs, latestMs;
    std::optional<Hmc2Header> latestHeader;
};
// Worker-owned reusable read session; no writer lock, no GUI dependencies.
// Debug builds assert use/destruction on the constructing thread.
// Caches LRU metadata for 64 files, the active record, and a bounded 16-record
// replay window (262144 entries max); never retains a requested series/range.
// File size/mtime and directory contents invalidate metadata/availability, including
// appends by a writer in another process. Callback references expire on return.
class Hmc2Reader {
  public:
    explicit Hmc2Reader(std::filesystem::path root);
    ~Hmc2Reader();
    Hmc2Reader(const Hmc2Reader &) = delete;
    Hmc2Reader &operator=(const Hmc2Reader &) = delete;
    SeriesAvailability availability(const std::string &symbol, const std::string &layer, int64_t tfMs,
                                    ReadControl &control);
    ScanResult visit(const std::string &symbol, const std::string &layer, int64_t tfMs,
                     int64_t startMs, int64_t endMs, const std::function<void(const Hmc2Record &)> &visitor,
                     ReadControl &control);
    // Deterministic race/I/O seams; one-shot hooks are invoked before refreshing
    // a collected candidate and immediately before its payload read, respectively.
    void beforeCandidateForTest(std::function<void()> hook);
    void beforeReadForTest(std::function<void()> hook);
    struct Diagnostics { uint64_t indexedFrames = 0, directoryListings = 0, indexEvictions = 0; };
    Diagnostics diagnosticsForTest() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Single worker/writer owner. Writes throw on I/O/config errors. Range reads
// warn and skip unreadable files, preserving the rest of the requested history.
// Readers need no writer lock. Concurrent append tails are ignored, never repaired
// by a reader. Only the locked writer repairs incomplete terminal frames.
// Minutes use schema 3; hours use schema 4 with entry/zero-row coverage. Schemas 1/2 remain
// readable as absolute records. Returned entries are always reconstructed.
class Hmc2Store {
  public:
    // deterministicResume is opt-in for journal replay only: validate duplicate
    // buckets and restore the validated last delta base on reopen.
    explicit Hmc2Store(std::filesystem::path root, bool deterministicResume = false);
    ~Hmc2Store();
    Hmc2Store(const Hmc2Store &) = delete;
    Hmc2Store &operator=(const Hmc2Store &) = delete;
    void append(const Hmc2Record &record);
    // One-shot hook after flushing the next record's frame header. Tests can
    // throw to simulate a torn write or rendezvous with a concurrent reader.
    void afterFrameHeaderForTest(std::function<void()> hook);
    // Process-wide: called with each directory before any store fsyncs it, in
    // construction and appends. Tests record the syncs or throw to simulate a
    // failed directory sync; pass {} to clear.
    static void setDirectorySyncHookForTest(std::function<void(const std::filesystem::path &)> hook);
    static std::vector<Hmc2Record> readRange(const std::filesystem::path &root, const std::string &symbol,
                                             const std::string &layer, int64_t tfMs, int64_t startMs, int64_t endMs);
    static std::filesystem::path filePath(const std::filesystem::path &root, const Hmc2Header &header,
                                          int64_t bucketStartMs, uint32_t generation = 0);
    static uint64_t configHash(const Hmc2Header &header, double lowFrac, double highMult);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recording
