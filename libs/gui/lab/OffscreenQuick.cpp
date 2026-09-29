#include "OffscreenQuick.hpp"
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif

namespace lab {
bool metalDeviceAvailable() {
#ifdef Q_OS_MACOS
    static const bool available = [] {
        void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW);
        auto create = metal ? reinterpret_cast<void *(*)()>(dlsym(metal, "MTLCreateSystemDefaultDevice")) : nullptr;
        const bool ok = create && create();
        if (metal) dlclose(metal);
        return ok;
    }();
    return available;
#else
    return false;
#endif
}

OffscreenQuick::OffscreenQuick() = default;
OffscreenQuick::~OffscreenQuick() {
    // Scene graph resources belong to the render control; release them before
    // the QRhi they were created on.
    window_.reset();
    control_.reset();
    target_.reset(); pass_.reset(); depth_.reset(); color_.reset();
    rhi_.reset();
}

bool OffscreenQuick::create(QSize pixelSize, QString *error) {
    auto fail = [&](const char *message) {
        if (error) *error = QString::fromLatin1(message);
        return false;
    };
    if (!metalDeviceAvailable()) return fail("No MTLDevice (Metal unavailable)");
#ifdef Q_OS_MACOS
    QRhiMetalInitParams init;
    rhi_.reset(QRhi::create(QRhi::Metal, &init, QRhi::EnableTimestamps));
#endif
    if (!rhi_) return fail("Metal QRhi creation failed");
    // The offscreen QPA would otherwise select the software adaptation.
    QQuickWindow::setSceneGraphBackend(QStringLiteral("rhi"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    control_ = std::make_unique<QQuickRenderControl>();
    window_ = std::make_unique<QQuickWindow>(control_.get());
    window_->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi_.get()));
    if (!control_->initialize()) return fail("QQuickRenderControl::initialize failed");
    size_ = pixelSize;
    color_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size_, 1,
                                  QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!color_->create()) return fail("colour target creation failed");
    depth_.reset(rhi_->newTexture(QRhiTexture::D24S8, size_, 1, QRhiTexture::RenderTarget));
    if (!depth_->create()) return fail("depth target creation failed");
    QRhiTextureRenderTargetDescription description(QRhiColorAttachment(color_.get()));
    description.setDepthTexture(depth_.get());
    target_.reset(rhi_->newTextureRenderTarget(description));
    pass_.reset(target_->newCompatibleRenderPassDescriptor());
    target_->setRenderPassDescriptor(pass_.get());
    if (!target_->create()) return fail("render target creation failed");
    window_->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target_.get()));
    window_->resize(size_);
    window_->contentItem()->setSize(QSizeF(size_));
    return true;
}

QImage OffscreenQuick::renderFrame(QString *error) {
    if (!control_ || !target_) {
        if (error) *error = QStringLiteral("offscreen scene was not created");
        return {};
    }
    control_->polishItems();
    control_->beginFrame();
    control_->sync();
    control_->render();
    QRhiReadbackResult readback;
    auto *updates = rhi_->nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(color_.get()), &readback);
    control_->commandBuffer()->resourceUpdate(updates);
    control_->endFrame(); // offscreen frame: waits for GPU completion
    if (readback.data.size() != qsizetype(size_.width()) * size_.height() * 4) {
        if (error) *error = QStringLiteral("scene readback failed");
        return {};
    }
    return QImage(reinterpret_cast<const uchar *>(readback.data.constData()), size_.width(), size_.height(),
                  QImage::Format_RGBA8888_Premultiplied).copy();
}
} // namespace lab
