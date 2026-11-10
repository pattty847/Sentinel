#pragma once
#include "chart/layers/CvdLayer.hpp"
#include "render/ChartTextPrimitives.hpp"
#include <QSGGeometry>

namespace chart {
class FlowFooterLayer final : public IChartLayer {
public:
    explicit FlowFooterLayer(const CvdLayer& cvd);
    std::string_view name() const override { return "flowFooter"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void submitLabels(const ChartFrame&, LayerNodeContext&);
    bool drawing() const { return drawing_; }
    QRectF bounds() const { return bounds_; }
    double bottomInset() const { return bottomInset_; }
    int columnCount() const { return columns_; }
    const auto& glyphs() const { return glyphs_; }
private:
    struct Key {
        uint64_t revision = 0;
        qint64 atlas = 0;
        QRectF bounds;
        double left = 0, column = 0, timeframe = 0, dpr = 0, start = 0, end = 0;
        bool operator==(const Key&) const = default;
    };
    Key key_;
    bool built_ = false;
    static constexpr int kColumns = 1024, kGlyphs = kColumns * 18 + 16;
    using Vertex = QSGGeometry::ColoredPoint2D;
    double measure(const char*, const ChartTextAtlas&) const;
    void text(const char*, const QRectF&, const ChartTextAtlas&);
    void drawColumns(const ChartFrame&, const ChartTextAtlas&, Vertex*&);
    double widest(const ChartTextAtlas&) const;
    void column(int index, double x, double width, const ChartTextAtlas&, Vertex*&);
    const CvdLayer& cvd_;
    CvdPresentation captured_;
    QSGOpacityNode* gate_ = nullptr;
    QSGGeometryNode* node_ = nullptr;
    QRectF bounds_;
    FlowFooterLayout layout_;
    double maximum_ = 0, bottomInset_ = 0;
    bool drawing_ = false, reserved_ = false;
    int columns_ = 0, vertices_ = 0;
    std::vector<ChartGlyphInstance> glyphs_;
};
} // namespace chart
