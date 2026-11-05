#include "LevelRecorder.hpp"
#include "ExactDecimal.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace sentinel::levelstore {
void LevelRecorder::halt(const char *reason, size_t index) {
    if (halted_ || failed())
        return;
    if (!source_)
        source_ = sourceOwner_.get();
    nlohmann::json record = {{"representationVersion", representationVersion},
                             {"symbol", binding_.symbol},
                             {"reason", reason},
                             {"clock", lastClock_},
                             {"priceDigits", priceDigits_},
                             {"sizeDigits", sizeDigits_},
                             {"file", ""},
                             {"run", ""},
                             {"block", 0},
                             {"record", 0},
                             {"sequence", 0},
                             {"priceIncrement", ""},
                             {"sizeIncrement", ""},
                             {"priceText", ""},
                             {"sizeText", ""}};
    if (source_) {
        record["file"] = source_->file;
        record["run"] = source_->run;
        record["block"] = source_->block;
        record["record"] = source_->record;
        record["sequence"] = source_->sequence;
        record["priceIncrement"] = source_->priceIncrement;
        record["sizeIncrement"] = source_->sizeIncrement;
        if (index < source_->levels.size()) {
            record["priceText"] = source_->levels[index].priceText;
            record["sizeText"] = source_->levels[index].sizeText;
            record["isBid"] = source_->levels[index].isBid;
        }
    }
    output_->onHalt(record); // A persistence failure is transient, not a durable halt.
    haltRecord_ = std::move(record);
    halted_ = true;
    ++stats_.halts;
    sLog_Error("levelstore: product halted record=" << haltRecord_.dump());
}

bool LevelRecorder::exactUsable() const {
    auto bid = book_[1].rbegin();
    while (bid != book_[1].rend() && bid->second.size == 0)
        ++bid;
    auto ask = book_[0].begin();
    while (ask != book_[0].end() && ask->second.size == 0)
        ++ask;
    return bid != book_[1].rend() && ask != book_[0].end() && bid->first < ask->first;
}

void LevelRecorder::changeScale(uint32_t priceDigits, uint32_t sizeDigits) {
    if (priceDigits == priceDigits_ && sizeDigits == sizeDigits_)
        return;
    const auto scaled = [](int64_t n, uint32_t old, uint32_t next) {
        auto v = decimalAtScale({n, old}, next);
        if (!v)
            throw std::overflow_error("scale transition exceeds int64");
        return *v;
    };
    Side next[2];
    for (bool bid : {false, true})
        for (const auto &[price, slot] : book_[bid])
            next[bid].emplace(scaled(price, priceDigits_, priceDigits),
                              Slot{scaled(slot.size, sizeDigits_, sizeDigits), slot.touched});
    for (auto &t : touched_) {
        t.units = scaled(t.units, priceDigits_, priceDigits);
        t.before = scaled(t.before, sizeDigits_, sizeDigits);
    }
    for (auto &keys : legacyKeys_)
        for (auto &[key, price] : keys)
            price = scaled(price, priceDigits_, priceDigits);
    book_[0].swap(next[0]);
    book_[1].swap(next[1]);
    priceDigits_ = priceDigits;
    sizeDigits_ = sizeDigits;
}

void LevelRecorder::onSource(const std::string &symbol, int64_t clock, const recording::ExactSourcePtr &owner,
                             double legacyScale, bool applied) noexcept {
    if (failed() || halted_ || !mine(symbol))
        return;
    sourceOwner_ = owner;
    const auto &source = *owner;
    source_ = &source;
    lastClock_ = clock;
    try {
        if (!applied) {
            if (valid_)
                halt("legacy dropped input while exact consumer was valid");
            source_ = nullptr;
            return;
        }
        prepared_ = false;
        malformedSource_ = source.malformed;
        if (source.malformed)
            return; // Let the recorder apply its existing invalid-input decision.
        const auto pi = parseDecimal(source.priceIncrement), qi = parseDecimal(source.sizeIncrement);
        if (pi.status != DecimalStatus::Exact || qi.status != DecimalStatus::Exact || !pi.value.mantissa ||
            !qi.value.mantissa) {
            halt("invalid increment metadata");
            return;
        }
        uint32_t pd = std::max(priceDigits_, pi.value.digits), qd = std::max(sizeDigits_, qi.value.digits);
        for (size_t i = 0; i < source.levels.size(); ++i) {
            const auto &l = source.levels[i];
            if (l.price.status != DecimalStatus::Exact || l.size.status != DecimalStatus::Exact) {
                halt("decimal mantissa exceeds int64", i);
                return;
            }
            pd = std::max(pd, l.price.value.digits);
            qd = std::max(qd, l.size.value.digits);
            for (const auto &pair : {std::pair(l.price.value, pi.value), std::pair(l.size.value, qi.value)})
                if (!onDecimalGrid(pair.first, pair.second)) {
                    ++stats_.offGridValues;
                    sLog_Probe("levelstore.off_grid", "symbol=" << symbol << " value=" << decimalText(pair.first)
                                                                << " increment=" << decimalText(pair.second));
                }
        }
        if (pd > scaleCapForTest || qd > scaleCapForTest) {
            halt("test scale cap");
            return;
        }
        if (!dayStarted_ && liveLevels() == 0)
            changeScale(pd, qd);
        begin(clock);
        if (source.snapshot) {
            onReset(symbol, clock);
            legacyKeys_[0].clear();
            legacyKeys_[1].clear();
        }
        changeScale(pd, qd);
        legacyPriceScale_ = legacyScale;
        prepared_ = true;
        expectedChanges_.assign(source.levels.size(), false);
        for (size_t i = 0; i < source.levels.size(); ++i) {
            const auto &l = source.levels[i];
            const auto price = decimalAtScale(l.price.value, pd), size = decimalAtScale(l.size.value, qd);
            if (!price || !size) {
                halt("scaled decimal exceeds int64", i);
                return;
            }
            const auto key = recording::priceUnits(std::strtod(l.priceText.c_str(), nullptr), legacyScale);
            if (!key) {
                halt("legacy cannot key exact price", i);
                return;
            }
            if (*size > 0 && std::strtod(l.sizeText.c_str(), nullptr) == 0) {
                halt("legacy loses positive quantity", i);
                return;
            }
            for (bool bid : {false, true}) {
                const auto it = legacyKeys_[bid].find(*key);
                if (it != legacyKeys_[bid].end() && it->second != *price) {
                    halt("distinct exact prices share legacy key", i);
                    return;
                }
            }
            auto &side = book_[l.isBid];
            const auto found = side.find(*price);
            const int64_t old = found == side.end() ? 0 : found->second.size;
            expectedChanges_[i] = old != *size;
            if (exactInputForTest && clock >= binding_.floorMs && clock < binding_.ceilingMs)
                exactInputForTest(clock, l);
            touch(l.isBid, *price, *size);
            open_.inputs.push_back({static_cast<uint32_t>(open_.messages.size()), l.isBid, *price, *size});
            if (*size)
                legacyKeys_[l.isBid][*key] = *price;
            else
                legacyKeys_[l.isBid].erase(*key);
        }
    } catch (const std::overflow_error &e) {
        try {
            halt(e.what());
        } catch (const std::exception &error) {
            fail(error.what(), clock);
        }
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("unknown exact source failure", clock);
    }
}
void LevelRecorder::onSuppressed(const std::string &symbol, int64_t clock, size_t index) noexcept {
    if (failed() || halted_ || !mine(symbol))
        return;
    try {
        if (index >= expectedChanges_.size()) {
            halt("missing exact source index");
            return;
        }
        if (expectedChanges_[index])
            halt("legacy suppressed exact quantity change", index);
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("suppression diagnostic failure", clock);
    }
}
void LevelRecorder::onRejectedSource(const std::string &symbol, int64_t clock,
                                     const recording::ExactSourcePtr &owner) noexcept {
    if (failed() || halted_ || !mine(symbol) || owner->malformed)
        return;
    sourceOwner_ = owner;
    source_ = owner.get();
    if (lastClock_ == INT64_MIN)
        lastClock_ = clock;
    try {
        halt("legacy rejected valid decimal source");
    } catch (const std::exception &e) {
        fail(e.what(), clock);
    } catch (...) {
        fail("rejection diagnostic failure", clock);
    }
    source_ = nullptr;
}
} // namespace sentinel::levelstore
