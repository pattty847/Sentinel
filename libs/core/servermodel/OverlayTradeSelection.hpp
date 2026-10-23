#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class OverlayTradeBudget { Reject, Newest };

// Bounded selection from a tape whose arrival order can differ from exchange time.
// No sorting is needed until the caller has released the tape lock.
template<class Trade> class OverlayTradeSelection {
public:
    OverlayTradeSelection(int64_t startMs, int64_t endMs, size_t limit,
                          std::vector<Trade>& out, OverlayTradeBudget budget)
        : startMs_(startMs), endMs_(endMs), limit_(limit), out_(out), budget_(budget) {
        out_.clear();
    }
    bool add(const Trade& trade) {
        if (retainedFromMs_ == 0 || trade.timestampMs < retainedFromMs_)
            retainedFromMs_ = trade.timestampMs;
        if (trade.timestampMs < startMs_ || trade.timestampMs >= endMs_) return true;
        if (out_.size() < limit_) { out_.push_back(trade); return true; }
        if (budget_ == OverlayTradeBudget::Reject) { out_.clear(); return false; }
        if (!limited_) std::make_heap(out_.begin(), out_.end(), newer);
        limited_ = true;
        int64_t droppedMs = trade.timestampMs;
        if (!out_.empty() && trade.timestampMs > out_.front().timestampMs) {
            droppedMs = out_.front().timestampMs;
            std::pop_heap(out_.begin(), out_.end(), newer);
            out_.back() = trade;
            std::push_heap(out_.begin(), out_.end(), newer);
        }
        // Include the candle containing the last dropped timestamp, even when
        // it lies exactly on a candle boundary or ties a kept trade's timestamp.
        droppedThroughMs_ = std::max(droppedThroughMs_, droppedMs + 1);
        return true;
    }
    void finish() {
        std::stable_sort(out_.begin(), out_.end(), [](const Trade& a, const Trade& b) {
            return a.timestampMs < b.timestampMs;
        });
    }
    int64_t retainedFromMs() const {
        return limited_ ? std::max(retainedFromMs_, droppedThroughMs_) : retainedFromMs_;
    }
    bool budgetExceeded() const { return limited_; }
private:
    static bool newer(const Trade& a, const Trade& b) { return a.timestampMs > b.timestampMs; }
    int64_t startMs_, endMs_;
    size_t limit_;
    std::vector<Trade>& out_;
    OverlayTradeBudget budget_;
    int64_t retainedFromMs_ = 0, droppedThroughMs_ = 0;
    bool limited_ = false;
};
