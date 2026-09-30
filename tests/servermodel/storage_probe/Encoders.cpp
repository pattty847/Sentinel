#include "Encoders.hpp"
#include "servermodel/HmcolFormat.hpp"
#include <zstd.h>
#include <bit>
#include <cstring>
#include <numeric>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <time.h>
#endif

namespace storage_probe {
using namespace recording;
namespace {
constexpr int64_t dayMs = 86'400'000, keyMs = 900'000;
constexpr size_t maxBlock = 16 * 1024 * 1024;
template <class T> void put(Bytes& b, T v) {
    auto a = std::bit_cast<std::array<uint8_t, sizeof(T)>>(v);
    if constexpr (std::endian::native == std::endian::big) std::reverse(a.begin(), a.end());
    b.insert(b.end(), a.begin(), a.end());
}
uint32_t u32(const Bytes& b, size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("short frame");
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v |= uint32_t(b[p + i]) << (8 * i);
    return v;
}
Bytes compress(const Bytes& raw, Stats& stats, uint32_t magic) {
    if (raw.empty() || raw.size() > maxBlock) throw std::runtime_error("probe block exceeds 16 MiB");
    Bytes packed(ZSTD_compressBound(raw.size()));
    auto n = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(n)) throw std::runtime_error(ZSTD_getErrorName(n));
    packed.resize(n);
    Bytes frame;
    put(frame, magic);
    put(frame, uint32_t(n));
    put(frame, uint32_t(raw.size()));
    put(frame, hmcol::crc32(packed.data(), packed.size()));
    frame.insert(frame.end(), packed.begin(), packed.end());
    stats.bytes += frame.size();
    stats.payloadBytes += n;
    stats.rawBytes += raw.size();
    ++stats.blocks;
    return frame;
}
auto key(const Hmc2Entry& e) { return std::pair(e.row, e.isAsk); }
}
Bytes decompressBlock(const Bytes& frame) {
    const auto n = u32(frame, 4), rawSize = u32(frame, 8);
    if (frame.size() != 16ULL + n || !rawSize || rawSize > maxBlock ||
        hmcol::crc32(frame.data() + 16, n) != u32(frame, 12))
        throw std::runtime_error("invalid block length/CRC");
    Bytes raw(rawSize);
    const auto got = ZSTD_decompress(raw.data(), raw.size(), frame.data() + 16, n);
    if (ZSTD_isError(got) || got != raw.size() || ZSTD_findFrameCompressedSize(frame.data() + 16, n) != n)
        throw std::runtime_error("invalid zstd frame");
    return raw;
}
RawEncoder::RawEncoder(BlockSink sink) : sink_(std::move(sink)) {
    // Versioned stream header budget: magic/version/flags, clock origin, symbol,
    // binary64/clock units. No filesystem/index/fsync overhead is simulated.
    stats_.bytes = 32;
}
void RawEncoder::add(const Event& e) {
    if (e.timeMs < 0 || (stats_.entries && e.timeMs < last_)) throw std::runtime_error("raw clock reversed");
    if (e.levels.size() > 500'000) throw std::runtime_error("raw message exceeds level cap");
    if (!raw_.empty() && (e.timeMs - first_ >= 1000 || raw_.size() >= 1024 * 1024)) flush();
    if (raw_.empty()) { first_ = e.timeMs; last_ = 0; price_ = size_ = 0; }
    raw_.push_back(static_cast<uint8_t>(e.kind));
    putVarint(raw_, uint64_t(e.timeMs - last_));
    putVarint(raw_, e.levels.size());
    for (const auto& l : e.levels) {
        raw_.push_back(l.bid ? 1 : 0);
        const auto p = std::bit_cast<uint64_t>(l.price), s = std::bit_cast<uint64_t>(l.size);
        putVarint(raw_, p ^ price_);
        putVarint(raw_, s ^ size_);
        price_ = p; size_ = s;
    }
    last_ = e.timeMs;
    ++stats_.entries; // messages, including snapshots/invalidation markers
}
void RawEncoder::flush() {
    if (raw_.empty()) return;
    auto frame = compress(raw_, stats_, 0x31575053); // SPW1 research block
    if (sink_) sink_(frame);
    raw_.clear();
}
std::vector<Event> RawEncoder::decode(const Bytes& frame) {
    if (u32(frame, 0) != 0x31575053) throw std::runtime_error("raw magic");
    const auto raw = decompressBlock(frame);
    size_t pos = 0;
    auto var = [&] { uint64_t v; if (!getVarint(raw.data(), raw.size(), pos, v))
        throw std::runtime_error("raw varint"); return v; };
    uint64_t p = 0, s = 0, t = 0;
    std::vector<Event> events;
    while (pos < raw.size()) {
        const auto kind = raw[pos++];
        if (kind > 2) throw std::runtime_error("raw kind");
        auto dt = var();
        if (dt > uint64_t(INT64_MAX) - t) throw std::runtime_error("raw timestamp");
        t += dt;
        auto count = var();
        if (count > (raw.size() - pos) / 3) throw std::runtime_error("raw count");
        Event e{int64_t(t), static_cast<Kind>(kind), {}};
        for (uint64_t i = 0; i < count; ++i) {
            const auto side = raw[pos++];
            if (side > 1) throw std::runtime_error("raw side");
            p ^= var(); s ^= var();
            e.levels.push_back({side == 1, std::bit_cast<double>(p), std::bit_cast<double>(s)});
        }
        events.push_back(std::move(e));
    }
    return events;
}
void ColumnCodec::add(const Hmc2Record& r) {
    const auto day = floorDiv(r.bucketStartMs, dayMs);
    if (day != day_) {
        // Exact HMC2 file header length: prefix, two u16 strings, six 8-byte fields.
        stats_.bytes += 14 + 4 + r.header.symbol.size() + r.header.layer.size() + 48;
        day_ = day; previous_.reset();
    }
    const Hmc2Record* base = previous_ && previous_->bucketStartMs + r.header.tfMs == r.bucketStartMs &&
        floorDiv(previous_->bucketStartMs, keyMs) == floorDiv(r.bucketStartMs, keyMs) ? &*previous_ : nullptr;
    if (std::any_of(r.entries.begin(), r.entries.end(), [](const auto& e) { return !e.twapCode; })) base = nullptr;
    Bytes b;
    put(b, r.bucketStartMs); put(b, r.observedMs); put(b, r.flags);
    put(b, r.bidRowLo); put(b, r.bidRowHi); put(b, r.askRowLo); put(b, r.askRowHi);
    put(b, r.midOpen); put(b, r.midClose); put(b, r.midMin); put(b, r.midMax);
    put(b, uint32_t(0)); b.push_back(base ? 1 : 0);
    put(b, base ? base->bucketStartMs : int64_t(0));
    uint32_t count = 0;
    int64_t previousRow = 0;
    auto entry = [&](const Hmc2Entry& e, uint16_t old) {
        putVarint(b, zigzag(e.row - previousRow)); b.push_back(e.isAsk ? 1 : 0);
        putVarint(b, zigzag(int64_t(e.twapCode) - old));
        putVarint(b, zigzag(int64_t(e.peakCode) - e.twapCode));
        previousRow = e.row; ++count;
    };
    size_t old = 0;
    for (const auto& e : r.entries) {
        if (base) while (old < base->entries.size() && key(base->entries[old]) < key(e)) {
            const auto& gone = base->entries[old++]; entry({gone.row, gone.isAsk, 0, 0}, gone.twapCode);
        }
        const auto* prior = base && old < base->entries.size() && key(base->entries[old]) == key(e)
            ? &base->entries[old++] : nullptr;
        if (!prior || e.twapCode != prior->twapCode || e.peakCode != prior->peakCode)
            entry(e, prior ? prior->twapCode : 0);
    }
    if (base) while (old < base->entries.size()) {
        const auto& gone = base->entries[old++]; entry({gone.row, gone.isAsk, 0, 0}, gone.twapCode);
    }
    for (size_t i = 0; i < 4; ++i) b[80 + i] = uint8_t(count >> (8 * i));
    auto frame = compress(b, stats_, kHmc2RecordMagic);
    if (sink_) sink_(frame);
    base ? ++stats_.deltas : ++stats_.keyframes;
    stats_.entries += r.entries.size(); stats_.observedMs += r.observedMs;
    if (r.flags & kPartial) ++stats_.partialColumns;
    previous_ = r;
}
Columns::Columns(int64_t tfMs, bool peak, RecordSink sink) : tf_(tfMs), peak_(peak), sink_(std::move(sink)) {
    if (tfMs != 100 && tfMs != 1000 && tfMs != 10'000 && tfMs != 60'000)
        throw std::runtime_error("unsupported probe resolution");
}
void Columns::accrue(Row& r) {
    if (valid_) r.sum += r.size * (clock_ - r.last);
    r.last = clock_;
}
void Columns::clearBook() {
    for (auto& layer : rows_) for (auto& [key, r] : layer) { accrue(r); r.size = 0; r.levels = 0; }
    for (auto& b : book_) b.clear();
    valid_ = false;
}
void Columns::trackMid() {
    mid_ = std::midpoint(book_[0].rbegin()->first / 100.0, book_[1].begin()->first / 100.0);
    if (!open_) open_ = low_ = high_ = mid_;
    low_ = std::min(low_, mid_); high_ = std::max(high_, mid_);
}
void Columns::emitColumn() {
    for (size_t layer = 0; layer < 2; ++layer) {
        const int64_t tick = layer ? 500 : 100;
        Hmc2Record r;
        r.header = {"BTC-USD", layer ? "deep" : "near", tf_, 100, tick, {}, 0};
        r.bucketStartMs = bucket_; r.observedMs = observed_;
        r.flags = flags_ | (observed_ < tf_ ? kPartial : 0);
        r.midOpen = open_; r.midClose = mid_; r.midMin = low_; r.midMax = high_;
        r.bidRowLo = r.askRowLo = int64_t(std::ceil(static_cast<long double>(low_) * (layer ? .25 : .95) * 100 / tick));
        r.bidRowHi = r.askRowHi = int64_t(std::floor(static_cast<long double>(high_) * (layer ? 4.0 : 1.05) * 100 / tick));
        for (auto it = rows_[layer].begin(); it != rows_[layer].end();) {
            auto& [k, row] = *it;
            accrue(row);
            if (observed_ && k.first >= r.bidRowLo && k.first <= r.bidRowHi &&
                (row.sum > 0 || (peak_ && row.peak > 0))) {
                const double avg = double(row.sum / observed_);
                if ((avg > 0 && avg < r.header.sizeScale.floor) ||
                    (peak_ && row.peak > 0 && row.peak < r.header.sizeScale.floor)) r.flags |= kUnderflow;
                r.entries.push_back({k.first, k.second, encodeSize(avg), peak_ ? encodeSize(double(row.peak)) : uint16_t(0)});
            }
            row.sum = 0; row.peak = row.size;
            if (!row.levels) it = rows_[layer].erase(it); else ++it;
        }
        if (observed_) { codecs_[layer].add(r); if (sink_) sink_(r); }
    }
    observed_ = flags_ = 0; open_ = low_ = high_ = 0;
    if (valid_) trackMid();
}
void Columns::advance(int64_t t) {
    if (finished_) throw std::runtime_error("encoder already finished");
    // Bound price/time domain before any integer conversion or bucket addition.
    if (t < 0 || t > kHmc2EndMs) throw std::runtime_error("probe time outside supported range");
    if (clock_ < 0) { clock_ = t; bucket_ = floorDiv(t, tf_) * tf_; }
    if (t < clock_) throw std::runtime_error("column clock reversed");
    while (bucket_ + tf_ <= t) {
        if (valid_) observed_ += uint32_t(bucket_ + tf_ - clock_);
        clock_ = bucket_ + tf_; emitColumn(); bucket_ = clock_;
        if (!valid_ && bucket_ + tf_ <= t) {
            bucket_ = floorDiv(t, tf_) * tf_; clock_ = bucket_;
            for (auto& layer : rows_) for (auto& [k, r] : layer) r.last = clock_;
        }
    }
    if (valid_) observed_ += uint32_t(t - clock_);
    clock_ = t;
}
void Columns::add(const Event& e) {
    advance(e.timeMs);
    if (e.kind == Kind::Invalid) { clearBook(); ++invalidations_; return; }
    if (e.kind == Kind::Delta && !valid_) { ++ignoredDeltas_; return; }
    for (const auto& l : e.levels) {
        if (!priceUnits(l.price, 100) || l.price > 1e9 || !std::isfinite(l.size) || l.size < 0 || l.size > 1e12) {
            clearBook(); ++invalidations_; return;
        }
    }
    if (e.kind == Kind::Snapshot) { clearBook(); flags_ |= kResynced; }
    std::array<std::vector<Row*>, 2> touched;
    for (const auto& l : e.levels) {
        const auto price = *priceUnits(l.price, 100);
        auto& book = book_[!l.bid];
        const auto prior = book.find(price);
        const auto before = prior == book.end() ? 0.0 : prior->second;
        if (before == l.size) continue;
        if (l.size == 0) book.erase(price); else book[price] = l.size;
        for (size_t layer = 0; layer < 2; ++layer) {
            auto [it, inserted] = rows_[layer].try_emplace({floorDiv(price, layer ? 500 : 100), !l.bid});
            auto& r = it->second;
            if (inserted) r.last = clock_;
            accrue(r);
            r.levels += (l.size > 0) - (before > 0);
            r.size = r.levels ? std::max(0.0L, r.size + static_cast<long double>(l.size) - before) : 0.0L;
            touched[layer].push_back(&r);
        }
    }
    if (book_[0].empty() || book_[1].empty()) { clearBook(); ++invalidations_; return; }
    trackMid();
    for (auto& layer : touched) for (auto* r : layer) r->peak = std::max(r->peak, r->size);
    valid_ = true;
}
void Columns::finish(int64_t t) { advance(t); if (observed_) emitColumn(); finished_ = true; }
double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetThreadTimes failed");
    auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(value(kernel) + value(user)) * 1e-7;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) throw std::runtime_error("thread CPU clock failed");
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}
} // namespace storage_probe
