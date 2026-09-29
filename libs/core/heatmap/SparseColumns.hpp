#pragma once

#include "../servermodel/RecordingCodec.hpp"
#include <array>
#include <string>

namespace heatmap {
inline constexpr int64_t kMinuteMs = 60'000, kHourMs = 3'600'000, kDayMs = 86'400'000;

struct GridIdentity {
    uint64_t configHash = 0;
    int64_t rowTickUnits = 0;
    double priceScale = 100;
};
struct SparseEntry {
    uint32_t rowSide = 0; // row relative to NativeColumn::baseRow; ask in bit 31
    uint16_t code = 0;    // original 15-bit size code, interpreted with sizeScale
    uint32_t row() const { return rowSide & 0x7fffffffu; }
    bool isAsk() const { return rowSide >> 31; }
};
struct CoverageRun {
    int64_t lo = 0, hi = 0; // inclusive absolute native rows, sorted/disjoint per side
    uint64_t coveredMs = 0;
};

// One native grid/config/size-scale constituent of a time column. Keeping these
// separate preserves provenance across grid changes. Binning pools constituents
// with the same physical tick BEFORE normalizing their native-row denominators.
struct NativeColumn {
    GridIdentity grid;
    recording::SizeScale sizeScale;
    int64_t baseRow = 0;
    uint64_t observedMs = 0;
    std::array<std::vector<CoverageRun>, 2> coverage;
    std::vector<SparseEntry> entries;
    // Empty means observedMs for every entry (always empty for minute records).
    std::vector<uint64_t> entryCoveredMs;
    bool composed = false;
    // Composed columns only: decoded mean * covered duration, parallel to entries.
    // Do not bin/recompose their rounded codes: a second log quantization can
    // change the final display code. This is an in-memory accumulator, not wire data.
    std::vector<long double> numerators;
};
struct SparseColumn {
    int64_t bucketStartMs = 0;
    uint64_t observedMs = 0;
    uint32_t flags = 0;
    std::vector<NativeColumn> native;
};
struct SparseColumns {
    std::string symbol, layer;
    int64_t tfMs = kMinuteMs;
    // Bounding extent only, NOT proof of a scan. Epoch aligned to tfMs.
    int64_t startMs = 0, endMs = 0;
    std::vector<SparseColumn> columns; // ascending time; no synthetic gap columns
    struct TimeRange {
        int64_t startMs = 0, endMs = 0; // proven scanned [start,end), including recorder gaps
        bool operator==(const TimeRange&) const = default;
    };
    std::vector<TimeRange> scannedRanges; // sorted, disjoint, coalesced, aligned to tfMs
};
enum class BucketState { NotLoaded, Gap, Present };
bool isScanned(const SparseColumns& columns, int64_t startMs, int64_t endMs);
BucketState bucketState(const SparseColumns& columns, int64_t bucketStartMs);

uint32_t packRowSide(int64_t row, int64_t baseRow, bool ask);
uint64_t coveredMs(const std::vector<CoverageRun>& runs, int64_t row);
uint64_t entryCoveredMs(const NativeColumn& column, size_t index);
long double entryNumerator(const NativeColumn& column, size_t index);
// Throws invalid_argument on malformed data. Intended for worker/ingress boundaries.
void validate(const SparseColumns& columns);
} // namespace heatmap
