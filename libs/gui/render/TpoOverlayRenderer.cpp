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
 *   neutral  (no letter): 0x8000  ← culled by neutralFloor (transparent)
 *   present  (any letter): value BELOW neutral in [0x6666, 0x1000], scaled
 *                           by letter index so each letter gets a unique
 *                           intensity.  The shader maps this to bid colour
 *                           (blue spectrum); 'A' is faint, 'Z' is bright.
 *
 * The letter-to-intensity mapping gives the GPU shader enough information to
 * distinguish letter brackets visually without needing MSDF glyphs.
 */
#include "TpoOverlayRenderer.hpp"
#include "TpoDebugTrace.hpp"

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
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Encode a letter byte to a Grayscale16 texture value.
// Returns 0x8000 (neutral / transparent) for '\0'.
//
// The FootprintIntensityNode shader maps texture values as:
//   signedDelta = (encoded - 0.5) * 2.0
//   signedDelta < 0  → bid color (blue)    ← letters go here
//   signedDelta >= 0 → ask color (gold)
//   magnitude ≤ neutralFloor → transparent (discarded)
//
// Letters encode BELOW 0x8000 (neutral) in range [0x6666, 0x1000]:
//   'A' (idx=0) → 0x6666: magnitude ≈ 0.20 (faint blue, clearly visible)
//   'Z' (idx=25) → ~0x1000: magnitude ≈ 0.88 (bright blue)
//
// The gap between neutral (magnitude ≈ 0.00002) and 'A' (0.20) is large
// enough that neutralFloor=0.05 reliably culls background without clipping
// any letter.  The previous range [0x7FFF, 0x0FFF] put 'A' only 1 bit below
// neutral (magnitude ≈ 0.00003), making early letters invisible and leaking
// a faint gray background when neutral cells barely passed the cull test.
inline uint16_t encodeLetterToU16(char letter) {
    if (letter == '\0') {
        return 0x8000u;  // neutral → discarded by neutralFloor
    }
    // Normalise to 0-25 index (wrap unknown chars to 0).
    int idx = 0;
    if (letter >= 'A' && letter <= 'Z') {
        idx = letter - 'A';
    } else if (letter >= 'a' && letter <= 'z') {
        idx = letter - 'a';
    }
    // Map [0,25] → [0x6666, 0x1000]: below neutral (0x8000) → bid → blue.
    // 'A' (idx=0) → 0x6666 (faint), 'Z' (idx=25) → ~0x1000 (bright).
    const uint16_t top  = 0x6666u;
    const uint16_t step = static_cast<uint16_t>((0x6666u - 0x1000u) / 25u);  // ≈ 884
    return static_cast<uint16_t>(top - static_cast<uint16_t>(idx) * step);
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
                                int64_t viewEndMs,
                                QRectF  surfaceBounds) {
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
        const qreal srcY = std::clamp(sourceRect.y(), 0.0, static_cast<qreal>(m_gridHeight - 1));
        const qreal srcH = std::clamp(sourceRect.height(), 1.0,
                                      static_cast<qreal>(m_gridHeight) - srcY);
        if (m_displayMode == TpoStreamState::DisplayMode::VerticalTimeline &&
            sessionStartMs > 0 && sessionEndMs > sessionStartMs &&
            viewStartMs > 0 && viewEndMs > viewStartMs) {
            // Map visible world-time interval inside session into texture X.
            const int64_t visibleStart = std::max(viewStartMs, sessionStartMs);
            const int64_t visibleEnd = std::min(viewEndMs, sessionEndMs);
            if (visibleEnd > visibleStart) {
                const double sessionSpanMs = static_cast<double>(sessionEndMs - sessionStartMs);
                const double x0 = (static_cast<double>(visibleStart - sessionStartMs) / sessionSpanMs) *
                                  static_cast<double>(m_gridWidth);
                const double x1 = (static_cast<double>(visibleEnd - sessionStartMs) / sessionSpanMs) *
                                  static_cast<double>(m_gridWidth);
                const qreal srcX = std::clamp(static_cast<qreal>(x0), 0.0, static_cast<qreal>(m_gridWidth - 1));
                const qreal srcW = std::clamp(static_cast<qreal>(x1 - x0), 1.0,
                                              static_cast<qreal>(m_gridWidth) - srcX);
                tpoSrcRect = QRectF(srcX, srcY, srcW, srcH);
            } else {
                tpoSrcRect = QRectF();
            }
        } else {
            const qreal srcX = std::clamp(sourceRect.x(), 0.0, static_cast<qreal>(m_gridWidth - 1));
            const qreal srcW = std::clamp(sourceRect.width(), 1.0, static_cast<qreal>(m_gridWidth) - srcX);
            tpoSrcRect = QRectF(srcX, srcY, srcW, srcH);
        }
    }

    // ── Draw rect: mode-specific positioning ────────────────────────────────
    // VerticalTimeline projects session time onto screen using surfaceBounds (full item area,
    // 0,0,w,h) + viewStart/End — the same base that candles and labels use.  drawRect is the
    // heatmap's ring-clipped overlap rect and must NOT be used here; doing so causes the session
    // to drift rightward as the ring buffer advances and to mis-scale relative to other layers.
    QRectF effectiveDrawRect = drawRect;
    if (m_displayMode == TpoStreamState::DisplayMode::VerticalTimeline &&
        sessionStartMs > 0 && sessionEndMs > sessionStartMs &&
        viewStartMs > 0    && viewEndMs > viewStartMs) {
        const QRectF projectionBase = (!surfaceBounds.isEmpty()) ? surfaceBounds : drawRect;
        effectiveDrawRect = computeTimelinedDrawRect(projectionBase,
                                                     sessionStartMs, sessionEndMs,
                                                     viewStartMs, viewEndMs);
    }

    // ── Material parameters ─────────────────────────────────────────────────
    m_node->setColor(QColor(42, 50, 60, 255));         // alpha=255: letter alpha driven by shaped intensity
    m_node->setBidColor(QColor(102, 180, 255, 255));   // blue – letters
    m_node->setAskColor(QColor(255, 196, 80,  255));   // gold – unused by TPO
    m_node->setNeutralFloor(0.05f);   // 0x8000 has magnitude≈0.00002; 0.05 culls background reliably
    m_node->setMagnitudeScale(1.0f);  // letters span [0.20, 0.88]; no extra boost needed
    m_node->setMagnitudeGamma(0.5f);  // sqrt lift: 'A' alpha≈0.45, 'Z' alpha≈0.94

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

    if (tpo_debug::enabled() && drawTpo) {
        static QElapsedTimer tpoOverlayLogTimer;
        static bool tpoOverlayLogTimerStarted = false;
        if (!tpoOverlayLogTimerStarted) {
            tpoOverlayLogTimer.start();
            tpoOverlayLogTimerStarted = true;
        }
        if (tpoOverlayLogTimer.elapsed() > 250) {
            std::ostringstream payload;
            payload << "{"
                    << "\"mode\":" << static_cast<int>(m_displayMode)
                    << ",\"gridWidth\":" << m_gridWidth
                    << ",\"gridHeight\":" << m_gridHeight
                    << ",\"drawRectX\":" << drawRect.x()
                    << ",\"drawRectW\":" << drawRect.width()
                    << ",\"effectiveDrawRectX\":" << effectiveDrawRect.x()
                    << ",\"effectiveDrawRectW\":" << effectiveDrawRect.width()
                    << ",\"sourceRectX\":" << sourceRect.x()
                    << ",\"sourceRectW\":" << sourceRect.width()
                    << ",\"tpoSrcRectX\":" << tpoSrcRect.x()
                    << ",\"tpoSrcRectW\":" << tpoSrcRect.width()
                    << ",\"sessionStartMs\":" << sessionStartMs
                    << ",\"sessionEndMs\":" << sessionEndMs
                    << ",\"viewStartMs\":" << viewStartMs
                    << ",\"viewEndMs\":" << viewEndMs
                    << "}";
            tpo_debug::append("TpoOverlayRenderer.cpp:render",
                              "tpo_overlay_mapping",
                              "H5",
                              payload.str());
            tpoOverlayLogTimer.restart();
        }
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
