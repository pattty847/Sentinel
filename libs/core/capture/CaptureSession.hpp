#pragma once
#include "RawCapture.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class QCoreApplication;

namespace sentinel::capture {

// The ingest thread only copies into this bounded queue. Compression and fsync
// run on the disk thread. Overflow/storage errors fail the capture, never evict.
class Session {
public:
    Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes = 64 * 1024 * 1024);
    ~Session();
    bool submit(Record record) noexcept;
    void fail(std::string_view error, std::optional<RecordLocation> dropped = {}) noexcept;
    void close(std::string_view reason = "session closed"); // drain, reserved stop/gap record, sync, join
    std::string error() const;
    WriterStats stats() const;
    size_t queuedBytes() const;
private:
    static constexpr size_t FinalRecordReserve = 4096;
    void failLocked(std::string_view error, RecordLocation dropped);
    Record finalRecord();
    void run(WriterConfig config, nlohmann::json metadata);
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Record> m_queue;
    size_t m_bytes = 0, m_limit;
    bool m_stopping = false;
    std::string m_error;
    Record m_stopRecord;
    bool m_haveStop = false;
    uint64_t m_lastConnection = 0;
    std::optional<RecordLocation> m_firstDropped;
    WriterStats m_stats;
    std::thread m_thread;
};

int runApplication(QCoreApplication& application);
} // namespace sentinel::capture
