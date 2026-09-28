#include "BookRecorder.hpp"
#include "Hmc2Store.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory_resource>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace recording {
namespace {
constexpr int64_t kMinute = 60'000, kHour = 3'600'000;
// Epochs are restricted to chrono's positive calendar range, leaving arithmetic
// headroom for offsets, boundaries and lateness. Market data predates neither 1970
// nor supports years beyond 9999.
constexpr int64_t kMaxTime = 253402300799999LL;
using Key = std::pair<int64_t, bool>;
struct KeyHash {
    size_t operator()(const Key &k) const {
        return std::hash<int64_t>{}(k.first) ^ (k.second ? size_t{0x9e3779b9} : 0);
    }
};
struct Row {
    long double size = 0, integral = 0, peak = 0;
    int64_t last = 0;
    uint64_t touched = 0;
    // Nonzero price levels in this row. When it reaches 0 the size is exactly 0:
    // a running sum of float deltas leaves residues (long double is double on
    // arm64), which would keep phantom rows alive and flag underflow forever.
    uint32_t levels = 0;
};
void accrue(Row &row, int64_t t) {
    if (t > row.last)
        row.integral += row.size * (t - row.last);
    row.last = t;
}
struct Layer {
    Hmc2Header header;
    std::pmr::unordered_map<Key, Row, KeyHash> rows;
    std::vector<Row *> touched;
    std::vector<Hmc2Record> hourMinutes;
    int64_t hour = -1;
    explicit Layer(std::pmr::memory_resource *pool) : rows(pool) {
        rows.reserve(8192);
        touched.reserve(4096);
        hourMinutes.reserve(60);
    }
};
struct Symbol {
    std::pmr::unsynchronized_pool_resource pool;
    std::pmr::map<int64_t, double> bids{&pool}, asks{&pool};
    std::vector<Layer> layers;
    bool initialized = false, valid = false;
    int64_t clock = 0, minute = 0, closedThrough = 0, offset = 0;
    uint32_t observed = 0, flags = 0;
    double mid = 0, midOpen = 0, midMin = 0, midMax = 0, midClose = 0;
    uint64_t serial = 0;
    std::deque<Hmc2Record> pending;
};
int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
uint16_t encode(long double v, const SizeScale &scale, uint32_t &flags) {
    if (v <= 0)
        return 0;
    if (v < scale.floor)
        flags |= kUnderflow;
    return encodeSize(static_cast<double>(std::min(v, static_cast<long double>(std::numeric_limits<double>::max()))),
                      scale);
}
// Window tests use the row's lower price edge. Bounds cover zeros too: they are
// the actual inclusion window, not the min/max of the nonzero entries.
std::pair<int64_t, int64_t> bounds(double loMid, double hiMid, const LayerConfig &layer, double scale) {
    const long double low = static_cast<long double>(loMid) * layer.lowFrac * scale / layer.rowTickUnits;
    const long double high = static_cast<long double>(hiMid) * layer.highMult * scale / layer.rowTickUnits;
    constexpr auto max = std::numeric_limits<int64_t>::max();
    if (low >= 0x1p63L)
        return {1, 0};
    return {static_cast<int64_t>(std::ceil(std::max(0.0L, low))),
            high >= 0x1p63L ? max : static_cast<int64_t>(std::floor(high))};
}
bool covers(const Hmc2Record &r, const Key &k) {
    return k.second ? k.first >= r.askRowLo && k.first <= r.askRowHi : k.first >= r.bidRowLo && k.first <= r.bidRowHi;
}
} // namespace

struct BookRecorder::Impl {
    RecorderConfig cfg;
    std::function<int64_t()> localClock;
    Hmc2Store store;
    enum class Kind { Snapshot, Updates, Invalid, InvalidEnvelope, Tick };
    struct Message {
        Kind kind = Kind::Tick;
        const std::string *symbolName = nullptr;
        std::string reason;
        const std::string &symbol() const {
            static const std::string allSymbols;
            return symbolName ? *symbolName : allSymbols;
        }
        int64_t time = 0, local = 0;
        std::vector<Level> levels;
    };
    // Only the producer accesses this set. Its immutable strings have stable
    // addresses, so even long symbols require no allocation on repeated enqueues.
    std::set<std::string, std::less<>> producerSymbols;
    const std::string *internSymbol(const std::string &name) {
        auto it = producerSymbols.find(name);
        if (it == producerSymbols.end())
            it = producerSymbols.emplace(name).first;
        return &*it;
    }
    static constexpr size_t kQueueSlots = 4096;
    std::array<Message, kQueueSlots> queue;
    std::mutex mutex;
    std::condition_variable wake, drained;
    size_t head = 0, count = 0, queuedLevels = 0;
    bool stopping = false, busy = false, emergency = false;
    int64_t emergencyLocal = 0;
    std::thread worker;
    std::map<std::string, std::unique_ptr<Symbol>> symbols;
    std::atomic<uint64_t> columnsWritten{0}, lateEvents{0}, backwardSteps{0}, queueDrops{0}, invalidations{0},
        diskErrors{0};

    static RecorderConfig validateConfig(RecorderConfig c) {
        if (c.root.empty() || !std::isfinite(c.priceScale) || c.priceScale <= 0 || !std::isfinite(c.sizeScale.floor) ||
            c.sizeScale.floor <= 0 || !std::isfinite(c.sizeScale.codesPerOctave) || c.sizeScale.codesPerOctave <= 0 ||
            c.latenessMs < 0 || c.latenessMs > kHour || c.maxQueuedLevels == 0 || c.layers.empty())
            throw std::invalid_argument("BookRecorder: invalid config");
        std::set<std::string> names;
        for (const auto &l : c.layers) {
            if (l.rowTickUnits <= 0 || !std::isfinite(l.lowFrac) || !std::isfinite(l.highMult) || l.lowFrac <= 0 ||
                l.highMult < l.lowFrac || !names.insert(l.name).second)
                throw std::invalid_argument("BookRecorder: invalid layer");
            Hmc2Header h;
            h.symbol = "validation";
            h.layer = l.name;
            (void)Hmc2Store::filePath(c.root, h, 0);
        }
        return c;
    }
    Impl(RecorderConfig c, std::function<int64_t()> clock)
        : cfg(validateConfig(std::move(c))), localClock(std::move(clock)), store(cfg.root) {
        if (!localClock)
            throw std::invalid_argument("BookRecorder: missing local clock");
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
        sLog_Data("BookRecorder: stopped; open minutes and uncommitted lateness tail dropped");
    }
    void enqueue(Message m) {
        std::lock_guard lock(mutex);
        if (emergency || count == kQueueSlots) {
            ++queueDrops;
            if (!emergency) {
                emergency = true;
                emergencyLocal = m.local;
            }
            wake.notify_one();
            return;
        }
        const bool data = m.kind == Kind::Snapshot || m.kind == Kind::Updates;
        if (data && (m.levels.size() > cfg.maxQueuedLevels - queuedLevels || count >= kQueueSlots - 1)) {
            ++queueDrops;
            m.kind = Kind::InvalidEnvelope;
            m.reason = "queue level/slot overflow";
            std::vector<Level>{}.swap(m.levels);
        }
        queuedLevels += m.levels.size();
        queue[(head + count) % kQueueSlots] = std::move(m);
        ++count;
        wake.notify_one();
    }
    void run() {
        sentinel::logging::setCurrentThreadName("book-recorder");
        for (;;) {
            Message m;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return count || emergency || stopping; });
                if (!count && !emergency && stopping)
                    break;
                busy = true;
                if (count) {
                    m = std::move(queue[head]);
                    head = (head + 1) % kQueueSlots;
                    --count;
                    queuedLevels -= m.levels.size();
                } else {
                    m.kind = Kind::Invalid;
                    m.time = emergencyLocal;
                    m.local = emergencyLocal;
                    m.reason = "queue control overflow";
                    emergency = false;
                }
            }
            try {
                process(m);
            } catch (const std::exception &e) {
                ++diskErrors;
                sLog_Error("BookRecorder: worker error=" << e.what());
                // Preserve numerators but never keep integrating after an unhandled failure.
                for (auto &[name, s] : symbols)
                    invalidate(name, *s, "worker exception");
            }
            {
                std::lock_guard lock(mutex);
                busy = false;
                if (!count && !emergency)
                    drained.notify_all();
            }
        }
    }
    Symbol &getSymbol(const std::string &name) {
        if (const auto it = symbols.find(name); it != symbols.end())
            return *it->second;
        auto next = std::make_unique<Symbol>();
        next->layers.reserve(cfg.layers.size());
        for (const auto &l : cfg.layers) {
            auto &layer = next->layers.emplace_back(&next->pool);
            layer.header = {name, l.name, kMinute, cfg.priceScale, l.rowTickUnits, cfg.sizeScale, 0};
            layer.header.configHash = Hmc2Store::configHash(layer.header, l.lowFrac, l.highMult);
            (void)Hmc2Store::filePath(cfg.root, layer.header, 0);
        }
        return *symbols.emplace(name, std::move(next)).first->second;
    }
    void invalidate(const std::string &name, Symbol &s, const std::string &reason) {
        for (auto &layer : s.layers) {
            for (auto &[_, row] : layer.rows) {
                if (s.valid)
                    accrue(row, s.clock);
                // A rejected resnapshot may have installed provisional levels
                // while validity was already false. Never retain those levels.
                row.size = 0;
                row.levels = 0;
                row.last = s.clock;
            }
        }
        s.valid = false;
        s.bids.clear();
        s.asks.clear();
        ++invalidations;
        sLog_Data("BookRecorder: invalid symbol=" << name << " time=" << s.clock << " reason=" << reason);
    }
    bool trackMid(Symbol &s) {
        s.mid = std::midpoint(static_cast<double>(s.bids.rbegin()->first) / cfg.priceScale,
                              static_cast<double>(s.asks.begin()->first) / cfg.priceScale);
        if (!std::isfinite(s.mid))
            return false;
        if (!s.midMin) {
            s.midOpen = s.midMin = s.midMax = s.mid;
        }
        s.midMin = std::min(s.midMin, s.mid);
        s.midMax = std::max(s.midMax, s.mid);
        s.midClose = s.mid;
        return true;
    }
    void resetMinute(Symbol &s) {
        s.observed = 0;
        s.flags = 0;
        s.midOpen = s.midMin = s.midMax = s.midClose = 0;
        if (s.valid)
            trackMid(s);
    }
    void finishMinute(Symbol &s) {
        for (size_t li = 0; li < s.layers.size(); ++li) {
            auto &layer = s.layers[li];
            Hmc2Record r;
            r.header = layer.header;
            r.bucketStartMs = s.minute;
            r.observedMs = s.observed;
            r.flags = s.flags | (s.observed < kMinute ? kPartial : 0);
            r.midOpen = s.midOpen;
            r.midClose = s.midClose;
            r.midMin = s.midMin;
            r.midMax = s.midMax;
            const auto [lo, hi] = bounds(s.midMin, s.midMax, cfg.layers[li], cfg.priceScale);
            r.bidRowLo = r.askRowLo = lo;
            r.bidRowHi = r.askRowHi = hi;
            r.entries.reserve(layer.rows.size());
            for (auto it = layer.rows.begin(); it != layer.rows.end();) {
                auto &[key, row] = *it;
                accrue(row, s.clock);
                if (r.observedMs && key.first >= lo && key.first <= hi && (row.integral > 0 || row.peak > 0)) {
                    r.entries.push_back({key.first, key.second,
                                         encode(row.integral / r.observedMs, cfg.sizeScale, r.flags),
                                         encode(row.peak, cfg.sizeScale, r.flags)});
                }
                row.integral = 0;
                row.peak = row.size;
                if (row.size == 0)
                    it = layer.rows.erase(it);
                else
                    ++it;
            }
            if (r.observedMs) {
                std::sort(r.entries.begin(), r.entries.end(),
                          [](const auto &a, const auto &b) { return Key{a.row, a.isAsk} < Key{b.row, b.isAsk}; });
                s.pending.push_back(std::move(r));
            }
        }
    }
    void advance(Symbol &s, int64_t target) {
        target = std::max(target, s.clock);
        while (s.minute + kMinute <= target) {
            const auto end = s.minute + kMinute;
            if (s.valid)
                s.observed += static_cast<uint32_t>(end - s.clock);
            s.clock = end;
            finishMinute(s);
            s.minute = end;
            resetMinute(s);
            commit(s);
            // Skip unknown gaps in O(1). Never materialize zero-observation columns.
            if (!s.valid && target >= s.minute + kMinute) {
                s.minute = floorDiv(target, kMinute) * kMinute;
                s.clock = s.minute;
                for (auto &layer : s.layers)
                    for (auto &[_, row] : layer.rows)
                        row.last = s.clock;
            }
        }
        if (s.valid)
            s.observed += static_cast<uint32_t>(target - s.clock);
        s.clock = target;
        commit(s);
    }
    void write(const Hmc2Record &r) {
        store.append(r);
        ++columnsWritten;
        sLog_Probe("recording.close", "symbol=" << r.header.symbol << " layer=" << r.header.layer
                                                << " tf=" << r.header.tfMs << " bucket=" << r.bucketStartMs
                                                << " observed=" << r.observedMs << " entries=" << r.entries.size());
    }
    void rebuildHour(Layer &l, int64_t hour) {
        l.hour = hour;
        l.hourMinutes = Hmc2Store::readRange(cfg.root, l.header.symbol, l.header.layer, kMinute, hour, hour + kHour);
        std::erase_if(l.hourMinutes, [&](const auto &r) { return r.header.configHash != l.header.configHash; });
        sLog_Data("BookRecorder: rebuilt hour symbol=" << l.header.symbol << " layer=" << l.header.layer
                                                       << " hour=" << hour << " minutes=" << l.hourMinutes.size());
    }
    void writeHour(Layer &l) {
        if (l.hourMinutes.empty())
            return;
        Hmc2Record out;
        out.header = l.header;
        out.header.tfMs = kHour;
        // Config hash remains the originating minute policy for safe restart rebuilds.
        out.bucketStartMs = l.hour;
        std::map<Key, std::pair<long double, double>> values;
        bool first = true;
        for (const auto &r : l.hourMinutes) {
            out.observedMs += r.observedMs;
            out.flags |= r.flags;
            if (first) {
                out.midOpen = r.midOpen;
                out.midMin = r.midMin;
                out.midMax = r.midMax;
                out.bidRowLo = r.bidRowLo;
                out.bidRowHi = r.bidRowHi;
                out.askRowLo = r.askRowLo;
                out.askRowHi = r.askRowHi;
                first = false;
            } else {
                out.midMin = std::min(out.midMin, r.midMin);
                out.midMax = std::max(out.midMax, r.midMax);
                out.bidRowLo = std::min(out.bidRowLo, r.bidRowLo);
                out.bidRowHi = std::max(out.bidRowHi, r.bidRowHi);
                out.askRowLo = std::min(out.askRowLo, r.askRowLo);
                out.askRowHi = std::max(out.askRowHi, r.askRowHi);
            }
            out.midClose = r.midClose;
            for (const auto &e : r.entries) {
                auto &v = values[{e.row, e.isAsk}];
                v.first += static_cast<long double>(decodeSize(e.twapCode, r.header.sizeScale)) * r.observedMs;
                v.second = std::max(v.second, decodeSize(e.peakCode, r.header.sizeScale));
            }
        }
        out.flags &= ~kPartial;
        if (out.observedMs < kHour)
            out.flags |= kPartial;
        out.entries.reserve(values.size());
        for (const auto &[key, value] : values) {
            uint32_t coverage = 0;
            for (const auto &r : l.hourMinutes)
                if (covers(r, key))
                    coverage += r.observedMs;
            if (coverage)
                out.entries.push_back({key.first, key.second, encode(value.first / coverage, cfg.sizeScale, out.flags),
                                       encode(value.second, cfg.sizeScale, out.flags)});
        }
        write(out);
        l.hourMinutes.clear();
    }
    void rollup(Symbol &s, const Hmc2Record &r) {
        for (size_t li = 0; li < s.layers.size(); ++li) {
            auto &layer = s.layers[li];
            if (layer.header.layer != r.header.layer || !cfg.layers[li].hourlyRollup)
                continue;
            const auto hour = floorDiv(r.bucketStartMs, kHour) * kHour;
            if (layer.hour != hour) {
                if (layer.hour >= 0)
                    writeHour(layer);
                rebuildHour(layer, hour); // Includes the just-committed minute, with dedup.
            } else {
                auto it = std::lower_bound(layer.hourMinutes.begin(), layer.hourMinutes.end(), r.bucketStartMs,
                                           [](const auto &a, int64_t t) { return a.bucketStartMs < t; });
                if (it != layer.hourMinutes.end() && it->bucketStartMs == r.bucketStartMs)
                    *it = r;
                else
                    layer.hourMinutes.insert(it, r);
            }
        }
    }
    void commit(Symbol &s) {
        while (!s.pending.empty() && s.pending.front().bucketStartMs + kMinute + cfg.latenessMs <= s.clock) {
            auto r = std::move(s.pending.front());
            s.pending.pop_front();
            try {
                write(r);
                rollup(s, r);
            } catch (const std::exception &e) {
                ++diskErrors;
                sLog_Error("BookRecorder: column lost bucket=" << r.bucketStartMs << " error=" << e.what());
            }
        }
        s.closedThrough =
            std::max(s.closedThrough, floorDiv(std::max<int64_t>(0, s.clock - cfg.latenessMs), kMinute) * kMinute);
        for (size_t li = 0; li < s.layers.size(); ++li) {
            auto &l = s.layers[li];
            if (cfg.layers[li].hourlyRollup && l.hour >= 0 && l.hour + kHour + cfg.latenessMs <= s.clock &&
                !l.hourMinutes.empty()) {
                try {
                    writeHour(l);
                } catch (const std::exception &e) {
                    ++diskErrors;
                    sLog_Error("BookRecorder: hourly write failed error=" << e.what());
                    l.hourMinutes.clear();
                }
            }
        }
    }
    void apply(Symbol &s, const Message &m) {
        // Validate the complete message before changing any level. A malformed
        // batch invalidates continuity; skipping only its bad levels invents a book.
        for (const auto &level : m.levels) {
            if (!priceUnits(level.price, cfg.priceScale) || !std::isfinite(level.size) || level.size < 0) {
                sLog_Warning("BookRecorder: malformed L2 symbol=" << m.symbol());
                invalidate(m.symbol(), s, "malformed L2");
                return;
            }
        }
        if (m.kind == Kind::Snapshot) {
            for (auto &layer : s.layers)
                for (auto &[_, row] : layer.rows) {
                    accrue(row, s.clock);
                    row.size = 0;
                    row.levels = 0;
                }
            s.bids.clear();
            s.asks.clear();
            s.flags |= kResynced;
        }
        ++s.serial;
        for (auto &l : s.layers)
            l.touched.clear();
        for (const auto &level : m.levels) {
            const int64_t units = *priceUnits(level.price, cfg.priceScale);
            auto &book = level.isBid ? s.bids : s.asks;
            const auto old = book.find(units);
            const double oldSize = old == book.end() ? 0 : old->second;
            const long double delta = static_cast<long double>(level.size) - oldSize;
            if (delta == 0)
                continue;
            const int levelDelta = (level.size > 0 ? 1 : 0) - (oldSize > 0 ? 1 : 0);
            if (level.size == 0) {
                if (old != book.end())
                    book.erase(old);
            } else if (old != book.end())
                old->second = level.size;
            else
                book.emplace(units, level.size);
            for (auto &l : s.layers) {
                auto [it, added] = l.rows.try_emplace({floorDiv(units, l.header.rowTickUnits), !level.isBid});
                auto &row = it->second;
                if (added)
                    row.last = s.clock;
                if (row.touched != s.serial) {
                    accrue(row, s.clock);
                    row.touched = s.serial;
                    l.touched.push_back(&row);
                }
                row.levels = static_cast<uint32_t>(static_cast<int64_t>(row.levels) + levelDelta);
                row.size = row.levels == 0 ? 0.0L : std::max(0.0L, row.size + delta);
            }
        }
        for (const auto &layer : s.layers) {
            for (const auto *row : layer.touched) {
                if (!std::isfinite(row->size)) {
                    invalidate(m.symbol(), s, "row size overflow");
                    return;
                }
            }
        }
        if (s.bids.empty() || s.asks.empty()) {
            invalidate(m.symbol(), s, "missing two-sided mid");
            return;
        }
        if (!trackMid(s)) {
            invalidate(m.symbol(), s, "unrepresentable mid");
            return;
        }
        for (auto &l : s.layers)
            for (auto *row : l.touched)
                row->peak = std::max(row->peak, row->size);
        if (m.kind == Kind::Snapshot) {
            s.valid = true;
            sLog_Data("BookRecorder: snapshot symbol=" << m.symbol() << " time=" << s.clock
                                                       << " levels=" << m.levels.size());
        }
    }
    void process(const Message &m) {
        if (m.time < 0 || m.time > kMaxTime || m.local < 0 || m.local > kMaxTime) {
            sLog_Warning("BookRecorder: rejected timestamp time=" << m.time);
            for (auto &[name, s] : symbols)
                invalidate(name, *s, "invalid timestamp");
            return;
        }
        if (m.kind == Kind::Tick || m.kind == Kind::Invalid) {
            for (auto &[name, ptr] : symbols) {
                if (m.kind == Kind::Invalid && !m.symbol().empty() && name != m.symbol())
                    continue;
                auto &s = *ptr;
                if (!s.initialized)
                    continue;
                const auto t = std::clamp(m.time - s.offset, int64_t{0}, kMaxTime);
                advance(s, t);
                if (m.kind == Kind::Invalid)
                    invalidate(name, s, m.reason);
            }
            return;
        }
        auto &s = getSymbol(m.symbol());
        if (!s.initialized) {
            if (m.kind != Kind::Snapshot)
                return;
            s.initialized = true;
            s.clock = m.time;
            s.minute = floorDiv(m.time, kMinute) * kMinute;
            s.closedThrough = s.minute;
            for (size_t li = 0; li < s.layers.size(); ++li)
                if (cfg.layers[li].hourlyRollup)
                    rebuildHour(s.layers[li], floorDiv(m.time, kHour) * kHour);
        }
        const bool late = m.time < s.closedThrough;
        if (late)
            ++lateEvents;
        if (m.time < s.clock)
            ++backwardSteps;
        // Use the last actual local-minus-envelope offset, even for a backward
        // envelope; advance() independently clamps the integration clock.
        const int64_t t = std::max(m.time, s.clock);
        s.offset = m.local - m.time;
        advance(s, t);
        if (late)
            s.flags |= kLateEvents;
        if (m.kind == Kind::InvalidEnvelope) {
            sLog_Warning("BookRecorder: queue overflow symbol=" << m.symbol());
            invalidate(m.symbol(), s, m.reason);
            return;
        }
        if (m.kind == Kind::Snapshot || s.valid)
            apply(s, m);
        sLog_Probe("recording.event", "symbol=" << m.symbol() << " envelope=" << m.time << " clock=" << s.clock
                                                << " valid=" << s.valid << " levels=" << m.levels.size());
    }
};
BookRecorder::BookRecorder(RecorderConfig cfg) : BookRecorder(std::move(cfg), systemNow) {}
BookRecorder::BookRecorder(RecorderConfig cfg, std::function<int64_t()> clock)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(clock))) {}
BookRecorder::~BookRecorder() = default;
void BookRecorder::onSnapshot(const std::string &symbol, int64_t time, std::vector<Level> levels) {
    impl_->enqueue(
        {Impl::Kind::Snapshot, impl_->internSymbol(symbol), {}, time, impl_->localClock(), std::move(levels)});
}
void BookRecorder::onUpdates(const std::string &symbol, int64_t time, std::vector<Level> levels) {
    impl_->enqueue(
        {Impl::Kind::Updates, impl_->internSymbol(symbol), {}, time, impl_->localClock(), std::move(levels)});
}
void BookRecorder::onInvalid(const std::string &symbol, int64_t local, std::string reason) {
    impl_->enqueue({Impl::Kind::Invalid, impl_->internSymbol(symbol), std::move(reason), local, local, {}});
}
void BookRecorder::onTick(int64_t local) {
    impl_->enqueue({Impl::Kind::Tick, {}, {}, local, local, {}});
}
BookRecorder::Stats BookRecorder::stats() const {
    const auto &i = *impl_;
    return {i.columnsWritten.load(), i.lateEvents.load(),    i.backwardSteps.load(),
            i.queueDrops.load(),     i.invalidations.load(), i.diskErrors.load()};
}
void BookRecorder::drainForTest() {
    std::unique_lock lock(impl_->mutex);
    impl_->drained.wait(lock, [&] { return !impl_->count && !impl_->emergency && !impl_->busy; });
}
} // namespace recording
