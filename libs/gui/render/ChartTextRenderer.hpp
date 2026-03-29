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
    void beginFrame(QSGNode* parentNode, QQuickWindow* window, const ChartTextAtlas& atlas);
    void submitRun(const ChartTextRun& run, Priority priority);
    void submitGlyphs(const std::vector<ChartGlyphInstance>& glyphs, Priority priority);
    void endFrame();
    int droppedGlyphs() const { return m_droppedGlyphs; }
    int droppedHighGlyphs() const { return m_droppedHighGlyphs; }
    int droppedLowGlyphs() const { return m_droppedLowGlyphs; }

private:
    struct Bucket {
        QColor color;
        std::vector<ChartGlyphInstance> glyphs;
        ChartTextNode* node = nullptr;
        bool used = false;
    };

    Bucket* findOrCreateBucket(const QColor& color);
    bool canAccept(int glyphCount, Priority priority);

    QSGNode* m_root = nullptr;
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
