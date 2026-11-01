/*
Sentinel — levelstore (Architecture C), LevelTimeline
Role: the exact native-level timeline C records from BookRecorder decisions:
per minute, the recorder messages that changed C's state (each its own ordinal,
so several messages in one millisecond stay distinct states) and the per-level
size changes they made. Plus a canonical byte encoding used by the open-state
blob (and later the durable minute log).
Threading: plain data and pure functions.
*/
#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sentinel::levelstore {

// Why every level ended at a message (the reader drops its whole book there).
enum class ClearCause : uint8_t {
    None = 0,
    Invalid = 1,        // the recorder invalidated the book (it cleared its own)
    Unsupported = 2,    // a price C cannot represent exactly (interim fail-closed policy)
    Failed = 3,         // C failed internally (allocation, output, invariant)
    RestoreRefused = 4, // C's checkpoint state was refused on import
};
const char *clearCauseName(ClearCause c) noexcept;

struct TimelineMessage {
    int64_t timeMs = 0;              // the recorder's integration clock at the message
    bool observed = false;           // C's observation after the message
    bool reset = false;              // a snapshot the recorder accepted as a valid book
    ClearCause clear = ClearCause::None; // != None: every level ended here, before `changes`
    bool operator==(const TimelineMessage &) const = default;
};
struct TimelineChange {
    uint32_t message = 0; // index into MinuteBlock::messages
    bool isBid = false;
    int64_t units = 0;    // exact price units at 10^priceDigits
    double size = 0;      // the size the recorder applied; 0 ends the level
    bool operator==(const TimelineChange &) const = default;
};
// One finalised minute [bucketStartMs, +60 s): emitted when the recorder finishes it.
struct MinuteBlock {
    int64_t bucketStartMs = 0;
    uint32_t recorderObservedMs = 0; // the recorder's own covered time (onMinute)
    uint32_t recorderFlags = 0;      // the recorder's minute flags (onMinute)
    bool observedAtStart = false;    // C's observation at bucketStartMs
    bool damaged = false;            // C lost part of this minute (a failure or a refused restore):
                                     // its changes and clears still apply, but it is read as unobserved
    std::vector<TimelineMessage> messages; // ascending timeMs (equal times are distinct states)
    std::vector<TimelineChange> changes;   // ascending message; one change per level per message
    bool operator==(const MinuteBlock &) const = default;
};
struct BookLevel {
    bool isBid = false;
    int64_t units = 0;
    double size = 0;
    bool operator==(const BookLevel &) const = default;
};
// C's book at the day floor: the state the first minute block starts from.
struct BookImage {
    int64_t atMs = 0;
    bool observed = false;
    std::vector<BookLevel> levels; // bids ascending units, then asks ascending units
    bool operator==(const BookImage &) const = default;
};

// a + b, or std::runtime_error(what) when the sum leaves int64. Portable: checked
// before the addition (MSVC has no __builtin_add_overflow; cf. DomGrouping mulOverflows).
inline int64_t checkedAdd(int64_t a, int64_t b, const char *what) {
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) || (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
        throw std::runtime_error(what);
    return a + b;
}

// Little-endian byte writer/reader with varints. The reader throws std::runtime_error
// on truncated or malformed input; callers that must not throw catch it.
class ByteWriter {
  public:
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void varint(uint64_t v);
    void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void fixed32(uint32_t v);
    void fixed64(uint64_t v);
    void f64(double v);
    void str(const std::string &s);
};
class ByteReader {
  public:
    explicit ByteReader(std::span<const uint8_t> b) : b_(b) {}
    uint8_t u8();
    uint64_t varint();
    int64_t svarint() {
        const uint64_t v = varint();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1u);
    }
    uint32_t fixed32();
    uint64_t fixed64();
    double f64();
    std::string str(size_t maxSize);
    bool done() const { return pos_ == b_.size(); }
    size_t remaining() const { return b_.size() - pos_; }

  private:
    std::span<const uint8_t> b_;
    size_t pos_ = 0;
};

// Canonical encodings. decode* validates structure (ordering, finiteness, sizes) and
// throws std::runtime_error on any violation.
void encodeBlock(ByteWriter &w, const MinuteBlock &b);
MinuteBlock decodeBlock(ByteReader &r);
void encodeImage(ByteWriter &w, const BookImage &image);
BookImage decodeImage(ByteReader &r);

} // namespace sentinel::levelstore
