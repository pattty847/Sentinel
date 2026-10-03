#pragma once
#include "Grid.hpp"
#include "JournalFeed.hpp"

namespace sentinel::roller {
struct RollOptions {
    std::filesystem::path journalRoot, outputRoot;
    std::string product;
    int64_t fromMs = 0, toMs = 0; // [from,to), complete UTC minutes
    nlohmann::json overrides = nlohmann::json::object();
    bool dryRun = false;
    // Crash/lifecycle seam: invoked after a processed record, before a possible
    // checkpoint fence. Throw to interrupt; the next run must recover identically.
    std::function<void(uint64_t)> afterRecordForTest;
};
void validateOutputProduct(const std::filesystem::path& root, const std::string& product);
nlohmann::json roll(const RollOptions& options);
nlohmann::json diff(const std::filesystem::path& a, const std::filesystem::path& b,
                    const std::string& product, const std::string& layer, int64_t from, int64_t to,
                    int64_t tfMs = 60'000);
int64_t parseTime(const std::string& text);
int rollMain(int argc, char** argv);
int diffMain(int argc, char** argv);
} // namespace sentinel::roller
