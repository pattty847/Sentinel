#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include "RhiBackend.hpp"
#include <memory>

class QQuickWindow;
class QQuickRenderControl;
class QRhi;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;

namespace lab {
// Renders a real Qt Quick scene graph (QSGBatchRenderer, render nodes and all)
// into a texture on the selected QRhi backend (RhiBackend.hpp) through
// QQuickRenderControl, then reads it back. Works
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
    QRhi *rhi() const { return device_.rhi.get(); }
    const RhiBackend &backend() const { return device_.backend; }
    // Polish, sync, render one frame and read the colour target back (RGBA8,
    // top-left origin). Returns a null image on failure.
    QImage renderFrame(QString *error);
    // The same frame without the readback (timing runs). Waits for the GPU.
    bool renderFrameOnly(QString *error);
    // Whether the scene asked for a frame (QQuickRenderControl renderRequested or
    // sceneChanged) since the last render: tests drive frames only on request,
    // as a real render loop does.
    bool frameRequested() const { return requested_; }
private:
    HeadlessRhi device_;
    std::unique_ptr<QQuickRenderControl> control_;
    std::unique_ptr<QQuickWindow> window_;
    std::unique_ptr<QRhiTexture> color_, depth_;
    std::unique_ptr<QRhiTextureRenderTarget> target_;
    std::unique_ptr<QRhiRenderPassDescriptor> pass_;
    QSize size_;
    bool requested_ = false;
};
} // namespace lab
