#pragma once
#include "TradeBubbleFrame.hpp"
#include <QQuickItem>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

class TradeBubbleNode;
namespace trade_bubbles {
// The typed node pointer is render-thread-only, and cleared by its owning root.
struct RenderLink {
    std::shared_ptr<RenderFrame> frame;
    TradeBubbleNode* node = nullptr;
    uint64_t providerRevision = 1, syncedRevision = 0;
    void sync();
};
}
class TradeBubbleOverlayItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QObject* renderer READ renderer WRITE setRenderer NOTIFY rendererChanged)
public:
    explicit TradeBubbleOverlayItem(QQuickItem* parent = nullptr);
    ~TradeBubbleOverlayItem() override;
    QObject* renderer() const { return m_renderer.data(); }
    void setRenderer(QObject* renderer);
signals:
    void rendererChanged();
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
private:
    friend struct TradeBubbleOverlayTest;
    void connectWindow();
    QPointer<QObject> m_renderer;
    std::shared_ptr<trade_bubbles::RenderLink> m_link = std::make_shared<trade_bubbles::RenderLink>();
    QMetaObject::Connection m_syncConnection, m_destroyConnection;
};
