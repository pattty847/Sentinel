#pragma once
// One cache per process, shared across charts and timeframes. Mutex protects
// metadata/LRU; bodies are immutable. Epoch confirmation changes entry metadata,
// never a previously published body's epoch. Hidden bodies are inspectable for
// revalidation but read() cannot expose them. Wanted entries survive pressure.
#include "protocol/ChunkClientWire.hpp"
#include <list>
#include <mutex>
#include <set>
namespace sentinel::flow {
class FlowChunkCache {
public:
    using Key = protocol::chunks::Key;
    using Body = std::shared_ptr<const chunks::FlowChunk>;
    struct Entry { Body body; uint64_t epoch = 0, hash = 0; bool stale = false, hidden = false; };
    explicit FlowChunkCache(size_t budget = 256ull << 20) : budget_(budget) {}
    Body read(const Key&); // touch LRU, hidden -> nullptr
    std::optional<Entry> inspect(const Key&) const;
    void put(const Key&, Body, uint64_t epoch, uint64_t hash);
    bool confirm(const Key&, uint64_t hash, uint64_t epoch);
    bool hide(const Key&);
    // Returns keys newly hidden; stale held chunks remain readable.
    std::vector<Key> apply(const protocol::chunks::Availability&);
    void wanted(std::set<Key> keys);
    void clear();
    void setBudget(size_t bytes);
    struct Stats { size_t bytes, entries, budget, wantedBytes; };
    Stats stats() const;
private:
    mutable std::mutex mutex_;
    struct Node { Entry value; std::list<Key>::iterator lru; };
    std::map<Key, Node> entries_;
    std::list<Key> lru_;
    std::set<Key> wanted_;
    size_t bytes_ = 0, budget_;
    void evict();
};
}
