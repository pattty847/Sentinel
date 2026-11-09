#include "JournalFeed.hpp"
#include "SentinelLogging.hpp"
#include <chrono>
#include <deque>

namespace sentinel::roller {
JournalFeed::TradeSeedStats JournalFeed::seedTradeIds(
    const std::vector<JournalFile>& files, const JournalPos& cursor, bool inclusive) {
    const auto started = std::chrono::steady_clock::now();
    TradeSeedStats stats;
    std::vector<std::string> reversed;
    reversed.reserve(TradeIdWindowCapacity);
    size_t end = 0;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].header.at("run_id") == cursor.run &&
            files[i].header.at("first_block_ordinal").get<uint64_t>() <= cursor.block) end = i + 1;
    if (!end || cursor.product != product_) {
        stats.degraded = true;
        end = 0;
        sLog_Warning("Journal trade seed incomplete product=" << product_ << " reason=cursor_unavailable");
    }
    const auto warn = [&](const JournalFile& file, const std::string& reason) {
        stats.degraded = true;
        sLog_Warning("Journal trade seed incomplete product=" << product_
                     << " file=" << file.path.string() << " reason=" << reason);
    };
    for (size_t i = end; i > 0 && reversed.size() < TradeIdWindowCapacity; --i) {
        const auto& file = files[i - 1];
        const auto run = file.header.at("run_id").get<std::string>();
        const auto segment = file.header.at("segment").get<uint64_t>();
        if (i < end && files[i].header.at("run_id") == run &&
            files[i].header.at("segment").get<uint64_t>() != segment + 1)
            warn(file, "missing earlier segment");
        if (i == 1 && segment != 0) warn(file, "journal prefix missing");
        // Keep only the required suffix of this segment, then visit it backwards.
        // Memory is bounded by two windows plus the existing one-frame parser.
        const size_t needed = TradeIdWindowCapacity - reversed.size();
        std::deque<std::string> suffix;
        ++stats.segments;
        uint64_t segmentDecoded = 0;
        try {
            capture::RecordReader reader(QString::fromStdString(file.path.string()),
                !file.superseded, {}, [&](const capture::BlockIndex&, const char* reason) {
                    warn(file, reason);
                });
            capture::Record record;
            bool stoppedAtCursor = false;
            while (reader.next(record)) {
                const auto& result = reader.result();
                segmentDecoded = result.decodedRecords;
                if (i == end) {
                    const auto pos = std::pair(result.recordOrdinal, result.recordIndex);
                    const auto limit = std::pair(cursor.block, cursor.record);
                    if (pos > limit || (!inclusive && pos == limit)) { stoppedAtCursor = true; break; }
                }
                if (record.kind != capture::Kind::Frame) continue;
                try {
                    const auto frame = nlohmann::json::parse(record.payload);
                    if (frame.value("channel", "") != "market_trades") continue;
                    collectTrades(frame, record.time.systemNs / 1'000'000);
                    for (const auto& pending : frameTrades_) {
                        if (suffix.size() == needed) suffix.pop_front();
                        suffix.push_back(pending.trade.trade_id);
                    }
                } catch (const nlohmann::json::exception&) {
                    // Malformed frames do not enter the continuous window either.
                } catch (const std::runtime_error&) {
                }
            }
            segmentDecoded = reader.result().decodedRecords;
            if (!stoppedAtCursor && (reader.result().tornTail || (file.superseded && !reader.result().indexed)))
                warn(file, "incomplete earlier segment");
        } catch (const std::exception& e) {
            warn(file, e.what()); // Retain the readable prefix; seeding never aborts the roll.
        }
        stats.decodedRecords += segmentDecoded;
        for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) reversed.push_back(std::move(*it));
    }
    tradeWindow_.clear();
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) rememberTradeId(*it);
    frameTrades_.clear();
    stats.records = reversed.size();
    stats.ids = retainedTradeIds();
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    sLog_Data("Journal trade seed product=" << product_ << " records=" << stats.records
              << " ids=" << stats.ids << " segments=" << stats.segments << " decodedRecords=" << stats.decodedRecords
              << " ms=" << stats.ms << " degraded=" << stats.degraded);
    return stats;
}
} // namespace sentinel::roller
