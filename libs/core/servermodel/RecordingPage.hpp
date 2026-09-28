#pragma once
#include "Hmc2Store.hpp"

namespace recording {
struct BuildRequest {
    std::string symbol;
    int64_t tfMs = 60'000;
    int64_t endMs = 0; // inclusive output bucket; zero selects latest
    uint32_t count = 1024;
    double priceLo = 0, priceHi = 0;
    uint32_t rows = 2048;
    std::optional<double> displayTick;
    ReadLimits budgets{32'768, 100'000'000, 5'000};
};
struct PriceBand {
    double lo = 0, tick = 0;
    uint32_t rows = 0;
};
struct ServedColumn {
    int64_t bucketStartMs = 0;
    uint64_t observedMs = 0;
    uint32_t flags = 0;
    std::vector<uint16_t> cells, quantities; // descending, lower-edge price convention
    double quantityScale = 0;
    std::vector<uint8_t> validity; // bit i (LSB first) corresponds to cells[i]
};
enum class BuildStatus { Complete, Budget, Cancelled, InvalidRequest, IncompatibleGrid, IoError };
struct BuildResult {
    std::vector<ServedColumn> columns; // ascending time
    PriceBand band;
    SizeScale sizeScale;
    std::string layer;
    int64_t scannedStartMs = 0, scannedEndMs = 0, nextEnd = 0;
    bool exhausted = false;
    std::optional<int64_t> oldestAvailableMs, latestAvailableMs; // output bucket starts
    BuildStatus status = BuildStatus::Complete;
    uint64_t sourceRecords = 0, entriesVisited = 0;
    std::string message;
};
// Worker-owned reader sessions reuse the metadata/availability cache across pages.
// Root overload is a one-shot convenience. No network or GUI work occurs here.
BuildResult buildPage(Hmc2Reader &reader, const BuildRequest &request, StopToken stop = {});
BuildResult buildPage(const std::filesystem::path &root, const BuildRequest &request, StopToken stop = {});
} // namespace recording
