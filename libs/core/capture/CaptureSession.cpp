#include "CaptureSession.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <stdexcept>

namespace sentinel::capture {
Session::Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes) : m_limit(queueBytes) {
    if (queueBytes < 2 * FinalRecordReserve || queueBytes > 1024ULL * 1024 * 1024)
        throw std::runtime_error("invalid queue capacity");
    m_error.reserve(512);
    m_stopRecord.kind = Kind::CaptureStopped;
    m_stopRecord.payload.reserve(FinalRecordReserve);
    m_thread = std::thread([this, config = std::move(config), metadata = std::move(metadata)]() mutable {
        run(std::move(config), std::move(metadata));
    });
}
Session::~Session() { close(); }
void Session::failLocked(std::string_view error, RecordLocation dropped) {
    if (m_error.empty()) m_error.assign(error.substr(0, 512));
    // A write failure can reveal an older uncommitted frame after the producer
    // has already reported queue overflow. Preserve the earliest lost position.
    if (!m_firstDropped || (dropped.kind == Kind::Frame && m_firstDropped->kind != Kind::Frame) ||
        (dropped.kind == m_firstDropped->kind && dropped.time.steadyNs < m_firstDropped->time.steadyNs))
        m_firstDropped = dropped;
}
bool Session::submit(Record record) noexcept {
    const RecordLocation location{record.time, record.connection, record.kind};
    try {
        const auto size = record.payload.capacity() + sizeof(Record) + 64;
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
            if (record.payload.size() > MaxRecordBytes || size > m_limit - FinalRecordReserve - m_bytes) {
                failLocked("capture queue/record limit exceeded", location);
                m_wake.notify_one();
                return false;
            }
            m_queue.push_back(std::move(record)); m_bytes += size;
        }
        m_wake.notify_one(); return true;
    } catch (const std::exception& e) { fail(e.what(), location); return false; }
}
void Session::fail(std::string_view error, std::optional<RecordLocation> dropped) noexcept {
    std::lock_guard lock(m_mutex);
    failLocked(error, dropped.value_or(RecordLocation{Stamp::now(), m_lastConnection, Kind::EngineError}));
    m_wake.notify_one();
}
std::string Session::error() const { std::lock_guard lock(m_mutex); return m_error; }
WriterStats Session::stats() const { std::lock_guard lock(m_mutex); return m_stats; }
size_t Session::queuedBytes() const { std::lock_guard lock(m_mutex); return m_bytes; }
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
    std::optional<RecordLocation> current;
    bool finalAttempted = false;
    try {
        writer = std::make_unique<Writer>(std::move(config), std::move(metadata));
        while (true) {
            std::optional<Record> next;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(25), [&] { return m_stopping || !m_queue.empty(); });
                if (!m_queue.empty()) {
                    next.emplace(std::move(m_queue.front())); m_queue.pop_front();
                    m_bytes -= next->payload.capacity() + sizeof(Record) + 64;
                } else if (m_stopping) break;
            }
            if (next) {
                current = RecordLocation{next->time, next->connection, next->kind};
                writer->append(*next);
            }
            // Backlogged receive stamps may already be old. Let append() group
            // them by receive time/size, not one fsync per old queued frame.
            else writer->flushDue(Stamp::now().steadyNs);
            { std::lock_guard lock(m_mutex); m_stats = writer->stats(); }
        }
        auto terminal = finalRecord();
        current = RecordLocation{terminal.time, terminal.connection, terminal.kind};
        finalAttempted = true;
        writer->append(terminal);
        writer->close();
        { std::lock_guard lock(m_mutex); m_stats = writer->stats(); }
    } catch (const std::exception& e) {
        sLog_Error("Capture disk worker failed: error=" << e.what());
        const auto uncommitted = writer ? writer->firstUncommitted() : std::nullopt;
        fail(e.what(), uncommitted ? uncommitted : current);
        {
            std::unique_lock lock(m_mutex);
            // The app stops/joins the producer before close(), so the terminal
            // record contains the final connection and first dropped frame.
            m_wake.wait(lock, [&] { return m_stopping; });
            for (const auto& queued : m_queue)
                failLocked(m_error, {queued.time, queued.connection, queued.kind});
            m_queue.clear(); m_bytes = 0;
            // finalRecord() may have moved this slot before a failed final sync.
            if (finalAttempted) {
                m_stopRecord.kind = Kind::CaptureStopped;
                m_stopRecord.time = Stamp::now();
                m_stopRecord.connection = m_lastConnection;
            }
        }
        try {
            if (!writer) throw std::runtime_error("writer initialization failed");
            writer->abandonSegment();
            writer->append(finalRecord());
            writer->close();
            { std::lock_guard lock(m_mutex); m_stats = writer->stats(); }
            sLog_Warning("Capture failure marker persisted in a new segment");
        } catch (const std::exception& markerError) {
            // A removed/full/broken volume may reject even the reserved marker.
            // Keep the nonzero exit and state the persistence failure explicitly.
            sLog_Error("Capture failure marker could not be persisted: error=" << markerError.what());
        }
    }
}
} // namespace sentinel::capture
