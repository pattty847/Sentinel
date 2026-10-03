#include "OffscreenQuick.hpp"
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#endif

namespace lab {
OffscreenQuick::OffscreenQuick() = default;
OffscreenQuick::~OffscreenQuick() {
    // Scene graph resources belong to the render control; release them before
    // the QRhi they were created on.
    window_.reset();
    control_.reset();
    target_.reset(); pass_.reset(); depth_.reset(); color_.reset();
    // device_ (the QRhi) is destroyed last, as a member.
}

bool OffscreenQuick::create(QSize pixelSize, QString *error) {
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!device_.create(true)) return fail(device_.error);
    QRhi *rhi = device_.rhi.get();
    const QString name = device_.backend.name;
    // The offscreen QPA would otherwise select the software adaptation. Qt fixes
    // the backend at the first QQuickWindow of the process: a binary that makes
    // one before this point selects it in main() (lab::selectQuickSceneGraph).
    if (QString why; !selectQuickSceneGraph(&why)) return fail(why);
    control_ = std::make_unique<QQuickRenderControl>();
    QObject::connect(control_.get(), &QQuickRenderControl::renderRequested, [this] { requested_ = true; });
    QObject::connect(control_.get(), &QQuickRenderControl::sceneChanged, [this] { requested_ = true; });
    window_ = std::make_unique<QQuickWindow>(control_.get());
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    if (device_.vulkan) window_->setVulkanInstance(device_.vulkan.get());
#endif
    window_->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi));
    if (!control_->initialize())
        return fail(name + QStringLiteral(" backend: QQuickRenderControl::initialize failed (a QQuickWindow created "
                                          "earlier in this process may have fixed another scene graph backend; "
                                          "call lab::selectQuickSceneGraph() in main() before any QQuickWindow)"));
    size_ = pixelSize;
    color_.reset(rhi->newTexture(QRhiTexture::RGBA8, size_, 1,
                                  QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!color_->create()) return fail(name + QStringLiteral(" backend: colour target creation failed"));
    depth_.reset(rhi->newTexture(QRhiTexture::D24S8, size_, 1, QRhiTexture::RenderTarget));
    if (!depth_->create()) return fail(name + QStringLiteral(" backend: depth target creation failed"));
    QRhiTextureRenderTargetDescription description(QRhiColorAttachment(color_.get()));
    description.setDepthTexture(depth_.get());
    target_.reset(rhi->newTextureRenderTarget(description));
    pass_.reset(target_->newCompatibleRenderPassDescriptor());
    target_->setRenderPassDescriptor(pass_.get());
    if (!target_->create()) return fail(name + QStringLiteral(" backend: render target creation failed"));
    window_->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target_.get()));
    window_->resize(size_);
    window_->contentItem()->setSize(QSizeF(size_));
    return true;
}

bool OffscreenQuick::renderFrameOnly(QString *error) {
    if (!control_ || !target_) {
        if (error) *error = QStringLiteral("offscreen scene was not created");
        return false;
    }
    requested_ = false;
    control_->polishItems();
    control_->beginFrame();
    control_->sync();
    control_->render();
    control_->endFrame(); // offscreen frame: waits for GPU completion
    return true;
}

QImage OffscreenQuick::renderFrame(QString *error) {
    if (!control_ || !target_) {
        if (error) *error = QStringLiteral("offscreen scene was not created");
        return {};
    }
    requested_ = false;
    control_->polishItems();
    control_->beginFrame();
    control_->sync();
    control_->render();
    QRhiReadbackResult readback;
    auto *updates = device_.rhi->nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(color_.get()), &readback);
    control_->commandBuffer()->resourceUpdate(updates);
    control_->endFrame(); // offscreen frame: waits for GPU completion
    if (readback.data.size() != qsizetype(size_.width()) * size_.height() * 4) {
        if (error) *error = QStringLiteral("scene readback failed");
        return {};
    }
    QImage image(reinterpret_cast<const uchar *>(readback.data.constData()), size_.width(), size_.height(),
                 QImage::Format_RGBA8888_Premultiplied);
    // OpenGL reads back bottom row first; every other backend is top-left.
    return device_.rhi->isYUpInFramebuffer() ? image.mirrored() : image.copy();
}
} // namespace lab
