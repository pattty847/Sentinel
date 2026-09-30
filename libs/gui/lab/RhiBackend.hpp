#pragma once
// Runtime QRhi backend selection for the headless GPU harness (tests, lab, bench).
// SENTINEL_RHI_BACKEND=d3d11|d3d12|vulkan|opengl|metal picks one; otherwise the
// platform default: d3d11 on Windows, metal on macOS, opengl elsewhere.
// QRhi headers stay out of this header: they need Qt6::GuiPrivate, which not
// every includer links.
#include <QSGRendererInterface>
#include <QString>
#include <memory>
#include <string>

class QOffscreenSurface;
class QRhi;
class QVulkanInstance;

namespace lab {
struct RhiBackend {
    QString name;                                  // "d3d11", "metal", ...
    QSGRendererInterface::GraphicsApi graphicsApi = QSGRendererInterface::Unknown;
    bool fromEnv = false;                          // SENTINEL_RHI_BACKEND chose it
};

// The backend to use. `valid` is false (and `error` says why) for an unknown
// SENTINEL_RHI_BACKEND value or a backend this build or platform cannot create.
struct RhiSelection {
    RhiBackend backend;
    bool valid = false;
    QString error;
};
RhiSelection selectedRhiBackend();

// A headless QRhi plus what its backend needs to stay alive (the OpenGL fallback
// surface, the Vulkan instance). Members are declared so the QRhi dies first;
// shared_ptr because QVulkanInstance is incomplete in builds without Vulkan headers.
// Requires a QGuiApplication; GUI thread only.
struct HeadlessRhi {
    RhiBackend backend;
    QString error;                                 // why create() failed, naming the backend
    std::shared_ptr<QOffscreenSurface> surface;
    std::shared_ptr<QVulkanInstance> vulkan;
    std::unique_ptr<QRhi> rhi;

    HeadlessRhi();
    ~HeadlessRhi();
    HeadlessRhi(const HeadlessRhi &) = delete;
    HeadlessRhi &operator=(const HeadlessRhi &) = delete;
    // Creates the QRhi on the selected backend and requires compute support.
    // `timestamps` requests QRhi::EnableTimestamps (GPU frame times).
    bool create(bool timestamps = false);
    // "GPU case skipped: <reason>" for a test skip or a tool error.
    std::string skipReason() const;
};

// Empty when the selected backend can create a QRhi with compute here; otherwise
// why not (names the backend). Probed once per process.
QString gpuUnavailableReason();
// One line for logs and test banners: backend, where it came from, device, or
// why GPU work will be skipped.
QString describeRhi();
} // namespace lab
