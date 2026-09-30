#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include <memory>

class QQuickWindow;
class QQuickRenderControl;
class QRhi;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;

namespace lab {
// Qt's Metal QRhi aborts on a machine without an MTLDevice (sandboxed runs).
// Probe before creating one so tests skip and tools report instead of crashing.
bool metalDeviceAvailable();

// Renders a real Qt Quick scene graph (QSGBatchRenderer, render nodes and all)
// into a Metal texture through QQuickRenderControl, then reads it back. Works
// headless and with a locked screen, unlike QQuickWindow::grabWindow on an
// unexposed window. Requires a QGuiApplication; GUI thread only.
class OffscreenQuick {
public:
    OffscreenQuick();
    ~OffscreenQuick();
    OffscreenQuick(const OffscreenQuick &) = delete;
    OffscreenQuick &operator=(const OffscreenQuick &) = delete;
    bool create(QSize pixelSize, QString *error);
    QQuickWindow *window() const { return window_.get(); }
    QRhi *rhi() const { return rhi_.get(); }
    // Polish, sync, render one frame and read the colour target back (RGBA8,
    // top-left origin). Returns a null image on failure.
    QImage renderFrame(QString *error);
    // The same frame without the readback (timing runs). Waits for the GPU.
    bool renderFrameOnly(QString *error);
private:
    std::unique_ptr<QRhi> rhi_;
    std::unique_ptr<QQuickRenderControl> control_;
    std::unique_ptr<QQuickWindow> window_;
    std::unique_ptr<QRhiTexture> color_, depth_;
    std::unique_ptr<QRhiTextureRenderTarget> target_;
    std::unique_ptr<QRhiRenderPassDescriptor> pass_;
    QSize size_;
};
} // namespace lab
