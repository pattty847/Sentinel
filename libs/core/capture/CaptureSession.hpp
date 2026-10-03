#pragma once
#include "RawCapture.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sentinel::capture {

// Optional dependency seam for deterministic storage/queue fault tests. Called
// only by the disk worker; production uses empty hooks.
struct SessionHooks {
    std::function<void()> beforeDrain;
    std::function<void(const std::string&, std::string_view, const Record*)> beforeWriterOperation;
    // After a failed worker detached its backlog (session mutex released), before
    // the scan/free/release. Argument: detached record count.
    std::function<void(size_t)> backlogDetached;
};
struct ProductCapture { WriterConfig config; nlohmann::json metadata; };

// One process-wide capture queue pool shared by every product's session
// (per-symbol connections plan, owner decision 7). It is accounting only: a
// session's queue allocates per record as frames arrive, so an idle capture
// holds ~0 bytes and nothing is reserved up front. Each product may always use
// its floor; bytes above the floor come from the shared remainder
// (total - products x floor), first come first served. One product flooding the
// remainder therefore cannot refuse another product's frames below its floor.
// The mutex guards arithmetic only (never held across I/O or allocation);
// used() reads relaxed atomic mirrors, so /metrics never waits on a disk worker.
class QueuePool {
public:
    QueuePool(size_t totalBytes, size_t floorBytes, size_t products);
    bool reserve(size_t product, size_t bytes) noexcept;
    void release(size_t product, size_t bytes) noexcept;
    size_t used(size_t product) const noexcept { return m_productUsed[product].load(std::memory_order_relaxed); }
    size_t used() const noexcept { return m_totalUsed.load(std::memory_order_relaxed); }
    size_t total() const noexcept { return m_total; }
    size_t floor() const noexcept { return m_floor; }
    size_t products() const noexcept { return m_productUsed.size(); }
private:
    size_t above(size_t used) const noexcept { return used > m_floor ? used - m_floor : 0; }
    const size_t m_total, m_floor, m_sharedLimit;
    std::mutex m_mutex;
    size_t m_sharedUsed = 0;
    std::vector<size_t> m_used;
    std::vector<std::atomic<size_t>> m_productUsed;
    std::atomic<size_t> m_totalUsed{0};
};

// One product, one connection, one RAWL2 v1 stream. The ingest thread only
// copies into this bounded queue; compression and fsync run on the disk
// thread. Overflow/storage errors fail the capture, never evict.
class Session {
public:
    // Standalone: a private pool of queueBytes, minus the stop-record reserve.
    Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes = 64 * 1024 * 1024, SessionHooks hooks = {});
    // Shared pool: this session accounts its queue to pool slot `product`.
    Session(WriterConfig config, nlohmann::json metadata, std::shared_ptr<QueuePool> pool, size_t product, SessionHooks hooks = {});
    ~Session();
    bool submit(Record record) noexcept;
    void fail(std::string_view error, std::optional<RecordLocation> dropped = {}) noexcept;
    void close(std::string_view reason = "session closed"); // drain, reserved stop/gap record, sync, join
    std::string error() const;
    WriterStats stats() const;
    size_t queuedBytes() const noexcept { return m_pool->used(m_slot); }
    // Relaxed mirrors of stats() for /metrics samplers (no session mutex).
    uint64_t storedFrames() const noexcept { return m_storedFrames.load(std::memory_order_relaxed); }
    uint64_t storedFileBytes() const noexcept { return m_storedFileBytes.load(std::memory_order_relaxed); }
    static constexpr size_t FinalRecordReserve = 4096;
private:
    void failLocked(std::string_view error, RecordLocation dropped);
    void logFirstFailure() noexcept; // call with m_mutex NOT held
    void dropQueue(); // m_mutex NOT held
    Record finalRecord();
    void run(WriterConfig config, nlohmann::json metadata);
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Record> m_queue;
    bool m_stopping = false;
    std::string m_error;
    Record m_stopRecord;
    bool m_haveStop = false;
    uint64_t m_lastConnection = 0;
    std::optional<RecordLocation> m_firstDropped;
    // The first failure, captured under m_mutex by failLocked and logged by
    // logFirstFailure after the mutex is released (the log sink can block).
    struct FailureNote { std::string error; RecordLocation dropped; size_t queued, poolUsed; };
    std::optional<FailureNote> m_unloggedFailure;
    std::atomic<bool> m_failureLogPending{false};
    WriterStats m_stats;
    std::atomic<uint64_t> m_storedFrames{0}, m_storedFileBytes{0};
    std::string m_symbol;
    SessionHooks m_hooks;
    std::shared_ptr<QueuePool> m_pool;
    size_t m_slot = 0;
    std::thread m_thread;
};

} // namespace sentinel::capture
