#include "HeatmapDock.hpp"
#include <QQuickView>
#include <QSurfaceFormat>
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QQuickItem>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

HeatmapDock::HeatmapDock(QWidget* parent)
    : DockablePanel("HeatmapDock", "Charts", parent)
{
    buildUi();
}

QSize HeatmapDock::minimumSizeHint() const {
    return QSize(420, 300);
}

void HeatmapDock::buildUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(m_contentWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    m_toolbar = new TopToolbar(m_contentWidget);
    m_toolbar->setObjectName("HeatmapToolbar");
    mainLayout->addWidget(m_toolbar, 0);

    m_qquickView = new QQuickView;
    m_qquickView->setPersistentSceneGraph(true);
    m_qquickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_qquickView->setColor(Qt::black);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    m_qquickView->setFormat(format);

    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, m_contentWidget);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    mainLayout->addWidget(m_qmlContainer, 1);

    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (m_qmlContainer) {
            m_qmlContainer->setVisible(visible);
        }
        if (m_qquickView) {
            m_qquickView->setVisible(visible);
        }
    });
    
    m_contentWidget->setLayout(mainLayout);

    if (m_toolbar) {
        m_symbolInput = m_toolbar->symbolSearch();
        m_subscribeButton = nullptr; // Use toolbar button instead
    }
}

QObject* HeatmapDock::rootObject() const {
    if (m_qquickView) {
        return m_qquickView->rootObject();
    }
    return nullptr;
}

