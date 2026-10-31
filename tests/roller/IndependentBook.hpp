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
        if (watchLeft_ && !--watchLeft_) watch_.clear();
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
                if (valid && ok) diagnose(*updates, next, j);
                side[0].swap(next[0]); side[1].swap(next[1]);
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
                if (!applyLevels(*updates, side, &j)) invalidate();
            }
        }
        return gap;
    }

  private:
    std::string product_, run_;
    uint64_t connection_ = 0, updateIndex_ = 0;
    bool started_ = false;
    std::optional<uint64_t> sequence_;
    std::map<std::string, std::string> last_[2]; // price -> last update (event time, size, sequence)
    std::set<std::pair<int, std::string>> watch_;
    uint64_t watchLeft_ = 0;
    void invalidate() {
        valid = false;
        side[0].clear(); side[1].clear();
    }
    // Snapshot taken while the book is valid: per-level differences, each with the
    // level's last update before the snapshot; later updates to them are watched.
    void diagnose(const nlohmann::json &updates, const std::map<std::string, std::string> (&next)[2],
                  const nlohmann::json &frame) {
        std::map<std::string, std::string> eventTime[2];
        for (const auto &u : updates) {
            const auto p = canonDecimal(u.value("price_level", ""));
            if (p) eventTime[u.value("side", "") == "bid" ? 0 : 1][*p] = u.value("event_time", "");
        }
        nlohmann::json d = {{"timestamp", frame.value("timestamp", "")}, {"sequence", frame.value("sequence_num", uint64_t{0})},
                            {"onlyBook", 0}, {"onlySnapshot", 0}, {"sizeMismatch", 0},
                            {"levels", nlohmann::json::array()}, {"after", nlohmann::json::array()}};
        watch_.clear();
        for (int sd = 0; sd < 2; ++sd) {
            const auto note = [&](const std::string &price, const char *kind, const std::string &book, const std::string &snap) {
                d[kind] = d[kind].get<int>() + 1;
                watch_.insert({sd, price});
                if (d["levels"].size() < 40)
                    d["levels"].push_back({{"side", sd ? "ask" : "bid"}, {"price", price}, {"kind", kind}, {"book", book},
                                           {"snapshot", snap}, {"snapshotEventTime", eventTime[sd][price]},
                                           {"lastUpdate", last_[sd].contains(price) ? last_[sd].at(price) : ""}});
            };
            for (const auto &[p, q] : side[sd]) {
                const auto it = next[sd].find(p);
                if (it == next[sd].end()) note(p, "onlyBook", q, "");
                else if (it->second != q) note(p, "sizeMismatch", q, it->second);
            }
            for (const auto &[p, q] : next[sd])
                if (!side[sd].contains(p)) note(p, "onlySnapshot", "", q);
        }
        watchLeft_ = watch_.empty() ? 0 : 5000; // frames
        diagnostics.push_back(std::move(d));
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
                if (watch_.erase({index, *price}) && !diagnostics.empty() && diagnostics.back()["after"].size() < 40)
                    diagnostics.back()["after"].push_back({{"side", index ? "ask" : "bid"}, {"price", *price},
                                                           {"update", stamp}, {"framesAfter", 5000 - watchLeft_}});
                last_[index][*price] = stamp;
            }
            if (*size == "0") target[index].erase(*price);
            else target[index][*price] = *size;
        }
        return true;
    }
};

} // namespace e1a
