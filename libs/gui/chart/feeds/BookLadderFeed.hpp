#pragma once
#include "chart/feeds/LadderTracker.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <functional>
#include <utility>
#include <vector>

class LiveOrderBook;

namespace chart {
struct LadderFeedPorts {
    std::function<MappingFrameContext()> frame; // the published (drawn) frame
    std::function<void(double, double)> top;    // global best bid and ask, on every book update
    std::function<void()> update;               // schedule a frame
    std::function<int64_t()> clock;             // receive time, ms (wall clock)
    std::function<int64_t()> monotonic;         // wall radar clock, ms (empty: a feed-owned timer)
};

// GUI thread. Every book update forwards the global top; while the ladder is on and
// the chart exposed, it arms one coalescing timer that bins the replica onto the
// drawn rows (native server-level indices) and posts a LadderSnapshot to the layer.
// With the wall radar on it also tracks the rows (LadderTracker) and republishes every
// radar interval while active, so ages advance on a quiet book. A side's wall reference
// is the median size of its nonempty display bins among the kWallReferenceBins from its
// touch outward, from the whole capture (not the published rows), so dust far from the
// market at a wide zoom does not lower it and a view panned away from the market keeps
// it. A publication that is no observation (an incomplete capture) keeps the last one.
class BookLadderFeed final : public QObject {
public:
    static constexpr int kIntervalMs = 67;
    static constexpr int kRadarIntervalMs = 67; // 15 Hz, the DOM's cadence
    static constexpr int kWallReferenceBins = 32;
    static constexpr size_t kMaxLevels = 16384, kMaxScanWords = 16384;
    explicit BookLadderFeed(LadderMailbox& mailbox);
    void setPorts(LadderFeedPorts ports) { ports_ = std::move(ports); }
    void publish(const LiveOrderBook& book);
    void beginGeneration();
    void setEnabled(bool enabled);
    void setExposed(bool exposed);
    void onFrameChanged();
    // Radar off resets the tracker; on (while active) starts from a fresh baseline.
    void setRadar(bool on, double multiple);
    bool radar() const { return radar_; }
    double wallMultiple() const { return multiple_; }
    // The timer's work: bin the book onto the published frame's rows and post them.
    void publishRows();
    bool active() const { return enabled_ && exposed_; }
    bool pending() const { return timer_.isActive(); }
    uint64_t generation() const { return generation_; }
    uint64_t rowPublications() const { return rowPublications_; }
    const char* lastReason() const { return reason_; }
    const LadderTracker& tracker() const { return tracker_; }
    // Test hooks.
    void setRadarIntervalMs(int ms) { radarIntervalMs_ = ms; }
    void setScanBudget(size_t words, size_t levels) { scanWords_ = words, scanLevels_ = levels; }
private:
    struct Levels {
        std::vector<std::pair<uint32_t, double>> bids, asks;
    };
    void arm(int ms);
    void activeChanged(bool wasActive);
    void withdraw(const char* reason);
    int64_t monotonicMs() const;
    uint64_t series() const { return (generation_ << 32) | (epoch_ & 0xffffffffu); }
    LadderSnapshot& begin(const MappingFrameContext& frame);
    void post();
    const char* fill(LadderSnapshot& s, const chart_raster::RasterCamera& cam);
    LadderMailbox& mailbox_;
    LadderFeedPorts ports_;
    QTimer timer_;
    const LiveOrderBook* book_ = nullptr;
    LadderTracker tracker_;
    WallCandidates walls_;
    QElapsedTimer monotonic_;
    uint64_t epoch_ = 0;
    double multiple_ = 4;
    int radarIntervalMs_ = kRadarIntervalMs;
    size_t scanWords_ = kMaxScanWords, scanLevels_ = kMaxLevels;
    char asset_[8] = {};
    bool radar_ = false;
    Levels top_, rows_;
    double lastTick_ = 0;
    uint64_t generation_ = 0, serial_ = 0, rowPublications_ = 0;
    int64_t receivedAtMs_ = 0;
    double bestBid_ = 0, bestAsk_ = 0;
    bool enabled_ = false, exposed_ = true;
    const char* reason_ = "none";
};
} // namespace chart
