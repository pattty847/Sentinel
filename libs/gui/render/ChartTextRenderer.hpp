#pragma once

#include <QColor>
#include <QHash>
#include <QSGNode>
#include <vector>

#include "ChartTextAtlas.hpp"
#include "ChartTextDebugNode.hpp"
#include "ChartTextLayout.hpp"
#include "ChartTextNode.hpp"

class QQuickWindow;

class ChartTextRenderer {
public:
    enum class Priority {
        High,
        Low,
    };

    void onRootRebuilt();
    QSGNode* createRoot();
    void beginFrame(QSGNode* parentNode, QQuickWindow* window, const ChartTextAtlas& atlas);
    void submitRun(const ChartTextRun& run, Priority priority);
    void submitGlyphs(const std::vector<ChartGlyphInstance>& glyphs, Priority priority);
    void endFrame();
    // Between beginFrame and endFrame only (false otherwise; retry next frame): makes the
    // colour's bucket and node now, with its atlas texture, material and room for
    // `glyphs` glyphs, so that colour's first use on a later frame allocates nothing.
    // A reservation not larger than what the bucket holds is a no-op.
    bool reserve(const QColor& color, int glyphs);
    int droppedGlyphs() const { return m_droppedGlyphs; }
    int droppedHighGlyphs() const { return m_droppedHighGlyphs; }
    int droppedLowGlyphs() const { return m_droppedLowGlyphs; }
    // Bytes held by the colour buckets (tests: no growth in steady state).
    size_t capacityBytes() const;
    // Tests: glyphs accepted this frame, glyph capacity over every bucket's node, and
    // the buckets whose node holds an atlas texture.
    int submittedGlyphs() const { return m_usedGlyphs; }
    int nodeCapacityGlyphs() const;
    int textureCount() const;

private:
    struct Bucket {
        QColor color;
        std::vector<ChartGlyphInstance> glyphs;
        ChartTextNode* node = nullptr;
        bool used = false;
    };

    struct ShaderParams {
        float sdfBias, distanceSign, sprFloor;
    };
    Bucket* findOrCreateBucket(const QColor& color);
    void prepareNode(Bucket& bucket, const ShaderParams& params);
    bool canAccept(int glyphCount, Priority priority);

    QSGNode* m_root = nullptr;
    bool m_inFrame = false; // between beginFrame and endFrame (reserve is allowed only then)
    QQuickWindow* m_window = nullptr;
    const ChartTextAtlas* m_atlas = nullptr;
    std::vector<Bucket> m_buckets;
    int m_maxGlyphs = 48000;
    int m_usedGlyphs = 0;
    int m_droppedGlyphs = 0;
    int m_droppedHighGlyphs = 0;
    int m_droppedLowGlyphs = 0;
    ChartTextDebugNode* m_debugNode = nullptr;
    std::vector<ChartGlyphInstance> m_debugGlyphs;
};
