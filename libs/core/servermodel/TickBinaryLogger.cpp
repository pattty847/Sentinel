#include "TickBinaryLogger.hpp"
#include "SentinelLogging.hpp"
#include "Cpp20Utils.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

TickBinaryLogger::TickBinaryLogger(const std::string& baseDir) 
    : m_baseDir(baseDir) 
{
    if (!fs::exists(m_baseDir)) {
        fs::create_directories(m_baseDir);
    }
}

TickBinaryLogger::~TickBinaryLogger() {
    flush();
}

void TickBinaryLogger::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [sym, file] : m_files) {
        if (file.stream.is_open()) {
            file.stream.flush();
        }
    }
}

void TickBinaryLogger::logTrade(const Trade& trade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Convert timestamp to ms
    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        trade.timestamp.time_since_epoch()).count();
        
    LogFile& file = getFileForSymbol(trade.product_id, ts);
    if (!file.stream.is_open()) return;
    
    LogFormat::TradePayload payload;
    payload.price = trade.price;
    payload.size = trade.size;
    payload.side = (trade.side == AggressorSide::Buy) ? 1 : 2;
    
    // Header
    LogFormat::RecordHeader header;
    header.type = LogFormat::RecordType::Trade;
    header.timestamp_ms = ts;
    header.payload_len = sizeof(payload) + trade.trade_id.length();
    
    file.stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.stream.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    if (!trade.trade_id.empty()) {
        file.stream.write(trade.trade_id.c_str(), trade.trade_id.length());
    }
}

void TickBinaryLogger::logBookUpdate(const std::string& symbol, const std::vector<BookDelta>& deltas) {
    if (deltas.empty()) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
        
    LogFile& file = getFileForSymbol(symbol, ts);
    if (!file.stream.is_open()) return;
    
    LogFormat::RecordHeader header;
    header.type = LogFormat::RecordType::BookUpdate;
    header.timestamp_ms = ts;
    header.payload_len = deltas.size() * sizeof(LogFormat::BookDeltaPayload);
    
    file.stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    for (const auto& d : deltas) {
        LogFormat::BookDeltaPayload p;
        p.is_bid = d.isBid ? 1 : 0;
        p.index = d.idx;
        p.quantity = static_cast<float>(d.qty); // Downcast to float
        file.stream.write(reinterpret_cast<const char*>(&p), sizeof(p));
    }
}

TickBinaryLogger::LogFile& TickBinaryLogger::getFileForSymbol(const std::string& symbol, uint64_t timestamp_ms) {
    LogFile& file = m_files[symbol];
    
    // Simple hourly rotation
    uint64_t hour = timestamp_ms / (1000 * 3600);
    
    if (!file.stream.is_open() || file.currentHour != hour) {
        rotateFile(file, symbol, timestamp_ms);
    }
    
    return file;
}

void TickBinaryLogger::rotateFile(LogFile& file, const std::string& symbol, uint64_t timestamp_ms) {
    if (file.stream.is_open()) {
        file.stream.close();
    }
    
    using namespace std::chrono;
    auto tp = system_clock::time_point(milliseconds(timestamp_ms));
    auto day_tp = floor<days>(tp);
    auto ymd = year_month_day{day_tp};
    
    std::stringstream dateDirSS;
    dateDirSS << static_cast<int>(ymd.year()) << "-" 
              << std::setfill('0') << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
              << std::setw(2) << static_cast<unsigned>(ymd.day());
              
    fs::path symbolDir = fs::path(m_baseDir) / symbol;
    fs::path dayDir = symbolDir / dateDirSS.str();
    
    if (!fs::exists(dayDir)) {
        fs::create_directories(dayDir);
    }
    
    uint64_t hour = timestamp_ms / (1000 * 3600);
    int hourOfDay = (timestamp_ms / 1000 / 3600) % 24;
    
    std::stringstream fileName;
    fileName << std::setfill('0') << std::setw(2) << hourOfDay << ".bin";
    
    fs::path filePath = dayDir / fileName.str();
    
    bool isNew = !fs::exists(filePath);
    
    file.stream.open(filePath, std::ios::binary | std::ios::app);
    file.currentPath = filePath.string();
    file.currentHour = hour;
    
    if (isNew) {
        LogFormat::FileHeader fh;
        fh.created_at_ms = timestamp_ms;
        size_t len = std::min(symbol.length(), sizeof(fh.symbol) - 1);
        std::memcpy(fh.symbol, symbol.c_str(), len);
        fh.symbol[len] = '\0';
        
        file.stream.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    }
    
    sLog_Data("Rotated log file for " << symbol << " to " << file.currentPath);
}
