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
    virtual void remove(const std::filesystem::path &);
};
struct LevelLogRecord {
    DayBinding binding;
    std::variant<BookImage, MinuteBlock> value;
    int64_t minute() const;
};
enum class CutImageAtBoundary { Remove, Keep };
struct LevelLogTail {
    DayBinding binding;
    int64_t minute = 0;
    bool isImage = false;
    bool operator==(const LevelLogTail &) const = default;
};
class LevelLogReader {
  public:
    LevelLogReader(std::filesystem::path root, std::string product);
    // Snapshot scan in file/record order; checks CRC before canonical decoding.
    // A torn terminal frame is withheld, never modified. Other damage throws.
    void read(const std::function<void(const LevelLogRecord &)> &) const;
    // The latest day-start image in/before the hour, whose binding overlaps it.
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
    // Anchor recovery: call on a fresh instance BEFORE open(). Validates only the
    // retained prefix, discards the suffix, syncs it and leaves this writer open.
    // Also valid on an already-open, healthy writer. A failed instance stays poisoned.
    // Remove (default) drops images at the cut too; replay must emit them again.
    // Keep requires a valid image exactly at the cut and retains it, but no block
    // at/after the cut. Use when the restored consumer has already emitted that image.
    // Corruption before the proven cut boundary always throws without removing data.
    void truncateAfter(int64_t minute, CutImageAtBoundary image = CutImageAtBoundary::Remove);
    // Last record established durable by this instance's successful open/cut/sync.
    // Appends alone do not advance it. Throws before open or after poisoning.
    // There is no persisted fence marker: open() adopts and syncs the valid prefix;
    // an anchor restart must cut first, not infer the old fence from this summary.
    std::optional<LevelLogTail> durableTail() const;
    bool failed() const;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sentinel::levelstore
