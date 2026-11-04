#pragma once
#include "LevelLog.hpp"

namespace sentinel::levelstore::log_detail {
namespace fs = std::filesystem;
constexpr int64_t kHour = 3'600'000;
constexpr uint32_t kMaxPayload = 512 * 1024 * 1024;
constexpr size_t kHeader = 12;
struct Position {
    fs::path path;
    uint64_t offset = 0;
};
struct ScanResult {
    std::optional<Position> tail;
    std::optional<DayBinding> binding;
    std::optional<int64_t> lastMinute;
    std::optional<LevelLogTail> last;
};
struct CutFile {
    Position at;
    uint64_t oldSize = 0;
};
struct CutPlan {
    ScanResult retained;
    std::vector<CutFile> files;
};
CutPlan planCut(const fs::path &, const std::string &, int64_t, CutImageAtBoundary);
void check(bool, const std::string &);
void validateProduct(const std::string &);
void validateRecord(const LevelLogRecord &);
void advance(ScanResult &, const LevelLogRecord &);
std::vector<fs::path> paths(const fs::path &root, const std::string &product);
ScanResult scan(const fs::path &, const std::string &,
                const std::function<void(const LevelLogRecord &, const Position &)> &);
ByteWriter payload(const LevelLogRecord &);
ByteWriter header(uint8_t type, size_t length);
} // namespace sentinel::levelstore::log_detail
