#include "FlowChunkCache.hpp"
namespace sentinel::flow {
FlowChunkCache::Body FlowChunkCache::read(const Key& key) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (key.channel == "flow.summary") {
        auto rows = key; rows.channel = "flow.rows"; rows.priceStep = 1;
        const auto row = entries_.find(rows);
        if (row != entries_.end() && !row->second.value.hidden && !row->second.value.stale &&
            (it == entries_.end() || it->second.value.hidden ||
             (it->second.value.stale && !row->second.value.stale))) it = row;
    }
    if (it == entries_.end() || it->second.value.hidden) return {};
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return it->second.value.body;
}
std::optional<FlowChunkCache::Entry> FlowChunkCache::inspect(const Key& key) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    return it == entries_.end() ? std::nullopt : std::optional(it->second.value);
}
void FlowChunkCache::put(const Key& key, Body body, uint64_t epoch, uint64_t hash) {
    if (!body) return;
    std::lock_guard lock(mutex_);
    if (auto it = entries_.find(key); it != entries_.end()) {
        bytes_ -= it->second.value.body->storageBytes;
        lru_.erase(it->second.lru); entries_.erase(it);
    }
    lru_.push_front(key); bytes_ += body->storageBytes;
    entries_.emplace(key, Node{{std::move(body), epoch, hash, false, false}, lru_.begin()}); evict();
}
bool FlowChunkCache::confirm(const Key& key, uint64_t hash, uint64_t epoch) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.value.hash != hash) return false;
    auto& e = it->second.value; e.epoch = epoch; e.stale = e.hidden = false;
    return true;
}
bool FlowChunkCache::hide(const Key& key) {
    std::lock_guard lock(mutex_); const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.value.hidden) return false;
    it->second.value.hidden = true; return true;
}
std::vector<FlowChunkCache::Key> FlowChunkCache::apply(const protocol::chunks::Availability& a) {
    std::lock_guard lock(mutex_); std::vector<Key> hidden;
    for (auto& [key, node] : entries_) {
        if (key.instrument != a.instrument) continue;
        auto& e = node.value; e.stale = e.epoch != a.epoch(key);
        if (!a.contains(key) && !e.hidden) { e.hidden = true; hidden.push_back(key); }
        // Returning to sealed requires a fresh response before unhiding.
    }
    return hidden;
}
void FlowChunkCache::wanted(std::set<Key> keys) { std::lock_guard lock(mutex_); wanted_ = std::move(keys); evict(); }
void FlowChunkCache::clear() { std::lock_guard lock(mutex_); entries_.clear(); lru_.clear(); bytes_ = 0; }
void FlowChunkCache::setBudget(size_t bytes) { std::lock_guard lock(mutex_); budget_ = bytes; evict(); }
FlowChunkCache::Stats FlowChunkCache::stats() const {
    std::lock_guard lock(mutex_); size_t wanted = 0;
    for (const auto& [key, node] : entries_) if (wanted_.contains(key)) wanted += node.value.body->storageBytes;
    return {bytes_, entries_.size(), budget_, wanted};
}
void FlowChunkCache::evict() {
    for (auto it = lru_.end(); bytes_ > budget_ && it != lru_.begin();) {
        --it; if (wanted_.contains(*it)) continue;
        const auto entry = entries_.find(*it); bytes_ -= entry->second.value.body->storageBytes;
        entries_.erase(entry); it = lru_.erase(it);
    }
}
}
