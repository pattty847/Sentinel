/*
Sentinel — HeatmapColumnWindow
Role: client heatmap column cache plus the slot-addressed GPU window onto it.
      The cache is the data; the window is W consecutive buckets that slide by
      rewriting only the slots of buckets that enter it (INV-045).
Threading: owned and used by DataProcessor on its worker thread only.
*/
#pragma once

#include <QByteArray>
#include <QMetaType>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heatmap_window {

// One recorded bucket in its source (server) price band.
struct Column {
    int64_t bucketStartMs = 0;
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
    QByteArray intensity;   // rows * bytesPerCell, little-endian
    QByteArray liquidity;   // rows * 2, or empty
    double liquidityScale = 1.0;
    QByteArray validity; // packed LSB-first row bits; empty for legacy
    uint64_t observedMs = 0; // recording only: history may finalize a missed provisional
    bool provisional = false;
};

struct Band {
    double minPrice = 0.0;
    double maxPrice = 0.0;
    double tickSize = 0.0;
    bool valid() const { return maxPrice > minPrice && tickSize > 0.0; }
    bool sameAs(const Band& other) const;
};

struct SlotWrite {
    int slot = 0;
    int64_t bucketStartMs = 0;
    bool recorded = false;
    QByteArray intensity;   // display band, rows * bytesPerCell
    QByteArray liquidity;   // rows * 2, or empty
    double liquidityScale = 1.0;
    QByteArray validity; // packed LSB-first row bits; empty for legacy
};

enum class ValueEncoding { LegacyIntensity, AbsoluteLogSize };

// Everything the GUI needs to bring its ring in line with the window.
struct Update {
    int64_t timeframeMs = 0;
    int width = 0;
    int rows = 0;
    int bytesPerCell = 0;
    Band band;
    ValueEncoding valueEncoding = ValueEncoding::LegacyIntensity;
    uint64_t bandGeneration = 0;
    double sizeFloor = 0.0;
    double codesPerOctave = 0.0;
    int64_t windowEndMs = 0;    // newest bucket start in the window
    int newestSlot = 0;         // slot of windowEndMs; the oldest slot is newestSlot + 1
    bool full = false;          // every slot rewritten (placement jump or band change)
    bool pinnedToLive = false;  // window end is the newest live bucket
    std::vector<SlotWrite> writes;
    QByteArray coverage;        // width bytes, chronological (0 = oldest), 1 = recorded
    int64_t liveBucketMs = 0;   // > 0 when writes carry the newest live column
};
using UpdatePtr = std::shared_ptr<const Update>;

struct FetchRequest {
    int64_t endMs = 0;
    int count = 0;
};

struct WallQuery {
    std::optional<int64_t> startMs, endMs;
    std::optional<double> priceMin, priceMax;
    double minQty = 0.0;
    int limit = 20;
};

// One price cell and side over the scanned time range: a wall is a level, not a
// minute. qty is its peak aggregated resting size; bucketStartMs is when it peaked.
struct Wall {
    int64_t bucketStartMs = 0;
    double priceLow = 0, priceHigh = 0;
    bool ask = false;
    double qty = 0, notional = 0;
    bool forming = false;
    double meanQty = 0;          // mean over recorded columns in range (absent = 0)
    int64_t firstSeenMs = 0, lastSeenMs = 0;
    int columns = 0;             // recorded columns where the cell held this side
};

struct WallsSnapshot {
    int status = 200;
    int64_t loadedStartMs = 0, loadedEndMs = 0;
    double bandTick = 0;
    int recordedColumns = 0, missingColumns = 0;
    bool unknownRows = false;
    std::vector<Wall> walls;
};

// Signed heatmap intensity magnitude: bids are v, asks are v - 0x8000 (u16),
// matching heatmap_intensity.frag.
int intensityMagnitude(uint16_t value, int bytesPerCell);

// Maps a source column into a display band with the same row count. Rows that
// land on the same target keep the larger magnitude.
void resampleColumn(const Column& source, const Band& target, int rows, int bytesPerCell,
                    QByteArray& outIntensity, QByteArray& outLiquidity);

class ColumnWindow {
public:
    static constexpr int kPageColumns = 1024;

    // Resets the cache and window. width is the GPU window in buckets;
    // cacheCapacity is clamped to at least width + one page.
    void configure(int64_t timeframeMs, int width, int cacheCapacity);
    void clear();

    int64_t timeframeMs() const { return m_timeframeMs; }
    int width() const { return m_width; }
    int rows() const { return m_rows; }
    bool placed() const { return m_placed; }
    int64_t windowEndMs() const { return m_windowEndMs; }
    int64_t oldestAvailableMs() const { return m_floorMs; }
    size_t cachedColumns() const { return m_cache.size(); }
    WallsSnapshot captureWalls(const WallQuery& query) const;

    // Explicit recording projection; never clears the source/live cache or placement.
    // A generation change invalidates projected data/known ranges and rewrites every slot.
    bool setDisplayBand(const Band& band, uint64_t generation, Update& out);
    void setRecordingRequest(const std::string& requestId) { m_requestId = requestId; }
    bool ingestRecording(const std::vector<Column>& columns, uint64_t generation,
                         const std::string& requestId, int64_t scannedStartMs,
                         int64_t scannedEndMs, bool exhausted, int64_t oldestAvailableMs,
                         int64_t latestAvailableMs, double sizeFloor, double codesPerOctave,
                         Update& out, bool& firstPlacement, bool live = false);

    // Live forming/finalized bucket. Returns true and fills out when the GPU
    // window changes. firstPlacement is set when this created the window.
    bool ingestLive(const Column& column, int bytesPerCell, Update& out, bool& firstPlacement);

    // One server page. requestEndMs == 0 asked for the newest page.
    bool ingestHistory(const std::vector<Column>& columns,
                       int bytesPerCell,
                       int64_t requestEndMs,
                       int requestedCount,
                       int64_t oldestAvailableMs,
                       Update& out,
                       bool& firstPlacement);

    // Visible time range and whether the view follows live.
    bool setViewport(int64_t viewStartMs, int64_t viewEndMs, bool follow, Update& out);

    // Newest bucket near the viewport that is neither cached nor known missing.
    bool nextFetch(FetchRequest& out) const;
    int64_t unfinishedRecordingBucket() const;

private:
    int slotFor(int64_t bucketMs) const;
    int64_t align(int64_t ms) const;
    int64_t windowStartMs() const;
    int64_t latestDataMs() const;
    bool isKnown(int64_t bucketMs) const;
    void addKnown(int64_t startMs, int64_t endMs);
    bool acceptShape(const Column& column, int bytesPerCell);
    Band unionBand(int64_t startMs, int64_t endMs) const;
    const Column* newestCached() const;
    bool place(Update& out, const std::vector<int64_t>& changed);
    void emitWindow(bool full, const std::vector<int64_t>& changed, Update& out);
    void writeSlot(int64_t bucketMs, SlotWrite& out);
    void evict();

    int64_t m_timeframeMs = 0;
    int m_width = 0;
    int m_capacity = 0;
    int m_rows = 0;
    int m_bytesPerCell = 0;

    bool m_recording = false;
    uint64_t m_bandGeneration = 0;
    std::string m_requestId;
    double m_sizeFloor = 0.0, m_codesPerOctave = 0.0;
    int64_t m_latestRecordingMs = 0;
    std::map<int64_t, Column> m_recordingLive;
    uint64_t m_liveGeneration = 0;
    std::map<int64_t, Column> m_projected;
    std::map<int64_t, Column> m_cache;
    std::map<int64_t, int64_t> m_known;   // start -> end, inclusive, tf-aligned
    int64_t m_floorMs = 0;                // server storage floor; older is known missing
    int64_t m_latestLiveMs = 0;

    bool m_hasView = false;
    bool m_follow = true;
    int64_t m_viewStartMs = 0;
    int64_t m_viewEndMs = 0;

    bool m_placed = false;
    bool m_pinned = false;                // window end tracks the newest live bucket
    int64_t m_windowEndMs = 0;
    Band m_band;
    QByteArray m_zeroValidity;            // shared all-unknown recording mask
    QByteArray m_zeroIntensity;           // shared blank column for missing buckets
    std::vector<int64_t> m_slotBucket;    // bucket currently shown in each slot
    std::vector<uint8_t> m_slotRecorded;
};

} // namespace heatmap_window

Q_DECLARE_METATYPE(heatmap_window::UpdatePtr)
