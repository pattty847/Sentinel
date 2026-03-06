/*
 * Sentinel – TpoOverlayRenderer
 *
 * GPU-upload renderer for TPO letter profiles (Mode B).
 * Supports two display modes:
 *
 *   HorizontalProfile  – classic Market Profile rank-indexed display.
 *     Profile is anchored at x=0 in the texture; drawRect covers the full
 *     chart area.  Each texture column = one "rank level" of the profile.
 *
 *   VerticalTimeline   – time-aligned letter display.
 *     drawRect is clipped to the visible portion of [sessionStartMs,
 *     sessionEndMs], so TPO columns line up with their corresponding candles.
 *
 * Letter encoding in the Grayscale16 texture:
 *   neutral  (no letter): 0x8000  ← maps to mid-grey in the shader
 *   present  (any letter): value above neutral, scaled by letter position
 *                           in the A-Z alphabet so each letter gets a unique
 *                           intensity.  The shader maps this to bid colour
 *                           (blue spectrum).
 *
 * The letter-to-intensity mapping gives the GPU shader enough information to
 * distinguish letter brackets visually without needing MSDF glyphs.
 */
#include "TpoOverlayRenderer.hpp"

#include "FootprintIntensityNode.hpp"
#include "SentinelLogging.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Encode a letter byte to a Grayscale16 texture value.
// Returns 0x8000 (neutral) for '\0', otherwise maps 'A'–'Z' / 'a'–'z'
// to a range above neutral so the shader can colourise by bracket.
inline uint16_t encodeLetterToU16(char letter) {
    if (letter == '\0') {
        return 0x8000u;
    }
    // Normalise to 0-25 index (wrap unknown chars to 0).
    int idx = 0;
    if (letter >= 'A' && letter <= 'Z') {
        idx = letter - 'A';
    } else if (letter >= 'a' && letter <= 'z') {
        idx = letter - 'a';
    }
    // Map 0-25 to the range [0x9000, 0xFFFF] so the shader sees a strong
    // positive value for every letter.  Increment is 0x2EEF (~12015).
    const uint16_t base = 0x9000u;
    const uint16_t step = static_cast<uint16_t>(0x6FFFu / 25);
    return static_cast<uint16_t>(base + static_cast<uint16_t>(idx) * step);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void TpoOverlayRenderer::onRootRebuilt() {
    m_node = nullptr;
    m_lastWriteColumn = -1;
    m_textureDirty = true;
}

void TpoOverlayRenderer::setDisplayMode(TpoStreamState::DisplayMode mode) {
    if (m_displayMode != mode) {
        m_displayMode  = mode;
        m_textureDirty = true;
        // Null the node so the texture is rebuilt on next render call.
        m_node = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  VerticalTimeline: compute the clipped draw rect
// ─────────────────────────────────────────────────────────────────────────────
QRectF TpoOverlayRenderer::computeTimelinedDrawRect(const QRectF& fullDrawRect,
                                                     int64_t sessionStartMs,
                                                     int64_t sessionEndMs,
                                                     int64_t viewStartMs,
                                                     int64_t viewEndMs) const {
    if (viewEndMs <= viewStartMs || sessionEndMs <= sessionStartMs) {
        return QRectF();
    }

    const double viewSpan    = static_cast<double>(viewEndMs    - viewStartMs);
    const double sessStart   = static_cast<double>(sessionStartMs);
    const double sessEnd     = static_cast<double>(sessionEndMs);
    const double visStart    = static_cast<double>(viewStartMs);

    // Fraction of the view width where the session starts and ends.
    const double fracLeft    = (sessStart - visStart) / viewSpan;
    const double fracRight   = (sessEnd   - visStart) / viewSpan;

    const double drawLeft    = fullDrawRect.left()  + fracLeft  * fullDrawRect.width();
    const double drawRight   = fullDrawRect.left()  + fracRight * fullDrawRect.width();

    // Clamp to visible area.
    const double clampedLeft  = std::clamp(drawLeft,  fullDrawRect.left(),  fullDrawRect.right());
    const double clampedRight = std::clamp(drawRight, fullDrawRect.left(),  fullDrawRect.right());
    if (clampedRight <= clampedLeft) {
        return QRectF();
    }

    return QRectF(clampedLeft, fullDrawRect.top(),
                  clampedRight - clampedLeft, fullDrawRect.height());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main render hot-path
// ─────────────────────────────────────────────────────────────────────────────
void TpoOverlayRenderer::render(QQuickWindow* window,
                                QSGNode* parentNode,
                                bool drawTpo,
                                bool forceFull,
                                float timeOffset,
                                const QRectF& drawRect,
                                const QRectF& sourceRect,
                                int sharedGridWidth,
                                int sharedGridHeight,
                                std::vector<PendingUpload>& pendingUploads,
                                int64_t sessionStartMs,
                                int64_t sessionEndMs,
                                int64_t viewStartMs,
                                int64_t viewEndMs) {
    if (!window || !parentNode) {
        return;
    }
    const auto* rendererInterface = window->rendererInterface();
    const bool useIncrementalGlUploads = rendererInterface &&
        rendererInterface->graphicsApi() == QSGRendererInterface::OpenGL;

    const bool hasPending = !pendingUploads.empty();

    if (qEnvironmentVariableIsSet("SENTINEL_CHART_DEBUG")) {
        static QElapsedTimer tpoDebugTimer;
        static bool tpoDebugTimerStarted = false;
        if (!tpoDebugTimerStarted) {
            tpoDebugTimer.start();
            tpoDebugTimerStarted = true;
        }
        if (tpoDebugTimer.elapsed() > 1000) {
            sLog_Debug(
                QString("TPO overlay: mode=%1 draw=%2 pending=%3 grid=%4x%5")
                    .arg(static_cast<int>(m_displayMode))
                    .arg(drawTpo ? 1 : 0)
                    .arg(static_cast<int>(pendingUploads.size()))
                    .arg(m_gridWidth)
                    .arg(m_gridHeight));
            tpoDebugTimer.restart();
        }
    }

    // ── Grid dimension sync ─────────────────────────────────────────────────
    if (hasPending) {
        for (auto it = pendingUploads.rbegin(); it != pendingUploads.rend(); ++it) {
            if (it->gridWidth > 0 && it->gridHeight > 0) {
                if (m_gridWidth != it->gridWidth || m_gridHeight != it->gridHeight) {
                    m_gridWidth    = it->gridWidth;
                    m_gridHeight   = it->gridHeight;
                    m_textureDirty = true;
                }
                break;
            }
        }
        for (const auto& upload : pendingUploads) {
            if (upload.gridWidth == m_gridWidth && upload.gridHeight == m_gridHeight &&
                upload.x >= 0 && upload.x < m_gridWidth) {
                m_lastWriteColumn = upload.x;
            }
        }
    }

    if ((m_gridWidth <= 0 || m_gridHeight <= 0) && sharedGridWidth > 0 && sharedGridHeight > 0) {
        m_gridWidth    = sharedGridWidth;
        m_gridHeight   = sharedGridHeight;
        m_textureDirty = true;
    }

    if (!drawTpo && !hasPending) {
        if (m_node) {
            m_node->setRect(QRectF());
        }
        return;
    }

    // ── Ensure GPU node ─────────────────────────────────────────────────────
    if (!m_node) {
        m_node = new FootprintIntensityNode();
        parentNode->appendChildNode(m_node);
        m_textureDirty = true;
    }

    // ── CPU-side texture update (non-GL incremental path) ───────────────────
    if (!useIncrementalGlUploads && hasPending && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        if (!m_image.isNull()) {
            for (const auto& upload : pendingUploads) {
                if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                    continue;
                }
                if (upload.x < 0 || upload.x >= m_gridWidth ||
                    upload.letters.size() != m_gridHeight) {
                    continue;
                }
                for (int y = 0; y < m_gridHeight; ++y) {
                    const uint16_t encoded =
                        qToLittleEndian<uint16_t>(encodeLetterToU16(upload.letters.at(y)));
                    auto* row = reinterpret_cast<uint16_t*>(m_image.scanLine(y));
                    row[upload.x] = encoded;
                }
            }
            m_textureDirty = true;
        }
    }

    // ── Texture refresh ─────────────────────────────────────────────────────
    const bool textureMissing    = !m_node->hasTexture() || m_node->textureSize().isEmpty();
    const bool needTextureRefresh = m_textureDirty || (drawTpo && textureMissing);
    if (needTextureRefresh && m_gridWidth > 0 && m_gridHeight > 0) {
        ensureImage();
        if (!m_image.isNull()) {
            auto* tpoTexture = window->createTextureFromImage(m_image);
            if (!tpoTexture) {
                const QImage fallback = m_image.convertToFormat(QImage::Format_Grayscale16);
                tpoTexture = window->createTextureFromImage(fallback);
            }
            if (tpoTexture) {
                tpoTexture->setFiltering(QSGTexture::Nearest);
                m_node->setTexture(tpoTexture);
                m_textureDirty = false;
            } else {
                m_textureDirty = true;
            }
        } else {
            m_textureDirty = true;
        }
    }

    // ── Source rect (Y range always from shared viewport) ──────────────────
    QRectF tpoSrcRect(0, 0, m_gridWidth, m_gridHeight);
    if (sourceRect.width() > 0.0 && sourceRect.height() > 0.0) {
        const qreal srcX = std::clamp(sourceRect.x(), 0.0, static_cast<qreal>(m_gridWidth - 1));
        const qreal srcW = std::clamp(sourceRect.width(), 1.0,
                                      static_cast<qreal>(m_gridWidth) - srcX);
        const qreal srcY = std::clamp(sourceRect.y(), 0.0, static_cast<qreal>(m_gridHeight - 1));
        const qreal srcH = std::clamp(sourceRect.height(), 1.0,
                                      static_cast<qreal>(m_gridHeight) - srcY);
        tpoSrcRect = QRectF(srcX, srcY, srcW, srcH);
    }

    // ── Draw rect: mode-specific positioning ────────────────────────────────
    QRectF effectiveDrawRect = drawRect;
    if (m_displayMode == TpoStreamState::DisplayMode::VerticalTimeline &&
        sessionStartMs > 0 && sessionEndMs > sessionStartMs &&
        viewStartMs > 0    && viewEndMs > viewStartMs) {
        effectiveDrawRect = computeTimelinedDrawRect(drawRect,
                                                     sessionStartMs, sessionEndMs,
                                                     viewStartMs, viewEndMs);
    }

    // ── Material parameters ─────────────────────────────────────────────────
    m_node->setColor(QColor(42, 50, 60, 180));
    m_node->setBidColor(QColor(102, 180, 255, 255));
    m_node->setAskColor(QColor(255, 196, 80, 255));
    m_node->setNeutralFloor(0.01f);
    m_node->setMagnitudeScale(1.0f);
    m_node->setMagnitudeGamma(0.65f);

    Q_UNUSED(forceFull);
    Q_UNUSED(timeOffset);
    m_node->setTimeOffset(0.0f);

    if (drawTpo && !effectiveDrawRect.isEmpty()) {
        m_node->setRect(effectiveDrawRect);
        m_node->setSourceRect(tpoSrcRect);
    } else {
        m_node->setRect(QRectF());
        m_node->setSourceRect(QRectF());
    }

    // ── GL incremental column upload ────────────────────────────────────────
    if (useIncrementalGlUploads && hasPending) {
        for (auto& upload : pendingUploads) {
            if (upload.gridWidth != m_gridWidth || upload.gridHeight != m_gridHeight) {
                continue;
            }
            if (upload.x < 0 || upload.x >= m_gridWidth ||
                upload.letters.size() != m_gridHeight) {
                continue;
            }
            QByteArray encoded;
            encoded.resize(m_gridHeight * static_cast<int>(sizeof(uint16_t)));
            auto* dst = reinterpret_cast<uint16_t*>(encoded.data());
            for (int y = 0; y < m_gridHeight; ++y) {
                dst[y] = qToLittleEndian<uint16_t>(encodeLetterToU16(upload.letters.at(y)));
            }
            m_node->enqueueColumn(upload.x, std::move(encoded));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  CPU-side texture backing store
// ─────────────────────────────────────────────────────────────────────────────
void TpoOverlayRenderer::ensureImage() {
    if (!m_image.isNull() &&
        m_image.width()  == m_gridWidth  &&
        m_image.height() == m_gridHeight &&
        m_image.format() == QImage::Format_Grayscale16) {
        return;
    }
    m_image = QImage(m_gridWidth, m_gridHeight, QImage::Format_Grayscale16);
    if (m_image.isNull()) {
        return;
    }
    // Initialise to neutral (no letter present at any position).
    for (int y = 0; y < m_image.height(); ++y) {
        auto* row = reinterpret_cast<uint16_t*>(m_image.scanLine(y));
        for (int x = 0; x < m_image.width(); ++x) {
            row[x] = qToLittleEndian<uint16_t>(0x8000u);
        }
    }
}
