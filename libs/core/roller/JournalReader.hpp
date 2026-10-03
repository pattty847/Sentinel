#pragma once
#include "capture/RawCapture.hpp"
#include <filesystem>

namespace sentinel::roller {
struct JournalPos {
    std::string product, run;
    uint64_t block = 0;
    uint32_t record = 0;
    bool operator==(const JournalPos&) const = default;
};
void to_json(nlohmann::json& j, const JournalPos& p);
void from_json(const nlohmann::json& j, JournalPos& p);
struct JournalFile {
    std::filesystem::path path;
    nlohmann::json header;
    bool superseded = false;
};
struct JournalRecord {
    capture::Record record;
    JournalPos pos;
    nlohmann::json metadata;
    bool gapBefore = false;
    int version = 1;
};
// One product, header order (run start, run id, segment), one decoded block at
// a time. Reopen at a saved position to poll an open file; EOF is not a tick.
class JournalReader {
public:
    JournalReader(std::filesystem::path root, std::string product,
                  std::optional<JournalPos> start = {});
    bool next(JournalRecord& out);
    const std::vector<JournalFile>& files() const { return files_; }
    bool pending() const { return pending_; }
    // Latest snapshot <= receive time, including earlier hours/runs for warmup.
    std::optional<JournalPos> anchor(int64_t receiveMs) const;
private:
    std::string product_;
    std::vector<JournalFile> files_;
    size_t file_ = 0;
    std::unique_ptr<capture::RecordReader> reader_;
    std::optional<JournalPos> start_;
    bool gap_ = true, pending_ = false;
    uint64_t nextBlock_ = 0;
};
} // namespace sentinel::roller
