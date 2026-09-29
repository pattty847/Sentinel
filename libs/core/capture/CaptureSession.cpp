#include "CaptureSession.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace sentinel::capture {
Session::Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes) : m_limit(queueBytes) {
    if (queueBytes < 1024 || queueBytes > 1024ULL * 1024 * 1024) throw std::runtime_error("invalid queue capacity");
    m_thread = std::thread([this, config = std::move(config), metadata = std::move(metadata)]() mutable {
        run(std::move(config), std::move(metadata));
    });
}
Session::~Session() { close(); }
bool Session::submit(Record record) noexcept {
    try {
        const auto size = record.payload.capacity() + sizeof(Record) + 64;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || !m_error.empty()) return false;
            if (record.payload.size() > MaxRecordBytes || size > m_limit - m_bytes) {
                m_error = "capture queue/record limit exceeded; capture is incomplete";
                m_wake.notify_one();
                return false;
            }
            m_queue.push_back(std::move(record)); m_bytes += size;
        }
        m_wake.notify_one(); return true;
    } catch (const std::exception& e) { fail(e.what()); return false; }
}
void Session::fail(const std::string& error) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_error.empty()) m_error = error;
    m_wake.notify_one();
}
std::string Session::error() const { std::lock_guard lock(m_mutex); return m_error; }
WriterStats Session::stats() const { std::lock_guard lock(m_mutex); return m_stats; }
size_t Session::queuedBytes() const { std::lock_guard lock(m_mutex); return m_bytes; }
void Session::close() {
    { std::lock_guard lock(m_mutex); m_stopping = true; }
    m_wake.notify_one();
    if (m_thread.joinable()) m_thread.join();
}
void Session::run(WriterConfig config, nlohmann::json metadata) {
    sentinel::logging::setCurrentThreadName("capture-disk");
    try {
        Writer writer(std::move(config), std::move(metadata));
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
            if (next) writer.append(*next);
            // Backlogged receive stamps may already be old. Let append() group
            // them by receive time/size; wall time must not force one fsync per
            // queued frame after a temporary disk stall.
            else writer.flushDue(Stamp::now().steadyNs);
            { std::lock_guard lock(m_mutex); m_stats = writer.stats(); }
        }
        writer.close();
        { std::lock_guard lock(m_mutex); m_stats = writer.stats(); }
    } catch (const std::exception& e) {
        sLog_Error("Capture disk worker failed: error=" << e.what());
        fail(e.what());
    }
}
} // namespace sentinel::capture
