#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <unordered_map>
#include "../marketdata/model/TradeData.h"

// Simple binary format constants
namespace LogFormat {
    constexpr uint32_t MAGIC = 0x53454E54; // "SENT"
    constexpr uint16_t VERSION = 1;
    
    enum class RecordType : uint8_t {
        Trade = 1,
        BookUpdate = 2,
        BookSnapshot = 3
    };
    
    #pragma pack(push, 1)
    struct FileHeader {
        uint32_t magic = MAGIC;
        uint16_t version = VERSION;
        uint64_t created_at_ms = 0;
        char symbol[16] = {0};
    };
    
    struct RecordHeader {
        RecordType type;
        uint64_t timestamp_ms; // Exchange timestamp
        uint32_t payload_len;
    };
    
    struct TradePayload {
        double price;
        double size;
        uint8_t side; // 1=Buy, 2=Sell
        // trade_id is variable length string, stored after fixed payload
    };
    
    struct BookDeltaPayload {
        uint8_t is_bid;
        uint32_t index; // Relative index in LiveOrderBook
        float quantity; // Use float for space saving? Or double? Let's use float for deltas as mostly just for visuals/replay
    };
    #pragma pack(pop)
}

class TickBinaryLogger {
public:
    explicit TickBinaryLogger(const std::string& baseDir = "data/market");
    ~TickBinaryLogger();

    void logTrade(const Trade& trade);
    void logBookUpdate(const std::string& symbol, const std::vector<BookDelta>& deltas);
    
    // Flush all open files
    void flush();

private:
    std::string m_baseDir;
    std::mutex m_mutex;
    
    struct LogFile {
        std::ofstream stream;
        std::string currentPath;
        uint64_t currentHour = 0;
    };
    
    std::unordered_map<std::string, LogFile> m_files;
    
    LogFile& getFileForSymbol(const std::string& symbol, uint64_t timestamp_ms);
    void rotateFile(LogFile& file, const std::string& symbol, uint64_t timestamp_ms);
};
