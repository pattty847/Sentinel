#include "CaptureSession.hpp"
#include "CaptureRouting.hpp"
#include "SentinelLogging.hpp"
#include <algorithm>
#include <QUuid>
#include <stdexcept>

namespace sentinel::capture {
Session::Session(WriterConfig config, nlohmann::json metadata, size_t queueBytes)
    : Session(std::vector<ProductCapture>{{std::move(config), std::move(metadata)}}, queueBytes) {}
Session::Session(std::vector<ProductCapture> products, size_t queueBytes, SessionHooks hooks)
    : m_limit(queueBytes), m_hooks(std::move(hooks)) {
    if (queueBytes < 2 * FinalRecordReserve || queueBytes > 1024ULL * 1024 * 1024)
        throw std::runtime_error("invalid queue capacity");
    if (products.empty() || products.size() > MaxProducts) throw std::runtime_error("invalid product count");
    std::sort(products.begin(), products.end(), [](const auto& a, const auto& b) { return a.config.symbol < b.config.symbol; });
    std::vector<std::string> symbols;
    for (const auto& product : products) {
        validateSymbol(product.config.symbol);
        if (!symbols.empty() && symbols.back() == product.config.symbol) throw std::runtime_error("duplicate capture product");
        symbols.push_back(product.config.symbol);
    }
    if (products.size() > 1) {
        const auto run = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto started = Stamp::now().systemNs;
        for (auto& product : products) {
            product.metadata["run_id"] = run;
            product.metadata["run_started_system_ns"] = started;
            product.metadata["connection_products"] = symbols;
            product.metadata["routing"] = RoutingId;
        }
    }
    m_error.reserve(512);
    m_stopRecord.kind = Kind::CaptureStopped;
    m_stopRecord.payload.reserve(FinalRecordReserve);
    m_thread = std::thread([this, products = std::move(products)]() mutable { run(std::move(products)); });
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
void Session::run(std::vector<ProductCapture> products) {
    sentinel::logging::setCurrentThreadName("capture-disk");
    std::vector<std::unique_ptr<Writer>> writers;
    std::vector<std::string> symbols;
    for (const auto& product : products) symbols.push_back(product.config.symbol);
    const auto publishStats = [&] {
        WriterStats total;
        for (const auto& writer : writers) {
            const auto& stats = writer->stats();
            total.records += stats.records; total.frames += stats.frames; total.frameBytes += stats.frameBytes;
            total.blocks += stats.blocks; total.fileBytes += stats.fileBytes; total.files += stats.files;
        }
        std::lock_guard lock(m_mutex); m_stats = total;
    };
    std::vector<bool> failed(products.size(), false), closed(products.size(), false), proofWritten(products.size(), false);
    std::optional<RecordLocation> current;
    RoutingBatch batch;
    std::optional<uint64_t> expectedSequence;
    const auto operation = [&](size_t i, std::string_view name, const Record* record, auto action) {
        try {
            if (m_hooks.beforeWriterOperation) m_hooks.beforeWriterOperation(symbols[i], name, record);
            action();
        } catch (...) { failed[i] = true; throw; }
    };
    const auto each = [&](auto action) {
        std::exception_ptr first;
        for (size_t i = 0; i < writers.size(); ++i) if (!failed[i] && !closed[i]) {
            try { action(i); } catch (...) { if (!first) first = std::current_exception(); }
        }
        if (first) std::rethrow_exception(first);
    };
    const auto finishBatch = [&] {
        if (batch.empty()) return;
        each([&](size_t i) {
            const Record proof{Kind::FrameReference, batch.last, batch.connection, batch.receipt(symbols[i]).dump()};
            operation(i, "append", &proof, [&] { writers[i]->append(proof); });
            proofWritten[i] = true;
        });
        batch.clear(); std::fill(proofWritten.begin(), proofWritten.end(), false);
    };
    const auto append = [&](const Record& record) {
        if (record.kind == Kind::TransportUp) expectedSequence = 0;
        if (writers.size() == 1 || record.kind != Kind::Frame) {
            finishBatch();
            each([&](size_t i) { operation(i, "append", &record, [&] { writers[i]->append(record); }); });
            return;
        }
        if (batch.due(record)) finishBatch();
        const auto identity = frameReceipt(record.payload, symbols);
        const auto& sequence = identity.at("sequence_num");
        if (!sequence.is_number_unsigned() || (expectedSequence && sequence.get<uint64_t>() != *expectedSequence)) {
            finishBatch();
            const Record invalidated{Kind::BookInvalidated, record.time, record.connection,
                R"({"reason":"capture connection sequence gap"})"};
            each([&](size_t i) { operation(i, "append", &invalidated, [&] { writers[i]->append(invalidated); }); });
        }
        expectedSequence = sequence.is_number_unsigned() && sequence.get<uint64_t>() != UINT64_MAX ?
            std::optional<uint64_t>(sequence.get<uint64_t>() + 1) : std::nullopt;
        const auto targets = identity.at("products").get<std::vector<std::string>>();
        batch.add(record, identity);
        each([&](size_t i) {
            if (std::binary_search(targets.begin(), targets.end(), symbols[i]))
                operation(i, "append", &record, [&] { writers[i]->append(record); });
        });
    };
    try {
        for (auto& product : products)
            writers.push_back(std::make_unique<Writer>(std::move(product.config), std::move(product.metadata)));
        if (m_hooks.beforeDrain) m_hooks.beforeDrain();
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
                append(*next);
            } else {
                if (!batch.empty() && Stamp::now().steadyNs - batch.first.steadyNs >= RoutingIntervalNs) finishBatch();
                each([&](size_t i) { operation(i, "flush", nullptr, [&] { writers[i]->flushDue(Stamp::now().steadyNs); }); });
            }
            publishStats();
        }
        finishBatch();
        const auto terminal = finalRecord();
        current = RecordLocation{terminal.time, terminal.connection, terminal.kind};
        // Complete each writer independently. A later close failure must not
        // reopen a successfully closed writer or duplicate its stop marker.
        for (size_t i = 0; i < writers.size(); ++i) {
            operation(i, "append", &terminal, [&] { writers[i]->append(terminal); });
            operation(i, "close", nullptr, [&] { writers[i]->close(); });
            closed[i] = true;
        }
        publishStats();
    } catch (const std::exception& e) {
        sLog_Error("Capture disk worker failed: error=" << e.what());
        fail(e.what(), current);
        for (size_t i = 0; i < writers.size(); ++i) if (failed[i])
            if (auto uncommitted = writers[i]->firstUncommitted()) fail(e.what(), uncommitted);
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stopping; });
            for (const auto& queued : m_queue) failLocked(m_error, {queued.time, queued.connection, queued.kind});
            m_queue.clear(); m_bytes = 0;
            m_stopRecord.kind = Kind::CaptureStopped;
            // Keep an explicitly submitted deterministic stop stamp when possible.
            if (!m_stopRecord.time.systemNs) m_stopRecord.time = Stamp::now();
            m_stopRecord.connection = m_lastConnection;
        }
        const auto terminal = finalRecord();
        for (size_t i = 0; i < writers.size(); ++i) {
            if (closed[i]) continue;
            try {
                if (failed[i]) writers[i]->abandonSegment();
                else {
                    try {
                        if (!batch.empty() && !proofWritten[i])
                            writers[i]->append({Kind::FrameReference, batch.last, batch.connection, batch.receipt(symbols[i]).dump()});
                        // Preserve healthy buffers before starting a marker segment.
                        writers[i]->flush();
                        writers[i]->sealSegment();
                    } catch (const std::exception& recoveryError) {
                        sLog_Error("Capture recovery seal failed: product=" << symbols[i] << " error=" << recoveryError.what());
                        writers[i]->abandonSegment(); // this writer also failed; peers remain sealed
                    }
                }
                writers[i]->append(terminal);
                writers[i]->close();
                sLog_Warning("Capture failure marker persisted: product=" << symbols[i]);
            } catch (const std::exception& markerError) {
                sLog_Error("Capture failure marker could not be persisted: product=" << symbols[i] << " error=" << markerError.what());
            }
        }
        if (writers.size() != products.size()) sLog_Error("Capture failure marker unavailable for uninitialized writers");
        publishStats();
    }
}
} // namespace sentinel::capture
