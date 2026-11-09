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
        b.rows.size() > UINT16_MAX || b.notable.size() > NotableCapacity)
        throw std::invalid_argument("invalid flow bar");
    int last = -1;
    for (const auto& r : b.rows) {
        if (r.offset < 0 || r.offset <= last) throw std::invalid_argument("invalid flow row offset");
        last = r.offset;
    }
}
}
Bytes encodeFlowBar(const FlowBar& b) {
    validate(b);
    Writer w; w.magic("FLB1");
    w.u(b.startMs, 8); w.u(b.timeframeMs, 8); w.u(b.lowRow, 8); w.d(b.nativeTick);
    for (double v : {b.open, b.high, b.low, b.close, b.baseVolume, b.quoteVolume,
                     b.buyVol, b.sellVol, b.delta, b.maxRowVol}) w.d(v);
    w.u(b.count, 8); w.u(b.revision, 4);
    w.u(b.rows.size(), 2);
    for (const auto& r : b.rows) {
        w.u(static_cast<uint16_t>(r.offset), 2);
        w.d(r.bidVol); w.d(r.askVol); w.u(r.bidCount, 4); w.u(r.askCount, 4);
    }
    w.u(b.notable.size(), 1);
    for (const auto& n : b.notable) {
        w.str(n.tradeId); w.u(n.exchangeMs, 8); w.d(n.price); w.d(n.size); w.d(n.notional);
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
    b.open = r.d(); b.high = r.d(); b.low = r.d(); b.close = r.d();
    b.baseVolume = r.d(); b.quoteVolume = r.d(); b.buyVol = r.d(); b.sellVol = r.d();
    b.delta = r.d(); b.maxRowVol = r.d(); b.count = r.u(8); b.revision = r.u(4);
    const auto rowCount = r.u(2);
    for (size_t i = 0; i < rowCount; ++i) {
        Row row; row.offset = static_cast<int16_t>(r.u(2));
        row.bidVol = r.d(); row.askVol = r.d(); row.bidCount = r.u(4); row.askCount = r.u(4);
        b.rows.push_back(row);
    }
    const auto notableCount = r.u(1);
    for (size_t i = 0; i < notableCount; ++i) {
        Notable n; n.tradeId = r.str(); n.exchangeMs = static_cast<int64_t>(r.u(8));
        n.price = r.d(); n.size = r.d(); n.notional = r.d(); n.minPrice = r.d(); n.maxPrice = r.d();
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
            bar.timeframeMs != day.timeframeMs || bar.nativeTick != day.nativeTick)
            throw std::invalid_argument("invalid day bar index");
        previous = bar.startMs;
        frames.push_back(encodeFlowBar(bar));
    }
    Writer w; w.magic("FLD1"); w.str(day.product); w.u(day.dayStartMs, 8);
    w.u(day.timeframeMs, 8); w.d(day.nativeTick);
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
