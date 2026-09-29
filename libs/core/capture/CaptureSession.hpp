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
    void fail(const std::string& error) noexcept;
    void close(); // drain accepted records, commit final block/index, join
    std::string error() const;
    WriterStats stats() const;
    size_t queuedBytes() const;
private:
    void run(WriterConfig config, nlohmann::json metadata);
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Record> m_queue;
    size_t m_bytes = 0, m_limit;
    bool m_stopping = false;
    std::string m_error;
    WriterStats m_stats;
    std::thread m_thread;
};

int runApplication(QCoreApplication& application);
} // namespace sentinel::capture
