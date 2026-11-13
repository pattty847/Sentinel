#include "chart/layers/LayerRegistry.hpp"
#include <QQuickItem>
#include <QSGNode>

namespace chart {
// Only render-thread roots touch these pointers. Both ends retain the link, so
// either root may die first, including during scene invalidation or a window move.
struct LayerOutput {
    QSGNode* owner = nullptr;
    QSGNode* surface = nullptr;
    QSGNode* content = nullptr;
    void attach(QSGNode* target) {
        if (!content || !target || content->parent() == target) return;
        if (content->parent()) content->parent()->removeChildNode(content);
        target->appendChildNode(content);
    }
};
namespace {
using Outputs = std::array<std::shared_ptr<LayerOutput>, 2>;
class OutputOwner final : public QSGNode {
public:
    explicit OutputOwner(const Outputs& outputs) : outputs_(outputs) {
        for (const auto& output : outputs_) {
            output->owner = this;
            output->content = new QSGNode;
            appendChildNode(output->content);
            output->attach(output->surface);
        }
    }
    ~OutputOwner() override {
        for (const auto& output : outputs_) {
            auto* content = output->content;
            if (content->parent()) content->parent()->removeChildNode(content);
            output->content = output->owner = nullptr;
            delete content;
        }
    }
    bool uses(const Outputs& outputs) const { return outputs_ == outputs; }
private:
    Outputs outputs_;
};
class OutputSurface final : public QSGNode {
public:
    explicit OutputSurface(std::shared_ptr<LayerOutput> output) : output_(std::move(output)) {
        output_->surface = this;
        output_->attach(this);
    }
    ~OutputSurface() override {
        output_->attach(output_->owner);
        output_->surface = nullptr;
    }
    bool uses(const std::shared_ptr<LayerOutput>& output) const { return output_ == output; }
private:
    std::shared_ptr<LayerOutput> output_;
};
QSGNode* surfaceRoot(const std::shared_ptr<LayerOutput>& output, QSGNode* oldRoot) {
    auto* root = static_cast<OutputSurface*>(oldRoot);
    if (root && !root->uses(output)) { delete root; root = nullptr; }
    return root ? root : new OutputSurface(output);
}
}
class LayerOutputItem final : public QQuickItem {
public:
    explicit LayerOutputItem(QQuickItem* parent, ChartDepth depth) : QQuickItem(parent) {
        setFlag(ItemHasContents, true);
        setZ(chart::depth(depth));
        setObjectName(depth == ChartDepth::Profiles ? "chartProfiles" : "chartForeground");
        // A nonempty item participates in sync; its unclipped nodes use chart coordinates.
        setSize({1, 1});
    }
    std::shared_ptr<LayerOutput> output;
protected:
    QSGNode* updatePaintNode(QSGNode* oldRoot, UpdatePaintNodeData*) override {
        return surfaceRoot(output, oldRoot);
    }
};

LayerRegistry::LayerRegistry() {
    for (auto& output : outputs_) output = std::make_shared<LayerOutput>();
}
LayerRegistry::~LayerRegistry() {
    QObject::disconnect(windowChanged_);
    for (auto* item : items_) delete item;
}
void LayerRegistry::bindOutputs(QQuickItem& chart) {
    items_ = {new LayerOutputItem(&chart, ChartDepth::Profiles), new LayerOutputItem(&chart, ChartDepth::Text)};
    windowChanged_ = QObject::connect(&chart, &QQuickItem::windowChanged, &chart,
                                     [this, &chart](QQuickWindow*) { resetOutputs(); chart.update(); });
    resetOutputs();
}
void LayerRegistry::resetOutputs() {
    // Old scene roots may still live on the old render thread. Never share links
    // between windows, just as the bubble frame handoff keeps separate node links.
    for (size_t i = 0; i < outputs_.size(); ++i) {
        outputs_[i] = std::make_shared<LayerOutput>();
        items_[i]->output = outputs_[i];
        items_[i]->update();
    }
}
QSGNode* LayerRegistry::createRoot(QSGNode* oldRoot) {
    auto* root = static_cast<OutputOwner*>(oldRoot);
    if (root && !root->uses(outputs_)) { delete root; root = nullptr; }
    if (!root) {
        root = new OutputOwner(outputs_);
        onRootRebuilt(root);
    }
    return root;
}
QSGNode* LayerRegistry::createOutputRoot(LayerPlane plane, QSGNode* oldRoot) {
    return surfaceRoot(outputs_[plane == LayerPlane::Profiles ? 0 : 1], oldRoot);
}
QSGNode* IChartLayer::createContainer() { return new QSGNode(); }
QSGNode* LayerRegistry::parentFor(const Entry& entry) const {
    if (entry.plane == LayerPlane::Fields) return root_;
    return outputs_[entry.plane == LayerPlane::Profiles ? 0 : 1]->content;
}
void LayerRegistry::onRootRebuilt(QSGNode* root) {
    root_ = root;
    for (auto& entry : layers_) {
        entry.layer->onRootRebuilt();
        entry.container = entry.layer->createContainer();
        if (entry.container) parentFor(entry)->appendChildNode(entry.container);
    }
}
void LayerRegistry::ensureContainer(Entry& entry) {
    if (entry.container || !root_) return;
    entry.container = entry.layer->createContainer();
    if (!entry.container) return;
    auto* parent = parentFor(entry);
    // A lazy layer takes its registered position within its own output plane.
    bool after = false;
    for (auto& next : layers_) {
        if (after && next.plane == entry.plane && next.container) {
            parent->insertChildNodeBefore(entry.container, next.container);
            return;
        }
        if (&next == &entry) after = true;
    }
    parent->appendChildNode(entry.container);
}
} // namespace chart
