#include "CaptureSession.hpp"
#include "SentinelLogging.hpp"
#include <stdexcept>

namespace sentinel::capture {
namespace {
// Accounting size of one queued record (payload buffer, record, deque slot).
size_t queuedSize(const Record& record) noexcept { return record.payload.capacity() + sizeof(Record) + 64; }
// Which lost position the gap marker names: a frame beats a lifecycle record;
// within one kind, the earliest receive time.
bool preferLost(const RecordLocation& candidate, const std::optional<RecordLocation>& current) {
    return !current || (candidate.kind == Kind::Frame && current->kind != Kind::Frame) ||
        (candidate.kind == current->kind && candidate.time.steadyNs < current->time.steadyNs);
}
} // namespace

QueuePool::QueuePool(size_t totalBytes, size_t floorBytes, size_t products)
    : m_total(totalBytes), m_floor(floorBytes),
      m_sharedLimit(products && floorBytes <= totalBytes / products ? totalBytes - products * floorBytes : 0),
      m_used(products, 0), m_productUsed(products) {
    if (products == 0 || products > MaxProducts) throw std::runtime_error("invalid capture queue product count");
    if (totalBytes == 0) throw std::runtime_error("invalid capture queue total");
    if (floorBytes > totalBytes / products)
        throw std::runtime_error("capture queue floors exceed the pool: products x floor > total");
}
bool QueuePool::reserve(size_t product, size_t bytes) noexcept {
    std::lock_guard lock(m_mutex);
    auto& used = m_used[product];
    if (bytes > m_total - used) return false; // also guards the addition below
    const auto extra = above(used + bytes) - above(used);
    if (extra > m_sharedLimit - m_sharedUsed) return false;
    m_sharedUsed += extra;
    used += bytes;
    m_productUsed[product].store(used, std::memory_order_relaxed);
    m_totalUsed.fetch_add(bytes, std::memory_order_relaxed);
    return true;
}
void QueuePool::release(size_t product, size_t bytes) noexcept {
    std::lock_guard lock(m_mutex);
    auto& used = m_used[product];
    bytes = std::min(bytes, used);
    m_sharedUsed -= above(used) - above(used - bytes);
    used -= bytes;
    m_productUsed[product].store(used, std::memory_order_relaxed);
    m_totalUsed.fetch_sub(bytes, std::memory_order_relaxed);
}

Session::Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes, SessionHooks hooks)
    : Session(std::move(config), std::move(metadata),
              queueBytes < 2 * FinalRecordReserve || queueBytes > 4096ULL * 1024 * 1024 ?
                  throw std::runtime_error("invalid queue capacity") :
                  std::make_shared<QueuePool>(queueBytes - FinalRecordReserve, 0, 1),
              0, std::move(hooks)) {}
Session::Session(WriterConfig config, nlohmann::json metadata, std::shared_ptr<QueuePool> pool, size_t product, SessionHooks hooks)
    : m_symbol(config.symbol), m_hooks(std::move(hooks)), m_pool(std::move(pool)), m_slot(product) {
    if (!m_pool || product >= m_pool->products()) throw std::runtime_error("invalid capture queue pool slot");
    validateSymbol(m_symbol);
    // RAWL2 v2 (one connection, several products) is read-only: never written.
    if (metadata.contains("connection_products") || metadata.contains("routing"))
        throw std::runtime_error("multi-product (RAWL2 v2) capture writing was removed");
    m_error.reserve(512);
    m_stopRecord.kind = Kind::CaptureStopped;
    m_stopRecord.payload.reserve(FinalRecordReserve);
    m_thread = std::thread([this, config = std::move(config), metadata = std::move(metadata)]() mutable {
        run(std::move(config), std::move(metadata));
    });
}
Session::~Session() { close(); }
void Session::failLocked(std::string_view error, RecordLocation dropped) {
    if (m_error.empty()) {
        m_error.assign(error.substr(0, 512));
        // Logged as soon as this mutex is released (logFirstFailure), not after
        // the app's stop/drain/join, which frozen disk I/O can block (FM-127).
        // Never logged under the mutex: the sink can stall and the ingest
        // thread would wait on this product's submit().
        m_unloggedFailure = FailureNote{m_error, dropped, m_pool->used(m_slot), m_pool->used()};
        m_failureLogPending.store(true, std::memory_order_release);
    }
    // A write failure can reveal an older uncommitted frame after the producer
    // has already reported queue overflow. Preserve the earliest lost position.
    if (preferLost(dropped, m_firstDropped)) m_firstDropped = dropped;
}
// Once per session, outside m_mutex; goes through the normal log sink.
void Session::logFirstFailure() noexcept {
    if (!m_failureLogPending.load(std::memory_order_acquire)) return;
    std::optional<FailureNote> note;
    {
        std::lock_guard lock(m_mutex);
        note.swap(m_unloggedFailure);
        m_failureLogPending.store(false, std::memory_order_relaxed);
    }
    if (!note) return;
    try {
        sLog_Error("Capture failed: product=" << m_symbol << " error=" << note->error
                   << " firstLostKind=" << static_cast<uint32_t>(note->dropped.kind) << " firstLostConn=" << note->dropped.connection
                   << " queuedBytes=" << note->queued << " poolUsedBytes=" << note->poolUsed
                   << " poolBytes=" << m_pool->total() << " floorBytes=" << m_pool->floor());
    } catch (...) {}
}
// Disk worker failed: every queued record is lost. Detach the backlog under a
// short lock, then scan and free it and return its bytes to the pool outside
// the lock: one I/O thread feeds every product, so a submit() for this product
// must never wait behind a large cleanup. The bytes go back now, not at close,
// so a failed product never holds shared capacity (or makes a healthy product
// fail for "pool limit").
void Session::dropQueue() {
    std::deque<Record> lost;
    { std::lock_guard lock(m_mutex); lost.swap(m_queue); }
    if (lost.empty()) return;
    if (m_hooks.backlogDetached) m_hooks.backlogDetached(lost.size());
    size_t bytes = 0;
    std::optional<RecordLocation> first;
    for (const auto& queued : lost) {
        bytes += queuedSize(queued);
        const RecordLocation at{queued.time, queued.connection, queued.kind};
        if (preferLost(at, first)) first = at;
    }
    lost = {}; // free the payloads before returning their bytes
    m_pool->release(m_slot, bytes);
    { std::lock_guard lock(m_mutex); failLocked(m_error, *first); }
    logFirstFailure();
}
bool Session::submit(Record record) noexcept {
    const RecordLocation location{record.time, record.connection, record.kind};
    // Runs after the lock below is released (declared first, destroyed last).
    struct LogAfterUnlock { Session& session; ~LogAfterUnlock() { session.logFirstFailure(); } } logAfterUnlock{*this};
    try {
        const auto size = queuedSize(record);
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return false;
            m_lastConnection = record.connection;
            if (record.kind == Kind::CaptureStopped) {
                // This dedicated slot is outside the data queue and remains
                // writable after overflow. close() augments it with gap details.
                m_stopRecord.time = record.time;
                m_stopRecord.connection = record.connection;
                if (record.payload.size() > FinalRecordReserve - 1024) {
                    failLocked("capture stop record exceeds reserved headroom", location);
                    m_stopRecord.payload = "{}";
                } else m_stopRecord.payload.assign(record.payload);
                m_haveStop = true;
                return true;
            }
            if (!m_error.empty()) {
                failLocked(m_error, location);
                return false;
            }
            if (record.payload.size() > MaxRecordBytes || !m_pool->reserve(m_slot, size)) {
                failLocked(m_pool->products() > 1 ? "capture queue pool limit exceeded" : "capture queue limit exceeded", location);
                m_wake.notify_one();
                return false;
            }
            try { m_queue.push_back(std::move(record)); }
            catch (...) { m_pool->release(m_slot, size); throw; }
        }
        m_wake.notify_one(); return true;
    } catch (const std::exception& e) { fail(e.what(), location); return false; }
}
void Session::fail(std::string_view error, std::optional<RecordLocation> dropped) noexcept {
    {
        std::lock_guard lock(m_mutex);
        failLocked(error, dropped.value_or(RecordLocation{Stamp::now(), m_lastConnection, Kind::EngineError}));
        m_wake.notify_one();
    }
    logFirstFailure();
}
std::string Session::error() const { std::lock_guard lock(m_mutex); return m_error; }
WriterStats Session::stats() const { std::lock_guard lock(m_mutex); return m_stats; }
void Session::close(std::string_view reason) {
    {
        std::lock_guard lock(m_mutex);
        if (!m_stopping && !m_haveStop) {
            m_stopRecord.time = Stamp::now();
            m_stopRecord.connection = m_lastConnection;
            m_stopRecord.payload = nlohmann::json({{"reason", reason.substr(0, 512)}}).dump();
            m_haveStop = true;
        }
        m_stopping = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable()) m_thread.join();
}
Record Session::finalRecord() {
    std::lock_guard lock(m_mutex);
    if (m_firstDropped) {
        const auto& lost = *m_firstDropped;
        m_stopRecord.payload = nlohmann::json({{"gap", true}, {"reason", m_error},
            {"first_dropped_system_ns", lost.time.systemNs}, {"first_dropped_steady_ns", lost.time.steadyNs},
            {"first_dropped_connection", lost.connection}, {"first_dropped_kind", static_cast<uint32_t>(lost.kind)}}).dump();
    }
    return std::move(m_stopRecord);
}
void Session::run(WriterConfig config, nlohmann::json metadata) {
    sentinel::logging::setCurrentThreadName("capture-disk");
    std::unique_ptr<Writer> writer;
    bool failed = false, closed = false;
    const auto publishStats = [&] {
        if (!writer) return;
        const auto stats = writer->stats();
        m_storedFrames.store(stats.frames, std::memory_order_relaxed);
        m_storedFileBytes.store(stats.fileBytes, std::memory_order_relaxed);
        std::lock_guard lock(m_mutex); m_stats = stats;
    };
    const auto operation = [&](std::string_view name, const Record* record, auto action) {
        try {
            if (m_hooks.beforeWriterOperation) m_hooks.beforeWriterOperation(m_symbol, name, record);
            action();
        } catch (...) { failed = true; throw; }
    };
    std::optional<RecordLocation> current;
    try {
        writer = std::make_unique<Writer>(std::move(config), std::move(metadata));
        if (m_hooks.beforeDrain) m_hooks.beforeDrain();
        while (true) {
            std::optional<Record> next;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(25), [&] { return m_stopping || !m_queue.empty(); });
                if (!m_queue.empty()) {
                    next.emplace(std::move(m_queue.front())); m_queue.pop_front();
                    m_pool->release(m_slot, queuedSize(*next));
                } else if (m_stopping) break;
            }
            if (next) {
                current = RecordLocation{next->time, next->connection, next->kind};
                operation("append", &*next, [&] { writer->append(*next); });
            } else {
                operation("flush", nullptr, [&] { writer->flushDue(Stamp::now().steadyNs); });
            }
            publishStats();
        }
        const auto terminal = finalRecord();
        current = RecordLocation{terminal.time, terminal.connection, terminal.kind};
        operation("append", &terminal, [&] { writer->append(terminal); });
        operation("close", nullptr, [&] { writer->close(); });
        closed = true;
        publishStats();
    } catch (const std::exception& e) {
        sLog_Error("Capture disk worker failed: product=" << m_symbol << " error=" << e.what());
        fail(e.what(), current);
        if (writer && failed)
            if (auto uncommitted = writer->firstUncommitted()) fail(e.what(), uncommitted);
        // m_error is set: producers are refused from here on (recorded as
        // dropped, never queued) until the app stops and calls close().
        dropQueue();
        { std::unique_lock lock(m_mutex); m_wake.wait(lock, [&] { return m_stopping; }); }
        dropQueue();
        {
            std::lock_guard lock(m_mutex);
            m_stopRecord.kind = Kind::CaptureStopped;
            // Keep an explicitly submitted deterministic stop stamp when possible.
            if (!m_stopRecord.time.systemNs) m_stopRecord.time = Stamp::now();
            m_stopRecord.connection = m_lastConnection;
        }
        const auto terminal = finalRecord();
        if (!writer) {
            sLog_Error("Capture failure marker unavailable: product=" << m_symbol << " writer was not created");
        } else if (!closed) {
            try {
                if (failed) writer->abandonSegment();
                else {
                    try {
                        // Preserve healthy buffers before starting a marker segment.
                        writer->flush();
                        writer->sealSegment();
                    } catch (const std::exception& recoveryError) {
                        sLog_Error("Capture recovery seal failed: product=" << m_symbol << " error=" << recoveryError.what());
                        writer->abandonSegment();
                    }
                }
                writer->append(terminal);
                writer->close();
                sLog_Warning("Capture failure marker persisted: product=" << m_symbol);
            } catch (const std::exception& markerError) {
                sLog_Error("Capture failure marker could not be persisted: product=" << m_symbol << " error=" << markerError.what());
            }
        }
        publishStats();
    }
}
} // namespace sentinel::capture
