/*
Sentinel — LabDock
*/
#include "LabDock.hpp"
#include <QVBoxLayout>
#include <QQuickView>
#include <QQuickWindow>

LabDock::LabDock(QWidget* parent)
    : DockablePanel("LabDock", "Lab", parent) {
    buildUi();
}

QSize LabDock::minimumSizeHint() const {
    return QSize(360, 240);
}

void LabDock::buildUi() {
    QVBoxLayout* mainLayout = new QVBoxLayout(m_contentWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_qquickView = new QQuickView;
    m_qquickView->setPersistentSceneGraph(true);
    m_qquickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_qquickView->setColor(Qt::black);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    m_qquickView->setFormat(format);
    m_qquickView->setSource(QUrl("qrc:/Sentinel/Charts/LabView.qml"));

    m_qmlContainer = QWidget::createWindowContainer(m_qquickView, m_contentWidget);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    mainLayout->addWidget(m_qmlContainer, 1);

    m_contentWidget->setLayout(mainLayout);
}
