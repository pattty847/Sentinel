#include "FlowCodec.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace sentinel::flow {
namespace {
struct Writer {
    Bytes b;
    void u(uint64_t v, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void vu(uint64_t v) {
        while (v >= 128) { b.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
        b.push_back(static_cast<uint8_t>(v));
    }
    void d(double v) { u(std::bit_cast<uint64_t>(v), 8); }
    void str(const std::string& s) {
        if (s.size() > UINT16_MAX) throw std::length_error("flow string too long");
        u(s.size(), 2); b.insert(b.end(), s.begin(), s.end());
    }
    void magic(const char* s) { b.insert(b.end(), s, s + 4); }
};
struct Reader {
    std::span<const uint8_t> b;
    size_t p = 0;
    uint64_t u(size_t n) {
        if (n > b.size() - p) throw std::runtime_error("truncated flow frame");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(b[p++]) << (8 * i);
        return v;
    }
    uint64_t vu() {
        uint64_t v = 0;
        for (unsigned i = 0; i < 10; ++i) {
            const auto byte = u(1);
            if (i == 9 && byte > 1) throw std::runtime_error("flow varint overflow");
            v |= (byte & 0x7f) << (7 * i);
            if (!(byte & 0x80)) {
                if (i && byte == 0) throw std::runtime_error("noncanonical flow varint");
                return v;
            }
        }
        throw std::runtime_error("flow varint overflow");
    }
    double d() { return std::bit_cast<double>(u(8)); }
    std::string str() {
        const auto n = u(2);
        if (n > b.size() - p) throw std::runtime_error("truncated flow string");
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n; return s;
    }
    void magic(const char* s) { for (size_t i = 0; i < 4; ++i) if (u(1) != uint8_t(s[i])) throw std::runtime_error("flow magic mismatch"); }
    void done() const { if (p != b.size()) throw std::runtime_error("flow trailing bytes"); }
};
void validate(const FlowBar& b) {
    if (b.timeframeMs <= 0 || !std::isfinite(b.nativeTick) || b.nativeTick <= 0 ||
        b.priceScale <= 0 || b.baseScale <= 0 || b.quoteScale <= 0 ||
        b.rows.size() > UINT16_MAX || b.notable.size() > NotableCapacity)
        throw std::invalid_argument("invalid flow bar");
    for (double v : {b.open, b.high, b.low, b.close})
        if (!std::isfinite(v)) throw std::invalid_argument("nonfinite flow price");
    if (b.baseVolume < 0 || b.quoteVolume < 0 || b.buyVol < 0 || b.sellVol < 0 ||
        b.maxRowVol < 0) throw std::invalid_argument("negative flow volume");
    int last = -1;
    for (const auto& r : b.rows) {
        if (r.offset < 0 || r.offset <= last) throw std::invalid_argument("invalid flow row offset");
        if (r.bidVol < 0 || r.askVol < 0) throw std::invalid_argument("negative flow row volume");
        last = r.offset;
    }
    for (size_t i = 0; i < b.notable.size(); ++i) {
        const auto& n = b.notable[i];
        if (!std::isfinite(n.price) || !std::isfinite(n.minPrice) || !std::isfinite(n.maxPrice) ||
            n.size <= 0 || n.notional <= 0 || n.sweepLevels < 1 ||
            n.minPrice > n.price || n.price > n.maxPrice ||
            static_cast<uint8_t>(n.side) > 2) throw std::invalid_argument("invalid flow notable");
        if (i) {
            const auto& prev = b.notable[i - 1];
            if (prev.notional < n.notional ||
                (prev.notional == n.notional && (prev.exchangeMs > n.exchangeMs ||
                (prev.exchangeMs == n.exchangeMs && prev.tradeId > n.tradeId))))
                throw std::invalid_argument("noncanonical flow notable order");
        }
    }
}
}
Bytes encodeFlowBar(const FlowBar& b) {
    validate(b);
    Writer w; w.magic("FLB1");
    w.u(b.startMs, 8); w.u(b.timeframeMs, 8); w.u(b.lowRow, 8); w.d(b.nativeTick);
    w.u(b.priceScale, 8); w.u(b.baseScale, 8); w.u(b.quoteScale, 8);
    for (double v : {b.open, b.high, b.low, b.close}) w.d(v);
    for (int64_t v : {b.baseVolume, b.quoteVolume, b.buyVol, b.sellVol, b.delta, b.maxRowVol}) w.u(v, 8);
    w.u(b.count, 8); w.u(b.revision, 4);
    w.u(b.rows.size(), 2);
    for (const auto& r : b.rows) {
        w.u(static_cast<uint16_t>(r.offset), 2);
        w.vu(r.bidVol); w.vu(r.askVol); w.vu(r.bidCount); w.vu(r.askCount);
    }
    w.u(b.notable.size(), 1);
    for (const auto& n : b.notable) {
        w.str(n.tradeId); w.u(n.exchangeMs, 8); w.d(n.price); w.u(n.size, 8); w.u(n.notional, 8);
        w.d(n.minPrice); w.d(n.maxPrice); w.u(static_cast<uint8_t>(n.side), 1);
        w.u(n.sweepLevels, 2);
    }
    return w.b;
}
FlowBar decodeFlowBar(std::span<const uint8_t> bytes) {
    Reader r{bytes}; r.magic("FLB1");
    FlowBar b;
    b.startMs = static_cast<int64_t>(r.u(8));
    b.timeframeMs = static_cast<int64_t>(r.u(8));
    b.lowRow = static_cast<int64_t>(r.u(8)); b.nativeTick = r.d();
    b.priceScale = static_cast<int64_t>(r.u(8));
    b.baseScale = static_cast<int64_t>(r.u(8));
    b.quoteScale = static_cast<int64_t>(r.u(8));
    b.open = r.d(); b.high = r.d(); b.low = r.d(); b.close = r.d();
    b.baseVolume = static_cast<int64_t>(r.u(8)); b.quoteVolume = static_cast<int64_t>(r.u(8));
    b.buyVol = static_cast<int64_t>(r.u(8)); b.sellVol = static_cast<int64_t>(r.u(8));
    b.delta = static_cast<int64_t>(r.u(8)); b.maxRowVol = static_cast<int64_t>(r.u(8));
    b.count = r.u(8); b.revision = r.u(4);
    const auto rowCount = r.u(2);
    for (size_t i = 0; i < rowCount; ++i) {
        Row row; row.offset = static_cast<int16_t>(r.u(2));
        row.bidVol = static_cast<int64_t>(r.vu()); row.askVol = static_cast<int64_t>(r.vu());
        const auto bids = r.vu(), asks = r.vu();
        if (bids > UINT32_MAX || asks > UINT32_MAX) throw std::runtime_error("flow row count overflow");
        row.bidCount = bids; row.askCount = asks;
        b.rows.push_back(row);
    }
    const auto notableCount = r.u(1);
    for (size_t i = 0; i < notableCount; ++i) {
        Notable n; n.tradeId = r.str(); n.exchangeMs = static_cast<int64_t>(r.u(8));
        n.price = r.d(); n.size = static_cast<int64_t>(r.u(8));
        n.notional = static_cast<int64_t>(r.u(8)); n.minPrice = r.d(); n.maxPrice = r.d();
        const auto side = r.u(1);
        if (side > 2) throw std::runtime_error("invalid flow side");
        n.side = static_cast<Side>(side); n.sweepLevels = r.u(2);
        b.notable.push_back(std::move(n));
    }
    r.done(); validate(b); return b;
}
Bytes encodeFlowDay(const DayFile& day) {
    if (day.dayStartMs % 86'400'000 != 0 || day.timeframeMs <= 0 ||
        day.bars.size() > UINT32_MAX) throw std::invalid_argument("invalid flow day");
    std::vector<Bytes> frames;
    frames.reserve(day.bars.size());
    int64_t previous = day.dayStartMs - 1;
    for (const auto& bar : day.bars) {
        if (bar.startMs <= previous || bar.startMs < day.dayStartMs ||
            bar.startMs >= day.dayStartMs + 86'400'000 ||
            bar.timeframeMs != day.timeframeMs || bar.nativeTick != day.nativeTick ||
            bar.priceScale != day.priceScale || bar.baseScale != day.baseScale ||
            bar.quoteScale != day.quoteScale)
            throw std::invalid_argument("invalid day bar index");
        previous = bar.startMs;
        frames.push_back(encodeFlowBar(bar));
    }
    Writer w; w.magic("FLD1"); w.str(day.product); w.u(day.dayStartMs, 8);
    w.u(day.timeframeMs, 8); w.d(day.nativeTick);
    w.u(day.priceScale, 8); w.u(day.baseScale, 8); w.u(day.quoteScale, 8);
    w.b.insert(w.b.end(), day.sourceFingerprint.begin(), day.sourceFingerprint.end());
    w.u(frames.size(), 4);
    uint64_t offset = w.b.size() + frames.size() * 20;
    for (size_t i = 0; i < frames.size(); ++i) {
        w.u(day.bars[i].startMs, 8); w.u(offset, 8); w.u(frames[i].size(), 4);
        offset += frames[i].size();
    }
    for (const auto& frame : frames) w.b.insert(w.b.end(), frame.begin(), frame.end());
    return w.b;
}
DayFile decodeFlowDay(std::span<const uint8_t> bytes) {
    Reader r{bytes}; r.magic("FLD1");
    DayFile day; day.product = r.str(); day.dayStartMs = static_cast<int64_t>(r.u(8));
    day.timeframeMs = static_cast<int64_t>(r.u(8)); day.nativeTick = r.d();
    day.priceScale = static_cast<int64_t>(r.u(8));
    day.baseScale = static_cast<int64_t>(r.u(8));
    day.quoteScale = static_cast<int64_t>(r.u(8));
    for (auto& byte : day.sourceFingerprint) byte = r.u(1);
    const auto count = r.u(4);
    if (count > (bytes.size() - r.p) / 20) throw std::runtime_error("truncated flow index");
    struct Entry { int64_t start; uint64_t offset; uint32_t length; };
    std::vector<Entry> index;
    for (size_t i = 0; i < count; ++i)
        index.push_back({static_cast<int64_t>(r.u(8)), r.u(8), static_cast<uint32_t>(r.u(4))});
    size_t end = r.p;
    for (const auto& e : index) {
        if (e.offset != end || e.length > bytes.size() - end) throw std::runtime_error("invalid flow index");
        auto bar = decodeFlowBar(bytes.subspan(end, e.length));
        if (bar.startMs != e.start) throw std::runtime_error("flow index key mismatch");
        day.bars.push_back(std::move(bar)); end += e.length;
    }
    if (end != bytes.size()) throw std::runtime_error("flow day trailing bytes");
    // Apply the same day and ordering validation as the writer.
    if (encodeFlowDay(day) != Bytes(bytes.begin(), bytes.end())) throw std::runtime_error("invalid flow day");
    return day;
}
} // namespace sentinel::flow
