#pragma once
// Test-only ground truth: a minimal Coinbase L2 book rebuilt from raw journal
// payloads. It shares no parsing or book code with JournalFeed, BookRecorder
// or the Data Lab engine; JournalReader supplies record framing and
// decompression only. Prices and sizes stay exact decimal text.
#include "roller/JournalReader.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace e1a {

// Canonical decimal: digits[.digits], no leading integer zeros, no trailing
// fraction zeros. Empty optional when the text is not a plain decimal.
inline std::optional<std::string> canonDecimal(std::string_view s) {
    if (s.empty()) return {};
    size_t dot = std::string_view::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '.' && dot == std::string_view::npos && i > 0) dot = i;
        else if (s[i] < '0' || s[i] > '9') return {};
    }
    std::string intPart(s.substr(0, dot == std::string_view::npos ? s.size() : dot));
    std::string frac(dot == std::string_view::npos ? std::string_view{} : s.substr(dot + 1));
    while (intPart.size() > 1 && intPart.front() == '0') intPart.erase(intPart.begin());
    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    return frac.empty() ? intPart : intPart + "." + frac;
}
// Integer price units at 10^decimals, as canonical decimal text.
inline std::string unitsToDecimal(int64_t units, int decimals) {
    std::string s = std::to_string(units);
    if (decimals <= 0) return s;
    if (s.size() <= size_t(decimals)) s.insert(0, size_t(decimals) + 1 - s.size(), '0');
    s.insert(s.size() - size_t(decimals), ".");
    return *canonDecimal(s);
}
// Shortest fixed-notation text that round-trips the double, canonicalised.
inline std::string doubleToDecimal(double v) {
    char buf[512];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    const auto c = canonDecimal(std::string_view(buf, size_t(r.ptr - buf)));
    return c ? *c : std::string(buf, size_t(r.ptr - buf));
}

class IndependentBook {
  public:
    explicit IndependentBook(std::string product) : product_(std::move(product)) {}
    std::map<std::string, std::string> side[2]; // [0] bids, [1] asks: canonical price -> canonical size
    bool valid = false;
    nlohmann::json diagnostics = nlohmann::json::array(); // snapshots taken while valid
    nlohmann::json jumps = nlohmann::json::array();       // first sequence jumps (seq > previous + 1)
    uint64_t dropUpdateEvent = 0; // mutation seam: ignore the Nth update event (1-based), 0 = off
    int64_t dropAtReceiveMs = 0;  // mutation seam: ignore the first update event received at/after this
    struct Counts {
        uint64_t records = 0, frames = 0, snapshots = 0, updateEvents = 0, levels = 0, droppedUpdates = 0,
                 v1SequenceGaps = 0, sequenceRegressions = 0, routingGaps = 0, journalGaps = 0,
                 connectionBoundaries = 0, transport = 0, captureInvalidations = 0, malformed = 0,
                 v1Frames = 0, v2Frames = 0, frameReferences = 0, sequenceJumpsAnyVersion = 0;
    } counts;
    // Applies one record; returns true when it carried a sequence discontinuity
    // (v1 jump, regression, routing receipt gap or a framing gap).
    bool apply(const sentinel::roller::JournalRecord &in) {
        ++counts.records;
        const auto &r = in.record;
        nowMs_ = r.time.systemNs / 1'000'000;
        ++frameIndex_;
        evaluateHorizons();
        bool gap = false;
        if (started_ && in.gapBefore) { ++counts.journalGaps; gap = true; }
        if (!started_ || in.gapBefore || run_ != in.pos.run || connection_ != r.connection) {
            if (started_) ++counts.connectionBoundaries;
            invalidate();
            sequence_.reset();
            run_ = in.pos.run;
            connection_ = r.connection;
            started_ = true;
        }
        using sentinel::capture::Kind;
        switch (r.kind) {
        case Kind::TransportUp:
        case Kind::TransportDown:
        case Kind::CaptureStopped:
            ++counts.transport;
            invalidate();
            sequence_.reset();
            return gap;
        case Kind::BookInvalidated:
        case Kind::ResyncRequested: {
            const auto j = nlohmann::json::parse(r.payload, nullptr, false);
            const auto p = j.is_object() ? j.value("product", "") : "";
            if (p.empty() || p == product_) { ++counts.captureInvalidations; invalidate(); }
            return gap;
        }
        case Kind::FrameReference: {
            ++counts.frameReferences;
            const auto j = nlohmann::json::parse(r.payload, nullptr, false);
            if (j.is_object() && j.value("sequence_gaps", uint64_t{0})) { ++counts.routingGaps; invalidate(); gap = true; }
            return gap;
        }
        case Kind::Frame: break;
        default: return gap;
        }
        ++counts.frames;
        ++(in.version == 1 ? counts.v1Frames : counts.v2Frames);
        const auto j = nlohmann::json::parse(r.payload, nullptr, false);
        if (!j.is_object()) { ++counts.malformed; invalidate(); return gap; }
        if (const auto s = j.find("sequence_num"); s != j.end() && s->is_number_integer()) {
            const auto seq = s->get<uint64_t>();
            if (sequence_ && seq > *sequence_ + 1) {
                ++counts.sequenceJumpsAnyVersion;
                if (jumps.size() < 50)
                    jumps.push_back({{"from", *sequence_}, {"to", seq}, {"version", in.version},
                                     {"receiveMs", r.time.systemNs / 1'000'000}});
            }
            if (sequence_ && seq <= *sequence_) { ++counts.sequenceRegressions; gap = true; }
            else if (sequence_ && in.version == 1 && seq != *sequence_ + 1) { ++counts.v1SequenceGaps; gap = true; }
            sequence_ = seq;
            if (gap) { invalidate(); return true; } // a frame after a discontinuity is not applied
        }
        if (j.value("channel", "") != "l2_data") return gap;
        const auto events = j.find("events");
        if (events == j.end() || !events->is_array()) return gap;
        for (const auto &e : *events) {
            if (!e.is_object() || e.value("product_id", "") != product_) continue;
            const auto type = e.value("type", "");
            const auto updates = e.find("updates");
            if (updates == e.end() || !updates->is_array()) { ++counts.malformed; invalidate(); continue; }
            if (type == "snapshot") {
                ++counts.snapshots;
                std::map<std::string, std::string> next[2];
                const bool ok = applyLevels(*updates, next, nullptr);
                closeFork("interrupted by snapshot");
                if (valid && ok) diagnose(*updates, next, j, in);
                side[0].swap(next[0]); side[1].swap(next[1]);
                last_[0].clear(); last_[1].clear(); // stamps describe one book segment only
                valid = ok;
                if (!valid) invalidate();
            } else if (type == "update") {
                if (!valid) continue;
                if (dropUpdateEvent && ++updateIndex_ == dropUpdateEvent) { ++counts.droppedUpdates; continue; }
                if (dropAtReceiveMs && r.time.systemNs / 1'000'000 >= dropAtReceiveMs && !counts.droppedUpdates) {
                    ++counts.droppedUpdates;
                    continue;
                }
                ++counts.updateEvents;
                if (fork_.active) {
                    const auto levels = counts.levels;
                    applyLevels(*updates, fork_.side, nullptr);
                    counts.levels = levels;
                }
                if (!applyLevels(*updates, side, &j)) invalidate();
            }
        }
        return gap;
    }

    // End of replay: levels never updated again and unfinished horizons say so.
    void finish() {
        closeWatch("none before end of replay");
        closeFork("replay ended");
    }

  private:
    std::string product_, run_;
    uint64_t connection_ = 0, updateIndex_ = 0, frameIndex_ = 0;
    int64_t nowMs_ = 0;
    bool started_ = false;
    std::optional<uint64_t> sequence_;
    std::map<std::string, std::string> last_[2]; // price -> last update in this book segment
    // Differing levels of the newest valid-book snapshot -> index in its "levels"; each
    // gets its first later update (no frame cap) or the reason none was seen.
    std::map<std::pair<int, std::string>, size_t> watch_;
    struct Snap {
        int64_t receiveMs = 0;
        uint64_t frame = 0;
    } watchFrom_;
    // Controlled temporal test: the pre-snapshot book, never given the snapshot,
    // keeps taking the same updates; compared with the installed book at horizons.
    struct Fork {
        bool active = false;
        int64_t startMs = 0;
        size_t diag = 0, next = 0;
        std::map<std::string, std::string> side[2];
        std::set<std::pair<int, std::string>> initial;
    } fork_;
    static constexpr int64_t kHorizonsMs[3] = {5'000, 15'000, 60'000};
    void invalidate() {
        closeWatch("none before invalidation");
        closeFork("interrupted by invalidation");
        valid = false;
        side[0].clear(); side[1].clear();
        last_[0].clear(); last_[1].clear();
    }
    void closeWatch(const char *why) {
        for (const auto &[key, index] : watch_)
            diagnostics.back()["levels"][index]["next"] = {{"none", why}, {"secondsWatched", (nowMs_ - watchFrom_.receiveMs) / 1e3}};
        watch_.clear();
    }
    nlohmann::json forkDiff() const {
        uint64_t differing = 0, initialStill = 0;
        for (int sd = 0; sd < 2; ++sd) {
            const auto differs = [&](const std::string &p) {
                const auto a = side[sd].find(p), b = fork_.side[sd].find(p);
                return (a == side[sd].end()) != (b == fork_.side[sd].end()) ||
                       (a != side[sd].end() && a->second != b->second);
            };
            std::set<std::string> prices;
            for (const auto &[p, q] : side[sd]) prices.insert(p);
            for (const auto &[p, q] : fork_.side[sd]) prices.insert(p);
            for (const auto &p : prices) {
                if (!differs(p)) continue;
                ++differing;
                if (fork_.initial.contains({sd, p})) ++initialStill;
            }
        }
        return {{"differing", differing}, {"initialStillDiffering", initialStill},
                {"initialConverged", fork_.initial.size() - initialStill}, {"newDiffering", differing - initialStill}};
    }
    void evaluateHorizons() {
        while (fork_.active && fork_.next < 3 && nowMs_ >= fork_.startMs + kHorizonsMs[fork_.next]) {
            auto h = forkDiff();
            h["horizonS"] = kHorizonsMs[fork_.next] / 1000;
            diagnostics[fork_.diag]["horizons"].push_back(std::move(h));
            if (++fork_.next == 3) fork_.active = false;
        }
    }
    void closeFork(const char *why) {
        if (!fork_.active) return;
        for (; fork_.next < 3; ++fork_.next)
            diagnostics[fork_.diag]["horizons"].push_back({{"horizonS", kHorizonsMs[fork_.next] / 1000}, {"not reached", why}});
        fork_.active = false;
    }
    // Snapshot taken while the book is valid: every differing level, uncapped, with
    // its last update in this segment and (filled later) its first later update.
    void diagnose(const nlohmann::json &updates, const std::map<std::string, std::string> (&next)[2],
                  const nlohmann::json &frame, const sentinel::roller::JournalRecord &in) {
        closeWatch("superseded by a later snapshot");
        std::map<std::string, std::string> eventTime[2];
        for (const auto &u : updates) {
            const auto p = canonDecimal(u.value("price_level", ""));
            if (p) eventTime[u.value("side", "") == "bid" ? 0 : 1][*p] = u.value("event_time", "");
        }
        nlohmann::json d = {{"timestamp", frame.value("timestamp", "")}, {"sequence", frame.value("sequence_num", uint64_t{0})},
                            {"channel", frame.value("channel", "")}, {"run", in.pos.run}, {"block", in.pos.block},
                            {"record", in.pos.record}, {"connection", in.record.connection}, {"version", in.version},
                            {"receiveMs", nowMs_}, {"onlyBook", 0}, {"onlySnapshot", 0}, {"sizeMismatch", 0},
                            {"levels", nlohmann::json::array()}, {"horizons", nlohmann::json::array()}};
        diagnostics.push_back(std::move(d));
        auto &out = diagnostics.back();
        fork_ = {};
        fork_.active = true;
        fork_.startMs = nowMs_;
        fork_.diag = diagnostics.size() - 1;
        fork_.side[0] = side[0];
        fork_.side[1] = side[1];
        watchFrom_ = {nowMs_, frameIndex_};
        for (int sd = 0; sd < 2; ++sd) {
            const auto note = [&](const std::string &price, const char *kind, const std::string &book, const std::string &snap) {
                out[kind] = out[kind].get<int>() + 1;
                watch_[{sd, price}] = out["levels"].size();
                fork_.initial.insert({sd, price});
                out["levels"].push_back({{"side", sd ? "ask" : "bid"}, {"price", price}, {"kind", kind}, {"book", book},
                                         {"snapshot", snap}, {"snapshotEventTime", eventTime[sd][price]},
                                         {"lastUpdateThisSegment", last_[sd].contains(price) ? last_[sd].at(price) : ""}});
            };
            for (const auto &[p, q] : side[sd]) {
                const auto it = next[sd].find(p);
                if (it == next[sd].end()) note(p, "onlyBook", q, "");
                else if (it->second != q) note(p, "sizeMismatch", q, it->second);
            }
            for (const auto &[p, q] : next[sd])
                if (!side[sd].contains(p)) note(p, "onlySnapshot", "", q);
        }
    }
    bool applyLevels(const nlohmann::json &updates, std::map<std::string, std::string> (&target)[2],
                     const nlohmann::json *frame) {
        for (const auto &u : updates) {
            if (!u.is_object()) return false;
            const auto s = u.find("side"), p = u.find("price_level"), q = u.find("new_quantity");
            if (s == u.end() || p == u.end() || q == u.end() || !s->is_string() || !p->is_string() || !q->is_string())
                return false;
            const auto &sideText = s->get_ref<const std::string &>();
            const int index = sideText == "bid" ? 0 : sideText == "offer" || sideText == "ask" ? 1 : -1;
            const auto price = canonDecimal(p->get_ref<const std::string &>());
            const auto size = canonDecimal(q->get_ref<const std::string &>());
            if (index < 0 || !price || !size || *price == "0") return false;
            ++counts.levels;
            if (frame) {
                const std::string stamp = u.value("event_time", "") + " size=" + *size + " seq=" +
                                          std::to_string(frame->value("sequence_num", uint64_t{0}));
                if (const auto w = watch_.find({index, *price}); w != watch_.end()) {
                    diagnostics.back()["levels"][w->second]["next"] = {
                        {"update", stamp}, {"recordsAfter", frameIndex_ - watchFrom_.frame},
                        {"secondsAfter", (nowMs_ - watchFrom_.receiveMs) / 1e3},
                        {"equalsSnapshot", diagnostics.back()["levels"][w->second]["snapshot"] == (*size == "0" ? "" : *size)}};
                    watch_.erase(w);
                }
                last_[index][*price] = stamp;
            }
            if (*size == "0") target[index].erase(*price);
            else target[index][*price] = *size;
        }
        return true;
    }
};

} // namespace e1a
