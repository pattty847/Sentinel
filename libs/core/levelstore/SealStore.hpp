#pragma once
#include "BalSummary.hpp"
#include <filesystem>
#include <functional>
namespace sentinel::levelstore {
struct SealedHour {
    D2Hour timeline;
    std::vector<SummaryNode> summaries;
    bool operator==(const SealedHour &) const = default;
};
class SealStore {
  public:
    using Point = std::function<void(const char *)>;
    SealStore(std::filesystem::path root, std::string product, Point point = {});
    void publish(const D2Hour &);
    SealedHour read(int64_t hour) const;
    void publishDay(int64_t day);
    void verifyDay(int64_t day) const;
    void cut(int64_t minute);
    // Whole days are retained to pin the day-start image and scan prefix.
    void prune(int64_t through, int64_t windowMs = 172800000);
    void recoverRetention();
    void requireRetained(int64_t minute) const;

  private:
    std::filesystem::path root_, dir_;
    std::string product_;
    Point point_;
    void point(const char *) const;
    void atomic(const std::filesystem::path &, std::span<const uint8_t>, const char *);
};
} // namespace sentinel::levelstore
