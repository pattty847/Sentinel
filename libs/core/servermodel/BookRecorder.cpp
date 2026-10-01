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
// Reject unit mistakes before they can advance the integration clock.
constexpr int64_t kMaxTime = kHmc2EndMs - 1;
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
    int64_t hourThroughMs = 0;
    bool hourWatermarkBlocked = false;
    int64_t publishedMinuteMs = -1, publishedHourMs = -1, publishedColumnMs = -1; // last values handed to readers
    int64_t lastColumnMs = 0; // newest minute bucket this layer appended successfully
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
    int64_t minuteThroughMs = 0;
    bool minuteWatermarkBlocked = false;
    uint32_t observed = 0, flags = 0;
    int64_t lastPublish = 0;
    double mid = 0, midOpen = 0, midMin = 0, midMax = 0, midClose = 0;
    uint64_t serial = 0;
    std::deque<Hmc2Record> pending;
    // Set while invalid by the recorder's own decision; cleared by an accepted
    // snapshot or an upstream invalidation (whose reconnect brings a snapshot).
    std::string selfInvalidReason;
    int64_t nextResnapshotLocal = 0, resnapshotIntervalMs = 0; // 0 = cfg start interval
    int64_t validSinceLocal = 0;
    int64_t oneSidedSince = -1; // integration clock when a side emptied, -1 = two-sided
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
        bool upstream = false;        // Invalid from onInvalid(), not the recorder's own overflow control
        bool droppedSnapshot = false; // InvalidEnvelope that replaced a snapshot
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
    int64_t workerLocal = 0; // local time of the newest accepted message; worker only
    mutable std::mutex watermarksMutex;
    std::map<std::pair<std::string, std::string>, BookRecorder::Watermarks> watermarksBySeries;
    std::atomic<uint64_t> columnsWritten{0}, lateEvents{0}, backwardSteps{0}, queueDrops{0}, invalidations{0},
        diskErrors{0};

    static RecorderConfig validateConfig(RecorderConfig c) {
        if (c.root.empty() || !std::isfinite(c.priceScale) || c.priceScale <= 0 || !std::isfinite(c.sizeScale.floor) ||
            c.sizeScale.floor <= 0 || !std::isfinite(c.sizeScale.codesPerOctave) || c.sizeScale.codesPerOctave <= 0 ||
            c.latenessMs < 0 || c.latenessMs > kHour || c.maxQueuedLevels == 0 || c.layers.empty() ||
            c.resnapshotIntervalMs <= 0 || c.resnapshotMaxIntervalMs < c.resnapshotIntervalMs ||
            c.resnapshotStableMs < 0 || c.oneSidedGraceMs < 0 || c.livePublishMs <= 0)
            throw std::invalid_argument("BookRecorder: invalid config");
        std::set<std::string> names;
        for (const auto &l : c.layers) {
            if (l.rowTickUnits <= 0 || !std::isfinite(l.lowFrac) || !std::isfinite(l.highMult) || l.lowFrac <= 0 ||
                l.highMult < l.lowFrac || !names.insert(l.name).second)
                throw std::invalid_argument("BookRecorder: invalid layer");
            Hmc2Header h;
            h.symbol = "validation";
            h.layer = l.name;
            (void)Hmc2Store::filePath(c.root, h, kHmc2MinMs);
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
            m.droppedSnapshot = m.kind == Kind::Snapshot;
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
            (void)Hmc2Store::filePath(cfg.root, layer.header, kHmc2MinMs);
        }
        return *symbols.emplace(name, std::move(next)).first->second;
    }
    void requestResnapshot(const std::string &name, Symbol &s) {
        if (!cfg.onSelfInvalidated || s.selfInvalidReason.empty() || workerLocal < s.nextResnapshotLocal)
            return;
        // Exponential: a persistent cause must not reconnect the shared socket every interval.
        const int64_t interval = s.resnapshotIntervalMs ? s.resnapshotIntervalMs : cfg.resnapshotIntervalMs;
        s.nextResnapshotLocal = workerLocal + interval;
        s.resnapshotIntervalMs = std::min(interval * 2, std::max(cfg.resnapshotMaxIntervalMs, interval));
        sLog_Warning("BookRecorder: requesting resnapshot symbol=" << name << " reason=" << s.selfInvalidReason
                                                                   << " time=" << s.clock << " nextInMs=" << interval);
        try {
            cfg.onSelfInvalidated(name, s.selfInvalidReason);
        } catch (const std::exception &e) {
            sLog_Error("BookRecorder: resnapshot request failed symbol=" << name << " error=" << e.what());
        }
    }
    // upstream: reported via onInvalid; the source already owes a snapshot.
    void invalidate(const std::string &name, Symbol &s, const std::string &reason, bool upstream = false) {
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
        s.oneSidedSince = -1;
        s.bids.clear();
        s.asks.clear();
        ++invalidations;
        sLog_Data("BookRecorder: invalid symbol=" << name << " time=" << s.clock << " reason=" << reason);
        if (upstream) {
            s.selfInvalidReason.clear();
        } else {
            s.selfInvalidReason = reason;
            requestResnapshot(name, s);
        }
    }
    // The backoff resets only after a snapshot has stayed valid for resnapshotStableMs.
    void noteStable(Symbol &s) {
        if (s.valid && s.resnapshotIntervalMs && workerLocal - s.validSinceLocal >= cfg.resnapshotStableMs)
            s.resnapshotIntervalMs = s.nextResnapshotLocal = 0;
    }
    // A side that stays empty beyond the grace is a book we can no longer trust to
    // place the window: close observation at the grace end and ask for a snapshot.
    void expireOneSided(const std::string &name, Symbol &s, int64_t target) {
        if (!s.valid || s.oneSidedSince < 0 || target - s.oneSidedSince <= cfg.oneSidedGraceMs)
            return;
        advance(s, s.oneSidedSince + cfg.oneSidedGraceMs);
        invalidate(name, s, "one-sided book");
    }
    bool trackMid(Symbol &s) {
        if (s.bids.empty() || s.asks.empty())
            return false;
        const double mid = std::midpoint(static_cast<double>(s.bids.rbegin()->first) / cfg.priceScale,
                                         static_cast<double>(s.asks.begin()->first) / cfg.priceScale);
        if (!std::isfinite(mid))
            return false;
        s.mid = mid;
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
        // A valid one-sided book has no mid: the window keeps the last two-sided one.
        if (s.valid && !trackMid(s))
            s.midOpen = s.midMin = s.midMax = s.midClose = s.mid;
    }
    void publish(std::shared_ptr<const Hmc2Record> record) {
        if (!cfg.publisher) return;
        try {
            cfg.publisher(std::move(record));
        } catch (const std::exception &e) {
            sLog_Error("BookRecorder: publisher failed error=" << e.what());
        }
    }
    void publishCopy(const Hmc2Record &record, int64_t through, bool provisional) {
        if (!cfg.publisher) return;
        try {
            if (cfg.beforePublicationForTest) cfg.beforePublicationForTest(provisional);
            auto copy = std::make_shared<Hmc2Record>(record);
            copy->committedThroughMs = through;
            if (provisional) copy->flags |= kProvisional;
            publish(std::move(copy));
        } catch (const std::exception &e) {
            sLog_Error("BookRecorder: publication failed bucket=" << record.bucketStartMs << " error=" << e.what());
        }
    }
    void publishOpen(Symbol &s) {
        if (!cfg.publisher || !s.observed || s.clock - s.lastPublish < cfg.livePublishMs) return;
        s.lastPublish = s.clock;
        for (size_t li = 0; li < s.layers.size(); ++li) {
            const auto &layer = s.layers[li];
            auto r = std::make_shared<Hmc2Record>();
            r->header = layer.header;
            r->bucketStartMs = s.minute;
            r->committedThroughMs = s.closedThrough;
            r->observedMs = s.observed;
            r->flags = s.flags | kProvisional | (s.observed < kMinute ? kPartial : 0);
            r->midOpen = s.midOpen; r->midClose = s.midClose;
            r->midMin = s.midMin; r->midMax = s.midMax;
            const auto [lo, hi] = bounds(s.midMin, s.midMax, cfg.layers[li], cfg.priceScale);
            r->bidRowLo = r->askRowLo = lo;
            r->bidRowHi = r->askRowHi = hi;
            r->entries.reserve(layer.rows.size());
            for (const auto &[key, row] : layer.rows) {
                const auto integral = row.integral + (s.valid ? row.size * (s.clock - row.last) : 0);
                if (key.first >= lo && key.first <= hi && (integral > 0 || row.peak > 0))
                    r->entries.push_back({key.first, key.second,
                        encode(integral / r->observedMs, cfg.sizeScale, r->flags),
                        encode(row.peak, cfg.sizeScale, r->flags)});
            }
            sLog_Probe("recording.publish", "symbol=" << r->header.symbol << " layer=" << r->header.layer
                << " bucket=" << r->bucketStartMs << " observed=" << r->observedMs
                << " entries=" << r->entries.size() << " provisional=1");
            publish(std::move(r));
        }
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
                publishCopy(s.pending.back(), s.closedThrough, true);
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
    void restoreHours(Layer &l, int64_t currentHour) {
        if (currentHour > kHmc2MinMs) {
            const auto previous = currentHour - kHour;
            const auto persisted =
                Hmc2Store::readRange(cfg.root, l.header.symbol, l.header.layer, kHour, previous, currentHour);
            const bool exists = std::any_of(persisted.begin(), persisted.end(),
                                            [&](const auto &r) { return r.header.configHash == l.header.configHash; });
            if (!exists) {
                try {
                    rebuildHour(l, previous);
                    writeHour(l);
                } catch (const std::exception &e) {
                    l.hourWatermarkBlocked = true;
                    ++diskErrors;
                    sLog_Error("BookRecorder: previous-hour recovery failed symbol="
                               << l.header.symbol << " hour=" << previous << " error=" << e.what());
                }
            }
        }
        rebuildHour(l, currentHour);
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
                                       encode(value.second, cfg.sizeScale, out.flags), coverage});
        }
        // Sweep interval endpoints, retaining coverage of absent (zero) entries too.
        for (bool ask : {false, true}) {
            std::map<int64_t, int64_t> changes;
            for (const auto &r : l.hourMinutes) {
                const auto lo = ask ? r.askRowLo : r.bidRowLo;
                const auto hi = ask ? r.askRowHi : r.bidRowHi;
                if (lo <= hi) {
                    changes[lo] += r.observedMs;
                    if (hi != INT64_MAX)
                        changes[hi + 1] -= r.observedMs;
                }
            }
            int64_t covered = 0, lo = 0;
            for (const auto &[edge, delta] : changes) {
                if (covered && edge > lo)
                    out.coverage.push_back({lo, edge - 1, ask, static_cast<uint32_t>(covered)});
                covered += delta;
                lo = edge;
            }
            if (covered)
                out.coverage.push_back({lo, INT64_MAX, ask, static_cast<uint32_t>(covered)});
        }
        write(out);
        if (!l.hourWatermarkBlocked)
            l.hourThroughMs = std::max(l.hourThroughMs, l.hour + kHour);
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
            } catch (const std::exception &e) {
                s.minuteWatermarkBlocked = true;
                for (size_t li = 0; li < s.layers.size(); ++li)
                    if (cfg.layers[li].hourlyRollup) s.layers[li].hourWatermarkBlocked = true;
                ++diskErrors;
                sLog_Error("BookRecorder: column lost bucket=" << r.bucketStartMs << " error=" << e.what());
                continue;
            }
            for (auto &l : s.layers)
                if (l.header.layer == r.header.layer)
                    l.lastColumnMs = std::max(l.lastColumnMs, r.bucketStartMs);
            publishCopy(r, r.bucketStartMs + kMinute, false);
            try {
                rollup(s, r);
            } catch (const std::exception &e) {
                for (size_t li = 0; li < s.layers.size(); ++li)
                    if (cfg.layers[li].hourlyRollup) s.layers[li].hourWatermarkBlocked = true;
                ++diskErrors;
                sLog_Error("BookRecorder: minute committed; hourly accumulation failed bucket="
                           << r.bucketStartMs << " error=" << e.what());
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
                    l.hourWatermarkBlocked = true;
                    ++diskErrors;
                    sLog_Error("BookRecorder: hourly write failed error=" << e.what());
                    l.hourMinutes.clear();
                }
            }
        }
        if (!s.minuteWatermarkBlocked)
            s.minuteThroughMs = std::max(s.minuteThroughMs, s.closedThrough);
        const auto hourBoundary = floorDiv(s.minuteThroughMs, kHour) * kHour;
        // commit() runs per message; the watermarks move at most once a minute.
        // Take the reader lock only when a published value actually changes.
        bool changed = false;
        for (size_t li = 0; li < s.layers.size(); ++li) {
            auto &l = s.layers[li];
            if (cfg.layers[li].hourlyRollup && !l.hourWatermarkBlocked &&
                (l.hour < 0 || l.hour >= hourBoundary || l.hourMinutes.empty()))
                l.hourThroughMs = std::max(l.hourThroughMs, hourBoundary);
            const int64_t hourMs = cfg.layers[li].hourlyRollup ? l.hourThroughMs : 0;
            changed = changed || l.publishedMinuteMs != s.minuteThroughMs || l.publishedHourMs != hourMs ||
                      l.publishedColumnMs != l.lastColumnMs;
        }
        if (!changed)
            return;
        std::lock_guard lock(watermarksMutex);
        for (size_t li = 0; li < s.layers.size(); ++li) {
            auto &l = s.layers[li];
            const int64_t hourMs = cfg.layers[li].hourlyRollup ? l.hourThroughMs : 0;
            if (l.publishedMinuteMs == s.minuteThroughMs && l.publishedHourMs == hourMs &&
                l.publishedColumnMs == l.lastColumnMs)
                continue;
            watermarksBySeries[{l.header.symbol, l.header.layer}] = {s.minuteThroughMs, hourMs, l.lastColumnMs};
            l.publishedMinuteMs = s.minuteThroughMs;
            l.publishedHourMs = hourMs;
            l.publishedColumnMs = l.lastColumnMs;
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
            // A snapshot must seed a mid. After a valid update, a one-sided book is
            // still the exact, continuous book: keep it and integrate (2026-09-30:
            // treating it as invalid stopped the recorder until the next reconnect).
            if (m.kind == Kind::Snapshot) {
                invalidate(m.symbol(), s, "missing two-sided mid");
                return;
            }
            if (s.oneSidedSince < 0) {
                s.oneSidedSince = s.clock;
                sLog_Data("BookRecorder: one-sided book symbol=" << m.symbol()
                                                                << " empty=" << (s.bids.empty() ? "bid" : "ask")
                                                                << " time=" << s.clock);
            }
        } else {
            if (s.oneSidedSince >= 0) {
                sLog_Data("BookRecorder: two-sided again symbol=" << m.symbol() << " time=" << s.clock
                                                                  << " durationMs=" << (s.clock - s.oneSidedSince));
                s.oneSidedSince = -1;
            }
            if (!trackMid(s)) {
                invalidate(m.symbol(), s, "unrepresentable mid");
                return;
            }
        }
        for (auto &l : s.layers)
            for (auto *row : l.touched)
                row->peak = std::max(row->peak, row->size);
        if (m.kind == Kind::Snapshot) {
            s.valid = true;
            s.selfInvalidReason.clear();
            s.validSinceLocal = workerLocal;
            sLog_Data("BookRecorder: snapshot symbol=" << m.symbol() << " time=" << s.clock
                                                       << " levels=" << m.levels.size());
        }
    }
    void process(const Message &m) {
        if (m.time < kHmc2MinMs || m.time > kMaxTime || m.local < kHmc2MinMs || m.local > kMaxTime) {
            sLog_Warning("BookRecorder: rejected timestamp time=" << m.time);
            for (auto &[name, s] : symbols)
                invalidate(name, *s, "invalid timestamp");
            return;
        }
        workerLocal = m.local;
        if (m.kind == Kind::Tick || m.kind == Kind::Invalid) {
            for (auto &[name, ptr] : symbols) {
                if (m.kind == Kind::Invalid && !m.symbol().empty() && name != m.symbol())
                    continue;
                auto &s = *ptr;
                if (!s.initialized) {
                    // No observation yet; a lost first snapshot still needs a retry.
                    if (m.kind == Kind::Invalid)
                        s.selfInvalidReason = m.upstream ? std::string() : m.reason;
                    requestResnapshot(name, s);
                    continue;
                }
                const auto t = std::clamp(m.time - s.offset, int64_t{0}, kMaxTime);
                expireOneSided(name, s, t);
                advance(s, t);
                if (m.kind == Kind::Invalid)
                    invalidate(name, s, m.reason, m.upstream);
                else if (!s.valid)
                    requestResnapshot(name, s); // repeats while still invalid, with backoff
                noteStable(s);
                publishOpen(s);
            }
            return;
        }
        auto &s = getSymbol(m.symbol());
        if (!s.initialized) {
            if (m.kind == Kind::InvalidEnvelope && m.droppedSnapshot) {
                sLog_Warning("BookRecorder: first snapshot dropped symbol=" << m.symbol() << " reason=" << m.reason);
                s.selfInvalidReason = m.reason;
                requestResnapshot(m.symbol(), s);
            }
            if (m.kind != Kind::Snapshot)
                return;
            s.initialized = true;
            s.clock = m.time;
            s.minute = floorDiv(m.time, kMinute) * kMinute;
            s.closedThrough = s.minute;
            for (size_t li = 0; li < s.layers.size(); ++li)
                if (cfg.layers[li].hourlyRollup)
                    restoreHours(s.layers[li], floorDiv(m.time, kHour) * kHour);
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
        expireOneSided(m.symbol(), s, t);
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
        noteStable(s);
        publishOpen(s);
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
    impl_->enqueue({Impl::Kind::Invalid, impl_->internSymbol(symbol), std::move(reason), local, local, {}, true});
}
void BookRecorder::onTick(int64_t local) {
    impl_->enqueue({Impl::Kind::Tick, {}, {}, local, local, {}});
}
BookRecorder::Stats BookRecorder::stats() const {
    const auto &i = *impl_;
    return {i.columnsWritten.load(), i.lateEvents.load(),    i.backwardSteps.load(),
            i.queueDrops.load(),     i.invalidations.load(), i.diskErrors.load()};
}
BookRecorder::Watermarks BookRecorder::watermarks(const std::string &symbol, const std::string &layer) const {
    std::lock_guard lock(impl_->watermarksMutex);
    const auto it = impl_->watermarksBySeries.find({symbol, layer});
    return it == impl_->watermarksBySeries.end() ? Watermarks{} : it->second;
}
void BookRecorder::drainForTest() {
    std::unique_lock lock(impl_->mutex);
    impl_->drained.wait(lock, [&] { return !impl_->count && !impl_->emergency && !impl_->busy; });
}
} // namespace recording
