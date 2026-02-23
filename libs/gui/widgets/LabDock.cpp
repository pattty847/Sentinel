/*
Sentinel — LabDock
*/
#include "LabDock.hpp"
#include <QVBoxLayout>
#include <QQuickView>
#include <QQuickWindow>
#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QCoreApplication>

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
    // Add QRC import path for embedded Sentinel.Charts module
    m_qquickView->engine()->addImportPath("qrc:/qt/qml");

    // Fallback: add build directory path for development
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString qmlModulePath = QDir(appDir).absoluteFilePath("../../libs/gui");
    if (QFile::exists(QDir(qmlModulePath).filePath("qmldir"))) {
        m_qquickView->engine()->addImportPath(qmlModulePath);
    }
    if (QFile::exists(":/Sentinel/Charts/LabView.qml")) {
        m_qquickView->setSource(QUrl("qrc:/Sentinel/Charts/LabView.qml"));
    } else if (QFile::exists(":/qt/qml/Sentinel/Charts/LabView.qml")) {
        m_qquickView->setSource(QUrl("qrc:/qt/qml/Sentinel/Charts/LabView.qml"));
    } else {
#ifdef SENTINEL_SOURCE_DIR
        const QString localPath = QDir(QString::fromUtf8(SENTINEL_SOURCE_DIR)).filePath("libs/gui/qml/LabView.qml");
#else
        const QString localPath = QDir::current().filePath("libs/gui/qml/LabView.qml");
#endif
        m_qquickView->setSource(QUrl::fromLocalFile(localPath));
    }

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
}
