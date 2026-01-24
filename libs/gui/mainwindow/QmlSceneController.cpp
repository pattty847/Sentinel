#include "QmlSceneController.h"
#include "../UnifiedGridRenderer.h"
#include "../ChartModeController.h"
#include "../../core/SentinelLogging.hpp"
#include <QQmlContext>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QWidget>

QmlSceneController::QmlSceneController(QQuickView* qquickView) 
    : m_qquickView(qquickView) {
}

void QmlSceneController::loadQmlSource() {
    // Configurable path (extract from config)
    QSettings config("config.ini", QSettings::IniFormat);
    QString qmlPath = config.value("qml/path", QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath())).toString();
    
    const QString qmlEnv = qEnvironmentVariable("SENTINEL_QML_PATH");
    if (!qmlEnv.isEmpty()) qmlPath = qmlEnv;  // Override
    
    if (QFile::exists(qmlPath)) {
        m_qquickView->setSource(QUrl::fromLocalFile(qmlPath));
    } else {
        m_qquickView->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    if (m_qquickView->status() == QQuickView::Error) {
        sLog_Error("QML FAILED TO LOAD! Errors: " << m_qquickView->errors());
        // Note: QMessageBox requires parent widget, so we'll let MainWindowGPU handle this
    }
}

void QmlSceneController::verifyGpuAcceleration() {
    if (m_qquickView->status() != QQuickView::Ready) return;

    auto* rhi = m_qquickView->rendererInterface();
    if (!rhi || rhi->graphicsApi() == QSGRendererInterface::Null) {
        sLog_Error("No GPU acceleration available");
    }
}

void QmlSceneController::setChartModeController(ChartModeController* controller) {
    if (!m_qquickView || !controller) return;
    
    QQmlContext* context = m_qquickView->rootContext();
    context->setContextProperty("chartModeController", controller);
}

void QmlSceneController::updateSymbolInContext(const QString& symbol) {
    if (m_qquickView) {
        m_qquickView->rootContext()->setContextProperty("symbol", symbol);
    }
}

UnifiedGridRenderer* QmlSceneController::getUnifiedGridRenderer() const {
    return m_qquickView->rootObject() 
        ? m_qquickView->rootObject()->findChild<UnifiedGridRenderer*>("unifiedGridRenderer") 
        : nullptr;
}

bool QmlSceneController::isValid() const {
    return m_qquickView && m_qquickView->status() == QQuickView::Ready;
}

QString QmlSceneController::graphicsApiName(QSGRendererInterface::GraphicsApi api) const {
    switch (api) {
        case QSGRendererInterface::OpenGL: return "OpenGL";
        case QSGRendererInterface::Direct3D11: return "Direct3D 11";
        case QSGRendererInterface::Vulkan: return "Vulkan";
        case QSGRendererInterface::Metal: return "Metal";
        default: return "Unknown";
    }
}


