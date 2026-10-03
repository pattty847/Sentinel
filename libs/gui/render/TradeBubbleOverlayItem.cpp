#include "TradeBubbleOverlayItem.hpp"
#include "TradeBubbleNode.hpp"
#include "../UnifiedGridRenderer.h"
#include <QQuickWindow>

namespace {
class OverlayRoot final : public QSGNode {
public:
    explicit OverlayRoot(std::shared_ptr<trade_bubbles::RenderLink> link) : link_(std::move(link)) {
        bubbles_ = new TradeBubbleNode;
        appendChildNode(bubbles_);
        link_->node = bubbles_;
    }
    bool uses(const std::shared_ptr<trade_bubbles::RenderLink>& link) const { return link_ == link; }
    ~OverlayRoot() override { if (link_->node == bubbles_) link_->node = nullptr; }
private:
    std::shared_ptr<trade_bubbles::RenderLink> link_;
    TradeBubbleNode* bubbles_;
};
}
void trade_bubbles::RenderLink::sync() {
    if (!node) return;
    if (syncedRevision != providerRevision) {
        node->clear(); // Provider replacement is distinct even if addresses/row IDs are reused.
        syncedRevision = providerRevision;
    }
    if (frame) {
        node->sync(*frame->tape, frame->mapping, frame->enabled, frame->minNotional, frame->buy, frame->sell);
    } else {
        // Clearing a provider also clears its old mesh; no QObject is accessed here.
        node->clear();
    }
}
TradeBubbleOverlayItem::TradeBubbleOverlayItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, [this] { connectWindow(); });
}
TradeBubbleOverlayItem::~TradeBubbleOverlayItem() { disconnect(m_syncConnection); }
void TradeBubbleOverlayItem::setRenderer(QObject* renderer) {
    if (m_renderer == renderer && renderer) return;
    disconnect(m_destroyConnection);
    m_renderer = renderer;
    auto* chart = qobject_cast<UnifiedGridRenderer*>(renderer);
    m_link->frame = chart ? chart->tradeBubbleRenderFrame() : nullptr;
    ++m_link->providerRevision;
    if (renderer) m_destroyConnection = connect(renderer, &QObject::destroyed, this, [this] { setRenderer(nullptr); });
    update();
    emit rendererChanged();
}
void TradeBubbleOverlayItem::connectWindow() {
    disconnect(m_syncConnection);
    // A moved item can leave a root awaiting deletion in another render thread.
    // Give each window its own node link; old roots retain only the old link.
    auto frame = m_link->frame;
    m_link = std::make_shared<trade_bubbles::RenderLink>();
    m_link->frame = std::move(frame);
    update();
    if (!window()) return;
    // afterSynchronizing runs on the render thread while the GUI is blocked,
    // before QSG batching. Capture only C++ render data, never a QObject graph.
    m_syncConnection = connect(window(), &QQuickWindow::afterSynchronizing, window(),
                              [link=m_link] { link->sync(); }, Qt::DirectConnection);
}
QSGNode* TradeBubbleOverlayItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* root = static_cast<OverlayRoot*>(oldNode);
    if (root && !root->uses(m_link)) { delete root; root = nullptr; }
    return root ? root : new OverlayRoot(m_link);
}
