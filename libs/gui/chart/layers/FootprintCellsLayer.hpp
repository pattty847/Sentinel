#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/feeds/FlowFeed.hpp"
#include "chart/core/FootprintLod.hpp"
#include "render/ChartTextPrimitives.hpp"
#include <span>
#include <QSGGeometry>

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
    void submitLabels(const ChartFrame&, LayerNodeContext&, double bottomInset = 0);
    FlowMailbox& mailbox() { return mailbox_; }
    FootprintPresentation& presentation() { return presentation_; }
    FootprintStage stage() const { return stage_; }
    const FlowSnapshot& current() const { return *current_; }
    bool drawing() const { return drawing_; }
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
    void cells(const ChartFrame&);
    double windowMaximum(const ChartFrame&) const;
    void labels(const ChartFrame&, const ChartTextAtlas&, double bottomInset);
    struct Number { double volume, price, maximum; QRectF box; bool bid; };
    void number(const Number&, const ChartTextAtlas&);
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
    std::array<QRectF, FlowSnapshot::kMaxBars> cards_{};
    size_t cardCount_ = 0;
};
} // namespace chart
