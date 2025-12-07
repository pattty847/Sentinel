/*
Sentinel — QmlSceneController
Role: Manages QML scene loading, GPU verification, and context properties.
Inputs/Outputs: Takes QQuickView, loads QML source, sets context properties, verifies GPU.
Threading: Runs on main GUI thread.
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU during UI setup.
Observability: Logs QML load status and GPU acceleration via sLog_App/sLog_Error.
Related: MainWindowGpu.cpp, UnifiedGridRenderer.h, ChartModeController.h.
*/
#pragma once

#include <QQuickView>
#include <QSGRendererInterface>
#include <QString>

// Forward declarations
class ChartModeController;
class UnifiedGridRenderer;

class QmlSceneController {
public:
    explicit QmlSceneController(QQuickView* qquickView);
    
    void loadQmlSource();
    void verifyGpuAcceleration();
    void setChartModeController(ChartModeController* controller);
    void updateSymbolInContext(const QString& symbol);
    
    UnifiedGridRenderer* getUnifiedGridRenderer() const;
    bool isValid() const;
    
private:
    QString graphicsApiName(QSGRendererInterface::GraphicsApi api) const;
    
    QQuickView* m_qquickView;
};

