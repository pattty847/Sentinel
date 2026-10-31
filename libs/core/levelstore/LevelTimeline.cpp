#include "LevelTimeline.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace sentinel::levelstore {
namespace {
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(std::string("levelstore decode: ") + what);
}
constexpr uint8_t kObserved = 1, kReset = 2;
constexpr unsigned kClearShift = 2;
} // namespace

const char *clearCauseName(ClearCause c) noexcept {
    switch (c) {
    case ClearCause::None: return "none";
    case ClearCause::Invalid: return "invalid";
    case ClearCause::Unsupported: return "unsupported-price";
    case ClearCause::Failed: return "failed";
    case ClearCause::RestoreRefused: return "restore-refused";
    }
    return "?";
}

void ByteWriter::varint(uint64_t v) {
    while (v >= 0x80) {
        bytes.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(v));
}
void ByteWriter::fixed32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::fixed64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::f64(double v) { fixed64(std::bit_cast<uint64_t>(v)); }
void ByteWriter::str(const std::string &s) {
    varint(s.size());
    bytes.insert(bytes.end(), s.begin(), s.end());
}

uint8_t ByteReader::u8() {
    require(pos_ < b_.size(), "truncated");
    return b_[pos_++];
}
uint64_t ByteReader::varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const uint8_t byte = u8();
        require(shift < 63 || (byte & 0xFE) == 0, "overlong varint");
        v |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return v;
    }
    require(false, "overlong varint");
    return 0;
}
uint32_t ByteReader::fixed32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
    return v;
}
uint64_t ByteReader::fixed64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(u8()) << (8 * i);
    return v;
}
double ByteReader::f64() { return std::bit_cast<double>(fixed64()); }
std::string ByteReader::str(size_t maxSize) {
    const uint64_t n = varint();
    require(n <= maxSize && n <= remaining(), "string length");
    std::string s(reinterpret_cast<const char *>(b_.data() + pos_), static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return s;
}

void encodeBlock(ByteWriter &w, const MinuteBlock &b) {
    w.svarint(b.bucketStartMs);
    w.varint(b.recorderObservedMs);
    w.varint(b.recorderFlags);
    w.u8(static_cast<uint8_t>((b.observedAtStart ? 1 : 0) | (b.damaged ? 2 : 0)));
    w.varint(b.messages.size());
    int64_t t = b.bucketStartMs;
    for (const auto &m : b.messages) {
        w.svarint(m.timeMs - t);
        t = m.timeMs;
        w.u8(static_cast<uint8_t>((m.observed ? kObserved : 0) | (m.reset ? kReset : 0) |
                                  (static_cast<uint8_t>(m.clear) << kClearShift)));
    }
    w.varint(b.changes.size());
    uint32_t message = 0;
    int64_t units = 0;
    for (const auto &c : b.changes) {
        w.varint(c.message - message);
        message = c.message;
        w.u8(c.isBid ? 1 : 0);
        w.svarint(c.units - units);
        units = c.units;
        w.f64(c.size);
    }
}
MinuteBlock decodeBlock(ByteReader &r) {
    MinuteBlock b;
    b.bucketStartMs = r.svarint();
    const uint64_t observed = r.varint(), flags = r.varint();
    require(observed <= 60'000 && flags <= UINT32_MAX, "block header");
    b.recorderObservedMs = static_cast<uint32_t>(observed);
    b.recorderFlags = static_cast<uint32_t>(flags);
    const uint8_t bits = r.u8();
    require(bits <= 3, "block flags");
    b.observedAtStart = bits & 1;
    b.damaged = bits & 2;
    const uint64_t n = r.varint();
    require(n <= r.remaining(), "message count");
    b.messages.resize(static_cast<size_t>(n));
    int64_t t = b.bucketStartMs;
    for (auto &m : b.messages) {
        const int64_t d = r.svarint();
        require(d >= 0, "message time order");
        t += d;
        m.timeMs = t;
        const uint8_t f = r.u8();
        const uint8_t clear = f >> kClearShift;
        require(clear <= static_cast<uint8_t>(ClearCause::RestoreRefused), "message flags");
        m.observed = f & kObserved;
        m.reset = f & kReset;
        m.clear = static_cast<ClearCause>(clear);
    }
    const uint64_t k = r.varint();
    require(k <= r.remaining(), "change count");
    b.changes.resize(static_cast<size_t>(k));
    uint32_t message = 0;
    int64_t units = 0;
    for (auto &c : b.changes) {
        const uint64_t d = r.varint();
        require(d < n && message + d < n, "change message");
        message += static_cast<uint32_t>(d);
        c.message = message;
        const uint8_t side = r.u8();
        require(side <= 1, "change side");
        c.isBid = side == 1;
        units += r.svarint();
        require(units > 0, "change units");
        c.units = units;
        c.size = r.f64();
        require(std::isfinite(c.size) && c.size >= 0, "change size");
    }
    return b;
}
void encodeImage(ByteWriter &w, const BookImage &image) {
    w.svarint(image.atMs);
    w.u8(image.observed ? 1 : 0);
    w.varint(image.levels.size());
    int64_t units = 0;
    for (const auto &l : image.levels) {
        w.u8(l.isBid ? 1 : 0);
        w.svarint(l.units - units);
        units = l.units;
        w.f64(l.size);
    }
}
BookImage decodeImage(ByteReader &r) {
    BookImage image;
    image.atMs = r.svarint();
    const uint8_t observed = r.u8();
    require(observed <= 1, "image flags");
    image.observed = observed;
    const uint64_t n = r.varint();
    require(n <= r.remaining(), "image count");
    image.levels.resize(static_cast<size_t>(n));
    int64_t units = 0;
    const BookLevel *previous = nullptr;
    for (auto &l : image.levels) {
        const uint8_t side = r.u8();
        require(side <= 1, "image side");
        l.isBid = side == 1;
        units += r.svarint();
        l.units = units;
        l.size = r.f64();
        require(l.units > 0 && std::isfinite(l.size) && l.size > 0, "image level");
        // Bids first, then asks; strictly ascending units within a side.
        require(!previous || (previous->isBid && !l.isBid) ||
                    (previous->isBid == l.isBid && previous->units < l.units), "image order");
        previous = &l;
    }
    return image;
}

} // namespace sentinel::levelstore
