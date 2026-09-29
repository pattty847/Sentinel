#pragma once
// Research-only encoders. The production recorder deliberately cannot write sub-minute HMC2.
#include "servermodel/Hmc2Store.hpp"
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <span>

namespace storage_probe {
using Bytes = std::vector<uint8_t>;
struct Level { bool bid; double price, size; };
enum class Kind : uint8_t { Snapshot, Delta, Invalid };
struct Event { int64_t timeMs; Kind kind; std::vector<Level> levels; };
struct Stats {
    uint64_t bytes = 0, payloadBytes = 0, rawBytes = 0, blocks = 0;
    uint64_t keyframes = 0, deltas = 0, entries = 0, observedMs = 0;
    uint64_t partialColumns = 0;
};
using BlockSink = std::function<void(const Bytes&)>;
Bytes decompressBlock(const Bytes& frame); // Validates length, zstd frame and CRC.

// Exact binary64 round trip of parsed local-stream levels; XOR/delta varints,
// preserving message boundaries/order. Not original exchange JSON or sequences.
class RawEncoder {
  public:
    explicit RawEncoder(BlockSink sink = {});
    void add(const Event& event);
    void flush();
    const Stats& stats() const { return stats_; }
    static std::vector<Event> decode(const Bytes& frame);
  private:
    Stats stats_;
    Bytes raw_;
    BlockSink sink_;
    int64_t first_ = 0, last_ = 0;
    uint64_t price_ = 0, size_ = 0;
};

// Mirrors HMC2 schema-3 record serialization and frame overhead, not an HMC2
// format extension. At 1m the output is byte-compatible (tested against Hmc2Store).
class ColumnCodec {
  public:
    explicit ColumnCodec(BlockSink sink = {}) : sink_(std::move(sink)) {}
    void add(const recording::Hmc2Record& record);
    const Stats& stats() const { return stats_; }
  private:
    Stats stats_;
    std::optional<recording::Hmc2Record> previous_;
    int64_t day_ = -1;
    BlockSink sink_;
};

// Independent concurrent simulations consume the same stream, interleaved on
// one benchmark thread. Each includes its own book-maintenance CPU cost.
class Columns {
  public:
    using RecordSink = std::function<void(const recording::Hmc2Record&)>;
    explicit Columns(int64_t tfMs, bool peak = true, RecordSink sink = {});
    void add(const Event& event);
    void advance(int64_t timeMs);
    void finish(int64_t timeMs); // Includes final partial column, flagged explicitly.
    const std::array<ColumnCodec, 2>& codecs() const { return codecs_; }
    uint64_t invalidations() const { return invalidations_; }
    uint64_t ignoredDeltas() const { return ignoredDeltas_; }
  private:
    using Key = std::pair<int64_t, bool>;
    struct Row {
        long double size = 0, sum = 0, peak = 0;
        int64_t last = 0;
        int64_t levels = 0;
    };
    std::array<std::map<Key, Row>, 2> rows_;
    std::array<std::map<int64_t, double>, 2> book_;
    std::array<ColumnCodec, 2> codecs_;
    int64_t tf_, clock_ = -1, bucket_ = 0;
    bool peak_, valid_ = false, finished_ = false;
    uint32_t observed_ = 0, flags_ = 0;
    double mid_ = 0, open_ = 0, low_ = 0, high_ = 0;
    uint64_t invalidations_ = 0, ignoredDeltas_ = 0;
    RecordSink sink_;
    void accrue(Row& row);
    void clearBook();
    void trackMid();
    void emitColumn();
};
double threadCpuSeconds();
} // namespace storage_probe
