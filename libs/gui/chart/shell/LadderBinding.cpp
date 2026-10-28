#include "chart/shell/LadderBinding.hpp"
#include "chart/layers/ChartLayers.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "render/GridViewState.hpp"
#include <QDateTime>
#include <QQuickItem>
#include <QQuickWindow>

namespace chart {
namespace {
// A window without a platform window is an offscreen render target (QQuickRenderControl).
bool shown(const QWindow* window) {
    return !window->handle() || (window->isVisible() && window->visibility() != QWindow::Minimized);
}

// Owned by the feed: its connections end with the feed, before the item's own teardown.
class LadderExposure final : public QObject {
public:
    LadderExposure(QQuickItem& item, BookLadderFeed& feed) : QObject(&feed), item_(item), feed_(feed) {
        connect(&item, &QQuickItem::visibleChanged, this, [this] { refresh(); });
        connect(&item, &QQuickItem::windowChanged, this, [this] { refresh(); });
        refresh();
    }
private:
    void refresh() {
        disconnect(window_);
        disconnect(top_);
        QWindow* window = item_.window();
        QWindow* top = window;
        while (top && top->parent()) top = top->parent();
        if (window) window_ = connect(window, &QWindow::visibilityChanged, this, [this] { refresh(); });
        if (top && top != window) top_ = connect(top, &QWindow::visibilityChanged, this, [this] { refresh(); });
        feed_.setExposed(item_.isVisible() && window && shown(window) && shown(top));
    }
    QQuickItem& item_;
    BookLadderFeed& feed_;
    QMetaObject::Connection window_, top_;
};
} // namespace

void bindLadder(LadderWiring w) {
    auto& feed = w.layers.ladderFeed;
    feed.setPorts({[&published = w.published] { return published.current(); }, std::move(w.top),
                   [&item = w.item] { item.update(); }, [] { return QDateTime::currentMSecsSinceEpoch(); }});
    // Render thread, after a frame the snapshot was not built for.
    w.layers.ladder.setRepublishRequest([&feed] {
        QMetaObject::invokeMethod(&feed, [&feed] { feed.onFrameChanged(); }, Qt::QueuedConnection);
    });
    QObject::connect(&w.view, &GridViewState::viewportChanged, &feed, &BookLadderFeed::onFrameChanged);
    QObject::connect(&w.view, &GridViewState::panVisualOffsetChanged, &feed, &BookLadderFeed::onFrameChanged);
    const auto apply = [&feed, &visibility = w.visibility] {
        feed.setEnabled(visibility.ladderEnabled());
        feed.setRadar(visibility.wallRadarEnabled(), visibility.wallMultiple());
    };
    QObject::connect(&w.visibility, &LayerVisibility::layerVisibilityChanged, &feed, apply);
    apply();
    new LadderExposure(w.item, feed);
}
} // namespace chart
