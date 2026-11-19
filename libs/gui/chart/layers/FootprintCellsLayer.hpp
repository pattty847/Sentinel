#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/feeds/FlowFeed.hpp"
#include "chart/core/FootprintLod.hpp"
#include "chart/core/ChartTextRule.hpp"
#include "render/ChartTextPrimitives.hpp"
#include <span>
#include <QSGGeometry>
#include <QString>

class QSGOpacityNode;
class QSGGeometryNode;
namespace chart {
class FootprintCellsLayer final : public IChartLayer {
public:
    FootprintCellsLayer();
    std::string_view name() const override { return "footprintCells"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    // Numbers follow the chart text rule: all of this frame's numbers or none.
    void submitLabels(const ChartFrame&, LayerNodeContext&, double bottomInset = 0, const ChartTextRule& rule = {});
    const ChartTextFit& textFit() const { return fit_; }
    FlowMailbox& mailbox() { return mailbox_; }
    FootprintPresentation& presentation() { return presentation_; }
    FootprintStage stage() const { return stage_; }
    const FlowSnapshot& current() const { return *current_; }
    bool drawing() const { return drawing_; }
    bool drawingNumbers() const { return drawing_ && stage_ == FootprintStage::Numbers && !glyphs_.empty(); }
    QString emptyStateText(const ChartFrame&) const;
    int vertexCount() const { return vertices_; }
    size_t glyphCount() const { return glyphs_.size(); }
    std::span<const QRectF> cards() const { return {cards_.data(), drawing_ ? cardCount_ : 0}; }
    const auto& glyphs() const { return glyphs_; }
private:
    struct Key {
        double top = 0, left = 0, row = 0, column = 0, tick = 0, dpr = 0;
        QRectF bounds;
        bool window = false;
        double dust = 0;
        FootprintStage stage = FootprintStage::Hidden;
        bool operator==(const Key&) const = default;
    };
    static constexpr int kMaxGlyphs = 32000;
    // More wanted numbers than this draw none (all or none): no screen fits this many readable numbers.
    static constexpr int kMaxNumbers = kMaxGlyphs / 3;
    void cells(const ChartFrame&);
    double windowMaximum(const ChartFrame&) const;
    struct Number {
        double volume, fillAlpha; QRectF box; FlowTier tier; bool bid;
        std::array<std::array<char, 16>, kNumberForms> text; // this frame's text per form
        std::array<uint8_t, kNumberForms> length;
    };
    bool collectNumbers(const ChartFrame&, double bottomInset);
    bool measureNumbers(ChartTextDemand&, double& cellW, double& cellH);
    void labels(const ChartFrame&, const ChartTextAtlas&, double bottomInset, const ChartTextRule&);
    double normaliser(const FlowColumn&) const;
    void number(const Number&, const ChartTextAtlas&, double dpr);
    QRectF cardRect(const ChartFrame&, const FlowColumn&) const;
    void rowGeometry(QSGGeometry::ColoredPoint2D*, int&, const ChartFrame&, const FlowColumn&, const FlowCell&);
    void stripGeometry(QSGGeometry::ColoredPoint2D*, int&, const ChartFrame&, const FlowColumn&, const FlowCell&);
    QRectF cellRect(const ChartFrame&, const FlowColumn&, const FlowCell&) const;
    FootprintPresentation presentation_;
    FlowMailbox mailbox_{&presentation_.coverage};
    std::unique_ptr<FlowSnapshot> current_;
    FootprintStage stage_ = FootprintStage::Hidden;
    QSGOpacityNode* gate_ = nullptr;
    QSGGeometryNode* node_ = nullptr;
    int vertices_ = 0, capacity_ = 0;
    bool drawing_ = false, reserved_ = false, built_ = false, labelsDirty_ = true;
    Key key_;
    qint64 atlasKey_ = 0;
    double labelBottomInset_ = 0;
    std::vector<ChartGlyphInstance> glyphs_;
    std::vector<Number> numbers_;
    ChartTextRule labelRule_;
    ChartTextFit fit_;
    int64_t fitTimeframeMs_ = 0;                   // the decision belongs to this timeframe, tick
    double fitTick_ = 0;                           // and scope generation (symbol)
    uint64_t fitGeneration_ = 0;
    std::array<QRectF, FlowSnapshot::kMaxBars> cards_{};
    size_t cardCount_ = 0;
};
} // namespace chart
