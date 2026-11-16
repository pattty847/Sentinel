#include "DockablePanel.hpp"
#include <QWidget>
#include <QMainWindow>
#include <QEvent>
#include <QTimer>

namespace {
// Coalesce layout notifications: tab membership can change without a dock-area change.
class DockTitles final : public QObject {
public:
    explicit DockTitles(QMainWindow* window) : QObject(window), window_(window) {
        setObjectName("dockTitles");
        window->installEventFilter(this);
    }
    void schedule() {
        if (pending_) return;
        pending_ = true;
        QTimer::singleShot(0, this, [this] {
            pending_ = false;
            for (auto* dock : window_->findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                connect(dock, &QDockWidget::dockLocationChanged, this, &DockTitles::schedule, Qt::UniqueConnection);
                connect(dock, &QDockWidget::topLevelChanged, this, &DockTitles::schedule, Qt::UniqueConnection);
                connect(dock, &QDockWidget::visibilityChanged, this, &DockTitles::schedule, Qt::UniqueConnection);
                bool tabbed = false;
                for (auto* peer : window_->tabifiedDockWidgets(dock))
                    tabbed |= !peer->isHidden() && !peer->isFloating();
                const bool hide = !dock->isFloating() && tabbed;
                auto* title = dock->titleBarWidget();
                if (hide && !title) {
                    auto* empty = new QWidget(dock);
                    empty->setObjectName("tabbedDockTitle");
                    empty->setFixedHeight(0);
                    dock->setTitleBarWidget(empty);
                } else if (!hide && title && title->objectName() == "tabbedDockTitle") {
                    dock->setTitleBarWidget(nullptr);
                    title->deleteLater();
                }
            }
        });
    }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::ChildRemoved ||
            event->type() == QEvent::ChildAdded) schedule();
        return false;
    }
private:
    QMainWindow* window_;
    bool pending_ = false;
};
} // namespace

DockablePanel::DockablePanel(const QString& id, const QString& title, QWidget* parent)
    : QDockWidget(title, parent)
    , m_panelId(id)
{
    if (auto* window = qobject_cast<QMainWindow*>(parent)) {
        auto* titles = static_cast<DockTitles*>(window->findChild<QObject*>("dockTitles", Qt::FindDirectChildrenOnly));
        if (!titles) titles = new DockTitles(window);
        titles->schedule();
    }
    setObjectName(id);  // Persistent identifier for layout restoration
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    
    // Explicitly allow all dock areas to enable side-by-side docking (not just tabbing)
    setAllowedAreas(Qt::AllDockWidgetAreas);
    
    // Create content widget container
    m_contentWidget = new QWidget(this);
    setWidget(m_contentWidget);
}

