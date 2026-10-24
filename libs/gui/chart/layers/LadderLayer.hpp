#pragma once
#include "chart/layers/IChartLayer.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

class QSGOpacityNode;
class QSGClipNode;
class QSGGeometryNode;

namespace chart {
// One display bin of the book on the chart's absolute row grid: price [k, k+1) * binPrice.
struct LadderRow {
    float bid = 0, ask = 0;
};

// The book on the drawn rows, published by the GUI-thread feed (BookLadderFeed).
struct LadderSnapshot {
    static constexpr int kMaxRows = 4096;
    uint64_t generation = 0; // symbol generation
    uint64_t serial = 0;     // the feed's publication count
    bool valid = false;      // false: withdrawn, no book, no raster, unsupported grid, too wide
    bool complete = false;   // the rows cover the whole window the feed saw
    // The grid: server tick, display tick = group server ticks, row 0 = absolute bin binLo.
    double serverTick = 0;
    int64_t group = 0, binLo = 0;
    int rowCount = 0;
    // The frame the feed evaluated (a layer republish is useful only for another one).
    double evalTick = 0;
    int64_t evalLo = 0, evalHi = 0;
    double bestBid = 0, bestAsk = 0;
    int64_t receivedAtMs = 0;
    std::array<LadderRow, kMaxRows> rows{};
    double binPrice() const { return double(group) * serverTick; }
};

// GUI thread writes `back()` and posts; the render thread takes the newest during sync.
// Every buffer is allocated once here; posting and taking only swap pointers.
class LadderMailbox {
public:
    LadderMailbox();
    ~LadderMailbox();
    LadderSnapshot& back() { return *back_; }
    void post();
    // Swaps `current` with the posted snapshot when one is waiting and its generation
    // is not older than `minGeneration` (an older one is dropped).
    bool take(std::unique_ptr<LadderSnapshot>& current, uint64_t minGeneration);
private:
    std::mutex mutex_;
    std::unique_ptr<LadderSnapshot> back_, pending_;
    bool fresh_ = false;
};

// Plain bid and ask bars in the strip right of the item (the reserved gutter before
// the price axis), on the heatmap's rows. Off: opacity 0, no geometry work.
class LadderLayer final : public IChartLayer {
public:
    static constexpr float kInsetPx = 2.0f;
    LadderLayer();
    ~LadderLayer() override;
    std::string_view name() const override { return "ladder"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void afterRendering() override;
    LadderMailbox& mailbox() { return mailbox_; }
    // Called from afterRendering (render thread) when the snapshot was built for
    // another frame; the callback queues the republish on the GUI thread.
    void setRepublishRequest(std::function<void()> request) { republish_ = std::move(request); }
    // Observers (tests, probes).
    bool drawing() const { return drawing_; }
    const LadderSnapshot& current() const { return *current_; }
    uint64_t rebuildCount() const { return rebuilds_; }
    uint64_t dirtyMarks() const { return dirtyMarks_; }
    uint64_t nodeCreations() const { return creations_; }
    int vertexCount() const { return vertices_; }
private:
    struct Key {
        uint64_t serial = 0;
        double topF = 0, rowPxF = 0, tick = 0, dpr = 0, left = 0, width = 0, top = 0, height = 0;
        bool operator==(const Key&) const = default;
    };
    bool drawable(const ChartFrame&) const;
    void rebuild(const ChartFrame&);
    LadderMailbox mailbox_;
    std::unique_ptr<LadderSnapshot> current_;
    std::function<void()> republish_;
    std::atomic<bool> wantsRepublish_{false};
    uint64_t requestedFor_ = 0, generation_ = 0;
    QSGOpacityNode* gate_ = nullptr;
    QSGClipNode* clip_ = nullptr;
    QSGGeometryNode* bars_ = nullptr;
    bool drawing_ = false, built_ = false;
    Key key_;
    uint64_t rebuilds_ = 0, dirtyMarks_ = 0, creations_ = 0;
    int vertices_ = 0;
};
} // namespace chart
