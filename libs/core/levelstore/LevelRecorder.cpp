#include "LevelRecorder.hpp"
#include "Crc32.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <stdexcept>

// Lifecycle (each case is handled at the place named here and tested in
// tests/levelstore/test_level_recorder.cpp):
// - Day recreation and midnight: roll() creates one LevelRecorder per UTC day with
//   its history recorder (Roller.cpp makeLevel) and destroys it after that recorder.
//   Output covers [floorMs, ceilingMs); the day's recorder runs on past the ceiling
//   for its lateness tail, and those minutes are dropped here (onMinute): the next
//   day's own recorder and LevelRecorder rebuild them from that day's warmup.
// - Warmup: the recorder replays from the snapshot before the floor. Those events
//   build C's book but their minutes are not output (onMinute). The day starts
//   (startDay: one BookImage at floorMs) at the first minute end or event at or
//   after the floor.
// - Restore: exportState/importState below, at the recorder's drained checkpoint.
//   The restored state includes the unfinished minute, the book, the recorder's
//   validity and observation and any unobserved episode, so a restore during an
//   invalid period, right after a reset, during warmup or at midnight continues
//   exactly as without it.
// - Snapshot resets (onReset): every level not re-sent ends, the rest continue
//   unchanged if their size did not change (diffed in endMessage).
// - Observation-invalid periods (onInvalid): every level ends (the recorder cleared
//   its book) and nothing is observed; updates while invalid never reach the sink.
// - forkLead: the lead recorder has no sink (BookRecorder::forkLead), so speculative
//   live replay never reaches C.

namespace sentinel::levelstore {
namespace {
using recording::DecimalIdentity;
constexpr int64_t kMinute = 60'000;
constexpr size_t kMaxSymbol = 256;
constexpr uint8_t kMagic[4] = {'L', 'S', 'C', '1'};
void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what);
}
// The paired recorder checkpoint's identity: its length and its own payload CRC32
// (BookRecorder::exportState appends CRC32(payload) little-endian), validated here.
// A CRC over the whole blob including that trailer would be the same constant
// residue for every valid export, so it could not tell two checkpoints apart.
struct RecorderDigest {
    uint64_t size = 0;
    uint32_t crc = 0;
};
std::optional<RecorderDigest> recorderDigest(const std::vector<uint8_t> &state) {
    if (state.size() < 5) return std::nullopt;
    uint32_t trailer = 0;
    for (int i = 0; i < 4; ++i) trailer |= static_cast<uint32_t>(state[state.size() - 4 + size_t(i)]) << (8 * i);
    if (sentinel::crc32(state.data(), state.size() - 4) != trailer) return std::nullopt;
    return RecorderDigest{state.size(), trailer};
}
// Capacity for `extra` more elements, growing geometrically: an exact-size reserve
// per message would copy the whole minute's timeline on every append.
template <class T> void reserveMore(std::vector<T> &v, size_t extra) {
    const size_t need = v.size() + extra;
    if (need > v.capacity()) v.reserve(std::max(need, std::max<size_t>(64, v.capacity() * 2)));
}
} // namespace

const char *unobservedName(LevelRecorder::Unobserved u) noexcept {
    switch (u) {
    case LevelRecorder::Unobserved::None: return "none";
    case LevelRecorder::Unobserved::Failed: return "failed";
    case LevelRecorder::Unobserved::RestoreRefused: return "restore-refused";
    }
    return "?";
}

std::string decimalText(const recording::RecorderSink::LevelChange &level) {
    char buf[96];
    if (!level.decimal.exact) {
        // The original text, carried with the recorder message (JournalFeed keeps the
        // first such price of each message).
        if (!level.priceText.empty()) return std::string(level.priceText);
        std::snprintf(buf, sizeof buf, "<no price text> price=%.17g", level.price);
        return buf;
    }
    std::string digits = std::to_string(static_cast<uint64_t>(level.decimal.digits));
    const size_t fraction = level.decimal.fraction;
    if (fraction == 0) return digits;
    if (digits.size() <= fraction) digits.insert(0, fraction - digits.size() + 1, '0');
    digits.insert(digits.size() - fraction, 1, '.');
    return digits;
}

LevelRecorder::LevelRecorder(DayBinding binding, LevelOutput *output)
    : binding_(std::move(binding)), output_(output) {
    if (!output_ || binding_.ceilingMs <= binding_.floorMs || binding_.floorMs % kMinute != 0)
        throw std::invalid_argument("LevelRecorder: invalid binding or output");
    touched_.reserve(1024);
}

bool LevelRecorder::mine(const std::string &symbol) noexcept {
    if (symbol == binding_.symbol) return true;
    ++stats_.foreignEvents;
    return false;
}

void LevelRecorder::begin(int64_t clock) {
    if (clock < lastClock_) throw std::logic_error("clock regression");
    lastClock_ = clock;
    if (!dayStarted_ && clock >= binding_.floorMs) startDay();
    if (inMessage_) return;
    inMessage_ = true;
    if (++serial_ == 0) { // wrapped: no slot may keep a stale touch mark
        for (auto &side : book_)
            for (auto &[_, slot] : side) slot.touched = 0;
        serial_ = 1;
    }
    messageReset_ = false;
    touched_.clear();
}

// Warmup ends here: the first BookImage is C's state at the floor instant (nothing
// changes between the last earlier event and the floor). Earlier minutes are dropped.
void LevelRecorder::startDay() {
    open_.messages.clear();
    open_.changes.clear();
    open_.inputs.clear();
    open_.observedAtStart = observed_;
    open_.priceDigits = priceDigits_; open_.sizeDigits = sizeDigits_;
    output_->onDayStart(binding_, BookImage{binding_.floorMs, observed_, book(), priceDigits_, sizeDigits_});
    dayStarted_ = true;
}

void LevelRecorder::touch(bool isBid, int64_t units, int64_t size) {
    auto [it, added] = book_[isBid].try_emplace(units);
    Slot &slot = it->second;
    if (slot.touched != serial_) {
        touched_.push_back({isBid, units, added ? int64_t(0) : slot.size});
        slot.touched = serial_;
    }
    slot.size = size;
}

void LevelRecorder::onReset(const std::string &symbol, int64_t clock) noexcept {
    if (failed() || halted_ || !mine(symbol)) return;
    try {
        begin(clock);
        if (prepared_ && messageReset_) return;
        messageReset_ = true;
        clearBook();
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("unknown exception", clock);
    }
}

void LevelRecorder::onImprecise(const std::string&, int64_t, const LevelChange&) noexcept {}
void LevelRecorder::onLevel(const std::string& symbol, int64_t clock, const LevelChange&) noexcept {
    if (failed() || halted_ || !mine(symbol)) return;
    if (!prepared_ && !malformedSource_) fail("missing exact source", clock);
}

void LevelRecorder::onApplied(const std::string &symbol, int64_t clock, bool valid, bool observing) noexcept {
    if (failed() || halted_ || !mine(symbol)) return;
    try {
        begin(clock);
        if (valid && malformedSource_) { halt("legacy accepted malformed decimal input"); return; }
        if (valid && observing != exactUsable()) { halt("legacy observation contradicts exact book"); return; }
        endMessage(clock, valid, observing, false);
        prepared_ = false; source_ = nullptr;
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("unknown exception", clock);
    }
}

void LevelRecorder::onInvalid(const std::string &symbol, int64_t clock, const std::string &reason, bool upstream) noexcept {
    if (failed() || halted_ || !mine(symbol)) return;
    try {
        begin(clock);
        if (reason == "worker exception") { fail("recorder worker exception", clock); return; }
        if (!upstream && !malformedSource_ && exactUsable()) { halt("legacy invalidation contradicts exact book"); return; }
        endMessage(clock, false, false, true);
        prepared_ = false; source_ = nullptr;
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("unknown exception", clock);
    }
}

void LevelRecorder::endMessage(int64_t clock, bool valid, bool observing, bool invalidated) {
    const bool wasObserved = observed_;
    const bool wasTracking = unobserved_ == Unobserved::None; // while unobserved the published book is empty
    valid_ = valid;
    recorderObserving_ = valid && observing;
    ClearCause clear = ClearCause::None;
    bool reset = false, discard = false;
    if (invalidated) clear = ClearCause::Invalid;
    else reset = messageReset_;
    observed_ = recorderObserving_ && unobserved_ == Unobserved::None;
    inMessage_ = false;
    if (discard) {
        clearBook();
        return;
    }
    if (clear != ClearCause::None) {
        // Clears are ordinary input events. Internal failure never resumes output,
        // so there is no separate reader state or deferred clear to reconcile. A
        // clear is recorded only when it changes the published state: the reader was
        // observed, or it held a level before this message (a level this same message
        // added, as in a snapshot the recorder then rejected, was never published).
        if (prepared_ || (dayStarted_ && (wasObserved || (wasTracking && readerHadLevels())))) {
            allocationSeam("clear");
            open_.messages.push_back({clock, false, false, clear, priceDigits_, sizeDigits_});
            ++stats_.messages;
            ++stats_.clears;
        }
        clearBook();
        return;
    }
    // Snapshot diffs come from hash order: sort them into the canonical book order.
    if (reset)
        std::sort(touched_.begin(), touched_.end(), [](const Touch &a, const Touch &b) {
            return a.isBid != b.isBid ? a.isBid : a.units < b.units;
        });
    // Reserve before any append, so an allocation failure never leaves a change
    // that points at a missing message.
    allocationSeam("changes");
    reserveMore(open_.changes, touched_.size());
    reserveMore(open_.messages, 1);
    const auto message = static_cast<uint32_t>(open_.messages.size());
    const size_t firstChange = open_.changes.size();
    for (const auto &t : touched_) {
        auto &side = book_[t.isBid];
        const auto it = side.find(t.units);
        const int64_t now = it->second.size;
        if (now != t.before) {
            open_.changes.push_back({message, t.isBid, t.units, now});
        }
        if (now == 0) side.erase(it);
    }
    touched_.clear();
    const size_t changes = open_.changes.size() - firstChange;

    open_.messages.push_back({clock, observed_, reset, ClearCause::None, priceDigits_, sizeDigits_});
    ++stats_.messages;
    stats_.changes += changes;
    stats_.resets += reset;
}

void LevelRecorder::onMinute(const std::string &symbol, int64_t bucketStartMs, uint32_t observedMs,
                             uint32_t flags) noexcept {
    if (failed() || halted_ || !mine(symbol)) return;
    const int64_t end = bucketStartMs + kMinute;
    try {
        if (inMessage_) throw std::logic_error("minute inside a message");
        if (end < lastClock_) throw std::logic_error("minute behind the clock");
        if (!open_.messages.empty() &&
            (open_.messages.front().timeMs < bucketStartMs || open_.messages.back().timeMs >= end))
            throw std::logic_error("message outside its minute");
        lastClock_ = end;
        if (bucketStartMs < binding_.floorMs) {
            // Warmup minute: builds the book only.
            if (end >= binding_.floorMs && !dayStarted_) startDay();
        } else {
            if (!dayStarted_) startDay(); // a skipped or snapshot-started warmup: state at the floor
            if (bucketStartMs < binding_.ceilingMs) {
                // Past the ceiling the minute belongs to the next day (dropped here).
                open_.bucketStartMs = bucketStartMs;
                open_.recorderObservedMs = observedMs;
                open_.recorderFlags = flags;
                output_->onBlock(binding_, open_);
                ++stats_.blocks;
            }
        }
        open_.messages.clear();
        open_.changes.clear();
    open_.inputs.clear();
        open_.observedAtStart = observed_;
    open_.priceDigits = priceDigits_; open_.sizeDigits = sizeDigits_;
    } catch (const std::overflow_error &e) {
        try { halt(e.what()); } catch (const std::exception& persistenceError) { fail(persistenceError.what(), end); }
    } catch (const std::exception &e) {
        fail(e.what(), end);
    } catch (...) {
        fail("unknown exception", end);
    }
}

// Every slot present when a message starts has size > 0 (zero sizes are erased at
// the end of each message), so an untouched slot is a published level, and a touched
// slot was published iff its size before this message was > 0. Clear path only.
bool LevelRecorder::readerHadLevels() const noexcept {
    if (liveLevels() > touched_.size()) return true;
    for (const auto &t : touched_)
        if (t.before > 0) return true;
    return false;
}

void LevelRecorder::allocationSeam(const char *site) {
    if (failAllocationForTest && failAllocationForTest(site)) throw std::bad_alloc();
}

void LevelRecorder::clearBook() noexcept {
    legacyKeys_[0].clear(); legacyKeys_[1].clear();
    book_[0].clear();
    book_[1].clear();
    touched_.clear();
}

// Internal failure ends this attempt. Retain partial state for diagnosis only;
// no snapshot, later callback or checkpoint can turn it into usable history.
void LevelRecorder::fail(const char *what, int64_t clock) noexcept {
    if (failed()) return;
    ++stats_.failures;
    unobserved_ = Unobserved::Failed;
    observed_ = false;
    std::snprintf(failureReason_, sizeof failureReason_, "%s", what);
    try {
        sLog_Warning("levelstore: processing stopped symbol=" << binding_.symbol
                     << " error=" << failureReason_ << " clock=" << clock);
    } catch (...) {
    }
}

void LevelRecorder::throwIfFailed() const {
    if (failed()) throw std::runtime_error("levelstore processing stopped: " + binding_.symbol + ": " + failureReason_);
}

std::vector<BookLevel> LevelRecorder::book() const {
    std::vector<BookLevel> out;
    out.reserve(liveLevels());
    for (bool isBid : {true, false}) {
        const size_t first = out.size();
        for (const auto &[units, slot] : book_[isBid])
            if (slot.size > 0) out.push_back({isBid, units, slot.size});
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                  [](const BookLevel &a, const BookLevel &b) { return a.units < b.units; });
    }
    return out;
}

size_t LevelRecorder::memoryBytes() const {
    // unordered_map: one node (key, slot, next pointer, cached hash) per level plus
    // one bucket pointer per bucket; vectors by capacity.
    size_t bytes = 0;
    for (const auto &side : book_) bytes += side.size() * 64;
    bytes += touched_.capacity() * sizeof(Touch);
    bytes += open_.messages.capacity() * sizeof(TimelineMessage) + open_.changes.capacity() * sizeof(TimelineChange);
    return bytes;
}

// Blob v6: "LSC1", u16 version, binding, paired recorder clock/length/payload CRC,
// flags, observation episode, clock, stats, book, open minute and payload CRC.
// Only healthy state is exportable. Sizes and times are integers; scales are explicit.
std::vector<uint8_t> LevelRecorder::exportState(std::optional<int64_t> recorderClock,
                                                const std::vector<uint8_t> &recorderState) const {
    throwIfFailed();
    if (halted_) throw std::runtime_error("halted C cannot export state");
    if (inMessage_) throw std::logic_error("LevelRecorder::exportState inside a message (recorder not drained)");
    if (recorderClock ? *recorderClock < lastClock_ : lastClock_ != INT64_MIN)
        throw std::logic_error("LevelRecorder::exportState: checkpoint clock behind C's last event");
    ByteWriter w;
    for (uint8_t m : kMagic) w.u8(m);
    w.u8(static_cast<uint8_t>(kStateVersion));
    w.u8(static_cast<uint8_t>(kStateVersion >> 8));
    w.str(binding_.symbol);
    w.svarint(binding_.floorMs);
    w.svarint(binding_.ceilingMs);
    w.svarint(binding_.priceDigits);
    w.fixed32(priceDigits_); w.fixed32(sizeDigits_);
    w.f64(legacyPriceScale_);
    nlohmann::json provenance = nullptr;
    if (sourceOwner_) {
        const auto& src = *sourceOwner_;
        provenance = {{"file",src.file},{"run",src.run},{"block",src.block},{"record",src.record},
            {"sequence",src.sequence},{"priceIncrement",src.priceIncrement},{"sizeIncrement",src.sizeIncrement},
            {"snapshot",src.snapshot},{"malformed",src.malformed}};
        if (!src.levels.empty()) {
            const auto& first = src.levels.front();
            provenance["level"] = {{"bid",first.isBid},{"price",first.priceText},{"size",first.sizeText}};
        }
    }
    w.str(provenance.dump());
    w.u8(recorderClock ? 1 : 0);
    w.svarint(recorderClock.value_or(0));
    const auto digest = recorderDigest(recorderState);
    if (!digest) throw std::logic_error("LevelRecorder::exportState: the paired recorder state is not a valid export");
    w.fixed64(digest->size);
    w.fixed32(digest->crc);
    w.u8(static_cast<uint8_t>((dayStarted_ ? 1 : 0) | (valid_ ? 2 : 0) | (recorderObserving_ ? 4 : 0) |
                              (observed_ ? 8 : 0)));
    w.u8(static_cast<uint8_t>(unobserved_));
    w.u8(lastClock_ == INT64_MIN ? 0 : 1);
    w.svarint(lastClock_ == INT64_MIN ? 0 : lastClock_);
    for (uint64_t v : {stats_.messages, stats_.changes, stats_.blocks, stats_.resets, stats_.clears,
                       stats_.offGridValues, stats_.halts, stats_.failures, stats_.restoreRefusals,
                       stats_.foreignEvents})
        w.varint(v);
    const auto levels = book();
    for (bool isBid : {true, false}) {
        const auto n = std::count_if(levels.begin(), levels.end(), [&](const BookLevel &l) { return l.isBid == isBid; });
        w.varint(static_cast<uint64_t>(n));
        int64_t units = 0;
        for (const auto &l : levels)
            if (l.isBid == isBid) {
                w.svarint(l.units - units);
                units = l.units;
                w.varint(static_cast<uint64_t>(l.size));
            }
    }
    encodeBlock(w, open_);
    w.fixed32(sentinel::crc32(w.bytes.data(), w.bytes.size()));
    return std::move(w.bytes);
}

LevelRecorder::ImportResult LevelRecorder::importState(const std::vector<uint8_t> &blob,
                                                       std::optional<int64_t> recorderClock,
                                                       const std::vector<uint8_t> &recorderState) noexcept {
    // Fixed storage: copying the reason must not allocate inside this noexcept path.
    char reason[192] = "unknown exception";
    try {
        throwIfFailed();
        require(lastClock_ == INT64_MIN && !dayStarted_ && !inMessage_ && liveLevels() == 0 &&
                    open_.messages.empty() && stats_ == Stats{},
                "import requires a fresh LevelRecorder");
        require(blob.size() >= 4 + 2 + 4, "no state");
        uint32_t crc = 0;
        for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(blob[blob.size() - 4 + i]) << (8 * i);
        require(crc == sentinel::crc32(blob.data(), blob.size() - 4), "state CRC mismatch");
        ByteReader r(std::span<const uint8_t>(blob.data(), blob.size() - 4));
        for (uint8_t m : kMagic) require(r.u8() == m, "not a levelstore state");
        const uint16_t version = static_cast<uint16_t>(r.u8() | (r.u8() << 8));
        require(version == kStateVersion, "state version mismatch");
        DayBinding b;
        b.symbol = r.str(kMaxSymbol);
        b.floorMs = r.svarint();
        b.ceilingMs = r.svarint();
        b.priceDigits = static_cast<int>(r.svarint());
        const auto priceDigits = r.fixed32(), sizeDigits = r.fixed32();
        const auto legacyScale = r.f64();
        const auto provenance = nlohmann::json::parse(r.str(r.remaining()));
        recording::ExactSourcePtr sourceOwner;
        if (!provenance.is_null()) {
            auto src = std::make_shared<recording::ExactSource>();
            src->file=provenance.at("file");src->run=provenance.at("run");src->block=provenance.at("block");
            src->record=provenance.at("record");src->sequence=provenance.at("sequence");
            src->priceIncrement=provenance.at("priceIncrement");src->sizeIncrement=provenance.at("sizeIncrement");
            src->snapshot=provenance.at("snapshot");src->malformed=provenance.at("malformed");
            if (provenance.contains("level")) {
                const auto& level=provenance.at("level");
                recording::ExactSourceLevel l;l.isBid=level.at("bid");l.priceText=level.at("price");l.sizeText=level.at("size");
                l.price=parseDecimal(l.priceText);l.size=parseDecimal(l.sizeText);src->levels.push_back(std::move(l));
            }
            sourceOwner=std::move(src);
        }
        require(std::isfinite(legacyScale) && legacyScale > 0, "legacy scale");
        require(b == binding_, "state belongs to another product-day binding");
        const uint8_t hasClock = r.u8();
        const int64_t clock = r.svarint();
        require(hasClock <= 1 && (hasClock ? std::optional<int64_t>(clock) : std::nullopt) == recorderClock,
                "state checkpoint clock differs from the recorder's");
        const auto digest = recorderDigest(recorderState);
        require(digest.has_value(), "the recorder state it would be paired with is not a valid export");
        const uint64_t pairedSize = r.fixed64();
        const uint32_t pairedCrc = r.fixed32();
        require(pairedSize == digest->size && pairedCrc == digest->crc, "state is not paired with this recorder state");
        const uint8_t flags = r.u8();
        require(flags <= 15, "state flags");
        const uint8_t episode = r.u8();
        require(episode == 0, "state episode");
        const uint8_t hasLast = r.u8();
        const int64_t last = r.svarint();
        require(hasLast <= 1 && (!hasLast || (recorderClock && last <= *recorderClock)), "state clock");
        Stats s;
        for (uint64_t *v : {&s.messages, &s.changes, &s.blocks, &s.resets, &s.clears, &s.offGridValues,
                            &s.halts, &s.failures, &s.restoreRefusals, &s.foreignEvents})
            *v = r.varint();
        Side sides[2];
        for (bool isBid : {true, false}) {
            const uint64_t n = r.varint();
            require(n <= r.remaining(), "state book count");
            int64_t units = 0;
            for (uint64_t i = 0; i < n; ++i) {
                const int64_t d = r.svarint();
                require(d > 0, "state book order");
                units = checkedAdd(units, d, "state book key overflow");
                const auto size = r.varint();
                require(size > 0 && size <= INT64_MAX, "state book size");
                sides[isBid].emplace(units, Slot{static_cast<int64_t>(size), 0});
            }
        }
        MinuteBlock open = decodeBlock(r);
        require(r.done(), "state trailing bytes");
        const bool dayStarted = flags & 1, valid = flags & 2, recorderObserving = flags & 4, observed = flags & 8;
        const auto unobserved = static_cast<Unobserved>(episode);
        require(!recorderObserving || valid, "state observation without validity");
        require(observed == (recorderObserving && unobserved == Unobserved::None), "state observation");
        require(unobserved == Unobserved::None || (sides[0].empty() && sides[1].empty()), "state episode with a book");
        require(open.messages.empty() || (hasLast && open.messages.back().timeMs <= last &&
                                          open.messages.front().timeMs >= recording::kHmc2MinMs),
                "state open minute clock");
        require(open.changes.empty() || open.changes.back().message < open.messages.size(), "state open minute");
        require(dayStarted || open.messages.empty() || !recorderClock || *recorderClock < binding_.floorMs,
                "state warmup");
        book_[0].swap(sides[0]);
        book_[1].swap(sides[1]);
        open_ = std::move(open);
        dayStarted_ = dayStarted;
        valid_ = valid;
        recorderObserving_ = recorderObserving;
        observed_ = observed;
        unobserved_ = unobserved;
        lastClock_ = hasLast ? last : INT64_MIN;
        stats_ = s;
        sourceOwner_ = std::move(sourceOwner);
        priceDigits_ = priceDigits; sizeDigits_ = sizeDigits; legacyPriceScale_ = legacyScale;
        for (bool bid : {false, true}) for (const auto& [p, q] : book_[bid]) {
            const auto text = decimalText(ExactDecimal{p, priceDigits_});
            const auto key = recording::priceUnits(std::strtod(text.c_str(), nullptr), legacyPriceScale_);
            require(key.has_value(), "restored unkeyable price");
            require(legacyKeys_[bid].emplace(*key, p).second, "restored price collision");
        }
        return {true, {}};
    } catch (const std::exception &e) {
        std::snprintf(reason, sizeof reason, "%s", e.what());
    } catch (...) {
        std::snprintf(reason, sizeof reason, "%s", "unknown exception");
    }
    if (!failed()) {
        unobserved_ = Unobserved::RestoreRefused;
        observed_ = false;
        ++stats_.restoreRefusals;
        std::snprintf(failureReason_, sizeof failureReason_, "%s", reason);
        try {
            sLog_Warning("levelstore: checkpoint state refused; processing stopped symbol="
                         << binding_.symbol << " reason=" << reason << " bytes=" << blob.size()
                         << " recorderClock=" << recorderClock.value_or(-1));
        } catch (...) {
        }
    }
    ImportResult refused;
    try {
        refused.reason = reason;
    } catch (...) { // the reason is diagnostics only
    }
    return refused;
}

} // namespace sentinel::levelstore
