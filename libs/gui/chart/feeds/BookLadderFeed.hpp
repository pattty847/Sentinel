#pragma once
#include "chart/layers/LadderLayer.hpp"
#include "render/ITimeAxisMappingProvider.hpp"
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
    std::function<int64_t()> clock;             // receive time, ms
};

// GUI thread. Every book update forwards the global top; while the ladder is on and
// the chart exposed, it arms one coalescing timer that bins the replica onto the
// drawn rows (native server-level indices) and posts a LadderSnapshot to the layer.
class BookLadderFeed final : public QObject {
public:
    static constexpr int kIntervalMs = 67;
    static constexpr size_t kMaxLevels = 16384, kMaxScanWords = 16384;
    explicit BookLadderFeed(LadderMailbox& mailbox);
    void setPorts(LadderFeedPorts ports) { ports_ = std::move(ports); }
    void publish(const LiveOrderBook& book);
    void beginGeneration();
    void setEnabled(bool enabled);
    void setExposed(bool exposed);
    void onFrameChanged();
    // The timer's work: bin the book onto the published frame's rows and post them.
    void publishRows();
    bool active() const { return enabled_ && exposed_; }
    bool pending() const { return timer_.isActive(); }
    uint64_t generation() const { return generation_; }
    uint64_t rowPublications() const { return rowPublications_; }
    const char* lastReason() const { return reason_; }
private:
    struct Levels {
        std::vector<std::pair<uint32_t, double>> bids, asks;
    };
    void arm(int ms);
    void activeChanged(bool wasActive);
    void withdraw(const char* reason);
    LadderSnapshot& begin(const MappingFrameContext& frame);
    void post();
    const char* fill(LadderSnapshot& s, const chart_raster::RasterCamera& cam);
    LadderMailbox& mailbox_;
    LadderFeedPorts ports_;
    QTimer timer_;
    const LiveOrderBook* book_ = nullptr;
    Levels top_, rows_;
    double lastTick_ = 0;
    uint64_t generation_ = 0, serial_ = 0, rowPublications_ = 0;
    int64_t receivedAtMs_ = 0;
    double bestBid_ = 0, bestAsk_ = 0;
    bool enabled_ = false, exposed_ = true;
    const char* reason_ = "none";
};
} // namespace chart
