#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sentinel::roller {
// Membership in the preceding Capacity trade occurrences, including duplicates
// and empty IDs. Empty IDs occupy time in the window but never deduplicate.
class TradeRecordWindow {
public:
    static constexpr size_t Capacity = 16'384;
    TradeRecordWindow() : ring_(Capacity) {
        counts_.reserve(Capacity);
        spare_.reserve(Capacity);
        // Duplicate-heavy windows shrink and regrow; recycle a fixed node
        // pool so neither transition allocates on the delivery path.
        for (size_t i = 0; i < Capacity; ++i) {
            counts_.emplace("", 0);
            spare_.push_back(counts_.extract(""));
        }
    }
    TradeRecordWindow(const TradeRecordWindow& other) : TradeRecordWindow() {
        const size_t first = other.size_ == Capacity ? other.next_ : 0;
        for (size_t i = 0; i < other.size_; ++i)
            remember(other.ring_[(first + i) % Capacity]);
    }
    TradeRecordWindow& operator=(const TradeRecordWindow& other) {
        if (this != &other) { TradeRecordWindow copy(other); *this = std::move(copy); }
        return *this;
    }
    TradeRecordWindow(TradeRecordWindow&&) = default;
    TradeRecordWindow& operator=(TradeRecordWindow&&) = default;
    void clear() {
        while (!counts_.empty()) spare_.push_back(counts_.extract(counts_.begin()));
        for (auto& id : ring_) id.clear();
        next_ = size_ = 0;
    }
    bool remember(const std::string& id) {
        const bool fresh = id.empty() || !counts_.contains(id);
        auto& old = ring_[next_];
        if (!old.empty()) {
            const auto it = counts_.find(old);
            if (--it->second == 0) spare_.push_back(counts_.extract(it));
        }
        if (!id.empty()) {
            auto it = counts_.find(id);
            if (it != counts_.end()) ++it->second;
            else {
                auto node = std::move(spare_.back());
                spare_.pop_back();
                node.key() = id;
                node.mapped() = 1;
                counts_.insert(std::move(node));
            }
        }
        old = id;
        next_ = (next_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
        return fresh;
    }
    size_t uniqueIds() const { return counts_.size(); }
    std::unordered_set<std::string> ids() const {
        std::unordered_set<std::string> result;
        result.reserve(counts_.size());
        for (const auto& [id, count] : counts_) result.insert(id);
        return result;
    }
private:
    std::vector<std::string> ring_;
    std::unordered_map<std::string, size_t> counts_;
    std::vector<decltype(counts_)::node_type> spare_;
    size_t next_ = 0, size_ = 0;
};
} // namespace sentinel::roller
