#include "RhiBackend.hpp"
#include <QOffscreenSurface>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>
#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#define SENTINEL_LAB_VULKAN 1
#endif

namespace lab {
namespace {
// Qt's Metal QRhi aborts on a machine without an MTLDevice (sandboxed runs), so
// probe before creating one.
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

constexpr int kD3DFeatureLevel11_1 = 0xb100; // D3D_FEATURE_LEVEL_11_1

const char *platformDefault() {
#if defined(Q_OS_WIN)
    return "d3d11";
#elif defined(Q_OS_MACOS)
    return "metal";
#else
    return "opengl";
#endif
}
} // namespace

RhiSelection selectedRhiBackend() {
    RhiSelection out;
    const QString env = qEnvironmentVariable("SENTINEL_RHI_BACKEND").trimmed().toLower();
    auto &b = out.backend;
    b.fromEnv = !env.isEmpty();
    b.name = b.fromEnv ? env : QString::fromLatin1(platformDefault());
    auto unsupported = [&](const char *why) {
        out.error = QStringLiteral("%1 backend %2").arg(b.name, QString::fromLatin1(why));
        return out;
    };
    if (b.name == "d3d11" || b.name == "d3d12") {
#ifdef Q_OS_WIN
        b.graphicsApi = b.name == "d3d12" ? QSGRendererInterface::Direct3D12 : QSGRendererInterface::Direct3D11;
#else
        return unsupported("is Windows only");
#endif
    } else if (b.name == "vulkan") {
#ifdef SENTINEL_LAB_VULKAN
        b.graphicsApi = QSGRendererInterface::Vulkan;
#else
        return unsupported("is not in this build (Qt without Vulkan, or no Vulkan headers at build time)");
#endif
    } else if (b.name == "opengl") {
#if QT_CONFIG(opengl)
        b.graphicsApi = QSGRendererInterface::OpenGL;
#else
        return unsupported("is not in this build (Qt without OpenGL)");
#endif
    } else if (b.name == "metal") {
#if QT_CONFIG(metal)
        b.graphicsApi = QSGRendererInterface::Metal;
#else
        return unsupported("is Apple only");
#endif
    } else {
        out.error = QStringLiteral("SENTINEL_RHI_BACKEND=%1 is not one of d3d11, d3d12, vulkan, opengl, metal").arg(env);
        return out;
    }
    out.valid = true;
    return out;
}

HeadlessRhi::HeadlessRhi() = default;
HeadlessRhi::~HeadlessRhi() {
    rhi.reset(); // before the surface or instance it was created with
    vulkan.reset();
    surface.reset();
}

bool HeadlessRhi::create(bool timestamps) {
    const RhiSelection selection = selectedRhiBackend();
    backend = selection.backend;
    auto fail = [&](const QString &why) {
        rhi.reset();
        error = why;
        return false;
    };
    if (!selection.valid) return fail(selection.error);
    const QString name = backend.name;
    const QRhi::Flags flags = timestamps ? QRhi::EnableTimestamps : QRhi::Flags();
    switch (backend.graphicsApi) {
#ifdef Q_OS_WIN
    case QSGRendererInterface::Direct3D11: {
        QRhiD3D11InitParams init;
        init.enableDebugLayer = qEnvironmentVariableIntValue("SENTINEL_RHI_DEBUG_LAYER") != 0;
        // Ask for feature level 11_1: the heatmap kernel binds more than the 8
        // UAVs that 11_0 allows per compute shader (Qt maps every storage buffer
        // to a UAV). Qt's default device creation never selects 11_1.
        QRhiD3D11NativeHandles level;
        level.featureLevel = kD3DFeatureLevel11_1;
        rhi.reset(QRhi::create(QRhi::D3D11, &init, flags, &level));
        if (!rhi) rhi.reset(QRhi::create(QRhi::D3D11, &init, flags));
        break;
    }
    case QSGRendererInterface::Direct3D12: {
        QRhiD3D12InitParams init;
        init.enableDebugLayer = qEnvironmentVariableIntValue("SENTINEL_RHI_DEBUG_LAYER") != 0;
        rhi.reset(QRhi::create(QRhi::D3D12, &init, flags));
        break;
    }
#endif
#ifdef SENTINEL_LAB_VULKAN
    case QSGRendererInterface::Vulkan: {
        vulkan = std::make_shared<QVulkanInstance>();
        vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (!vulkan->create())
            return fail(QStringLiteral("vulkan backend: QVulkanInstance::create failed (no Vulkan loader or driver)"));
        QRhiVulkanInitParams init;
        init.inst = vulkan.get();
        rhi.reset(QRhi::create(QRhi::Vulkan, &init, flags));
        break;
    }
#endif
#if QT_CONFIG(opengl)
    case QSGRendererInterface::OpenGL: {
        surface.reset(QRhiGles2InitParams::newFallbackSurface());
        QRhiGles2InitParams init;
        init.fallbackSurface = surface.get();
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &init, flags));
        break;
    }
#endif
#if QT_CONFIG(metal)
    case QSGRendererInterface::Metal: {
        if (!metalDeviceAvailable()) return fail(QStringLiteral("metal backend: no MTLDevice (Metal unavailable)"));
        QRhiMetalInitParams init;
        rhi.reset(QRhi::create(QRhi::Metal, &init, flags));
        break;
    }
#endif
    default:
        return fail(QStringLiteral("%1 backend is not supported by this harness").arg(name));
    }
    if (!rhi) return fail(QStringLiteral("%1 backend: QRhi::create failed (no usable device or driver)").arg(name));
    if (!rhi->isFeatureSupported(QRhi::Compute))
        return fail(QStringLiteral("%1 backend: device %2 has no compute support")
                        .arg(name, QString::fromUtf8(rhi->driverInfo().deviceName)));
    error.clear();
    return true;
}

std::string HeadlessRhi::skipReason() const {
    return ("GPU case skipped: " + error).toStdString();
}

QString gpuUnavailableReason() {
    static const QString reason = [] {
        HeadlessRhi probe;
        return probe.create() ? QString() : probe.error;
    }();
    return reason;
}

QString describeRhi() {
    const RhiSelection selection = selectedRhiBackend();
    const QString source = selection.backend.fromEnv ? QStringLiteral("SENTINEL_RHI_BACKEND")
                                                     : QStringLiteral("platform default");
    HeadlessRhi probe;
    if (!probe.create())
        return QStringLiteral("rhi backend=%1 (%2): GPU cases will SKIP: %3")
            .arg(selection.backend.name, source, probe.error);
    const auto info = probe.rhi->driverInfo();
    QString extra;
#ifdef Q_OS_WIN
    if (probe.backend.graphicsApi == QSGRendererInterface::Direct3D11)
        if (const auto *h = static_cast<const QRhiD3D11NativeHandles *>(probe.rhi->nativeHandles()))
            extra = QStringLiteral(" featureLevel=0x%1").arg(h->featureLevel, 0, 16);
#endif
    return QStringLiteral("rhi backend=%1 (%2) device=\"%3\" driverApi=%4%5")
        .arg(probe.backend.name, source, QString::fromUtf8(info.deviceName), QString::fromLatin1(probe.rhi->backendName()), extra);
}
} // namespace lab
