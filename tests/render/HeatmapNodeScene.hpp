#pragma once
// An offscreen Qt Quick scene with one HeatmapTileNode host (S5c/S5L-c node
// tests): the test sets the node's Frame, renders a frame with the real scene
// graph on the selected QRhi backend, and reads the image and node stats.
#include "lab/OffscreenQuick.hpp"
#include "render/heatmap/HeatmapTileNode.hpp"
#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <algorithm>
#include <memory>
#include <vector>

namespace nodefx {
class NodeHost : public QQuickItem {
public:
    NodeHost() { setFlag(ItemHasContents, true); }
    std::shared_ptr<heatmap::gpu::HeatmapTileStats> stats = std::make_shared<heatmap::gpu::HeatmapTileStats>();
    heatmap::gpu::HeatmapTileNode::Frame frame;
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        auto *node = old ? static_cast<heatmap::gpu::HeatmapTileNode *>(old) : new heatmap::gpu::HeatmapTileNode(stats);
        auto f = frame;
        f.rect = QRectF(0, 0, width(), height());
        node->setFrame(std::move(f));
        return node;
    }
};
struct Scene {
    lab::OffscreenQuick quick;
    NodeHost *host = nullptr;
    QString error;
    QImage image;
    bool create(QSize size) {
        if (!quick.create(size, &error)) return false;
        quick.window()->setColor(Qt::black);
        host = new NodeHost;
        host->setParentItem(quick.window()->contentItem());
        host->setSize(QSizeF(size));
        return true;
    }
    bool frame() {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        host->update();
        image = quick.renderFrame(&error);
        return !image.isNull();
    }
    ~Scene() { delete host; }
};
inline bool subset(std::vector<uint64_t> drawn, std::vector<uint64_t> resident) {
    std::sort(resident.begin(), resident.end());
    return std::all_of(drawn.begin(), drawn.end(),
                       [&](uint64_t id) { return std::binary_search(resident.begin(), resident.end(), id); });
}
} // namespace nodefx
