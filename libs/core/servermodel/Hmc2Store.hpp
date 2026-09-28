#pragma once
#include "RecordingCodec.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace recording {
inline constexpr uint32_t kHmc2Magic = 0x32434d48;       // bytes HMC2
inline constexpr uint32_t kHmc2RecordMagic = 0x32524348; // bytes HCR2
inline constexpr uint32_t kHmc2MaxRawLen = 16 * 1024 * 1024;
// Supported UTC calendar years: 2000 through 2200 inclusive.
inline constexpr int64_t kHmc2MinMs = 946684800000LL;
inline constexpr int64_t kHmc2EndMs = 7289654400000LL; // 2201-01-01, exclusive
inline constexpr uint32_t kPartial = 1u << 0;
inline constexpr uint32_t kResynced = 1u << 1;
inline constexpr uint32_t kLateEvents = 1u << 2;
inline constexpr uint32_t kUnderflow = 1u << 3;

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
};
struct Hmc2Record {
    Hmc2Header header;
    int64_t bucketStartMs = 0;
    uint32_t observedMs = 0, flags = 0;
    // Inclusive bounds; lo > hi means no coverage on that side.
    int64_t bidRowLo = 1, bidRowHi = 0, askRowLo = 1, askRowHi = 0;
    double midOpen = 0, midClose = 0, midMin = 0, midMax = 0;
    std::vector<Hmc2Entry> entries;
};

// Single worker/writer owner. Writes throw on I/O/config errors. Range reads
// warn and skip unreadable files, preserving the rest of the requested history.
// Readers need no writer lock. Concurrent append tails are ignored, never repaired
// by a reader. Only the locked writer repairs incomplete terminal frames.
class Hmc2Store {
  public:
    explicit Hmc2Store(std::filesystem::path root);
    ~Hmc2Store();
    Hmc2Store(const Hmc2Store &) = delete;
    Hmc2Store &operator=(const Hmc2Store &) = delete;
    void append(const Hmc2Record &record);
    // One-shot hook after flushing the next record's frame header. Tests can
    // throw to simulate a torn write or rendezvous with a concurrent reader.
    void afterFrameHeaderForTest(std::function<void()> hook);
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
