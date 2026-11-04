#pragma once
// Durable hourly LevelOutput. One externally serialized writer per product; readers
// may scan between append/sync calls. Cutback/recovery require exclusive access.
#include "LevelRecorder.hpp"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <variant>

namespace sentinel::levelstore {
// Throwing native operations, virtual only to exercise failures and process kills.
// A short write is a failure, never retried. No operation may silently swallow I/O errors.
class LevelLogFiles {
  public:
    virtual ~LevelLogFiles() = default;
    virtual void createDirectory(const std::filesystem::path &);
    virtual std::FILE *openAppend(const std::filesystem::path &);
    virtual size_t write(std::FILE *, std::span<const uint8_t>);
    virtual void close(std::FILE *);
    virtual void sync(const std::filesystem::path &, bool directory);
    virtual void truncate(const std::filesystem::path &, uint64_t size);
};
struct LevelLogRecord {
    DayBinding binding;
    std::variant<BookImage, MinuteBlock> value;
    int64_t minute() const;
};
class LevelLogReader {
  public:
    LevelLogReader(std::filesystem::path root, std::string product);
    // Snapshot scan in file/record order; checks CRC before canonical decoding.
    // A torn terminal frame is withheld, never modified. Other damage throws.
    void read(const std::function<void(const LevelLogRecord &)> &) const;
    // The latest day-start image at/before the hour, whose binding covers it.
    std::optional<LevelLogRecord> imageForHour(int64_t hourMs) const;
    static std::filesystem::path hourPath(const std::filesystem::path &root,
                                          const std::string &product, int64_t minute);
  private:
    std::filesystem::path root_;
    std::string product_;
};
class LevelLog final : public LevelOutput {
  public:
    // Construction does no I/O. open() is explicit, or lazy on the first operation;
    // even an open failure leaves an observable poisoned instance.
    LevelLog(std::filesystem::path root, std::string product,
             std::shared_ptr<LevelLogFiles> files = std::make_shared<LevelLogFiles>());
    ~LevelLog();
    LevelLog(const LevelLog &) = delete;
    LevelLog &operator=(const LevelLog &) = delete;
    void open();
    void onDayStart(const DayBinding &, const BookImage &) override;
    void onBlock(const DayBinding &, const MinuteBlock &) override;
    void sync();
    // Removes records at/after this aligned minute, including images at/after it.
    // Earlier images survive. Reappending at a removed image requires onDayStart.
    // Durable newest-file-first truncations; interrupted cutback leaves a prefix.
    void truncateAfter(int64_t minute);
    bool failed() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sentinel::levelstore
