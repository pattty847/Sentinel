#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/feeds/FlowFeed.hpp"
#include "chart/core/FootprintLod.hpp"
#include "render/ChartTextPrimitives.hpp"

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
    void submitLabels(const ChartFrame&, LayerNodeContext&);
    FlowMailbox& mailbox() { return mailbox_; }
    FootprintPresentation& presentation() { return presentation_; }
    FootprintStage stage() const { return stage_; }
    const FlowSnapshot& current() const { return *current_; }
    bool drawing() const { return drawing_; }
    int vertexCount() const { return vertices_; }
    size_t glyphCount() const { return glyphs_.size(); }
    static double intensity(const FlowColumn&, const FlowCell&, const FlowSnapshot&, bool window);
private:
    struct Key {
        double top = 0, left = 0, row = 0, column = 0, tick = 0, dpr = 0;
        QRectF bounds;
        bool window = false;
        bool operator==(const Key&) const = default;
    };
    static constexpr int kMaxGlyphs = 32000;
    void cells(const ChartFrame&);
    void labels(const ChartFrame&, const ChartTextAtlas&);
    void number(double, const QRectF&, const ChartTextAtlas&);
    QRectF cellRect(const ChartFrame&, const FlowColumn&, const FlowCell&) const;
    FlowMailbox mailbox_;
    std::unique_ptr<FlowSnapshot> current_;
    FootprintPresentation presentation_;
    FootprintStage stage_ = FootprintStage::Hidden;
    QSGOpacityNode* gate_ = nullptr;
    QSGGeometryNode* node_ = nullptr;
    int vertices_ = 0, capacity_ = 0;
    bool drawing_ = false, reserved_ = false, built_ = false, labelsDirty_ = true;
    Key key_;
    qint64 atlasKey_ = 0;
    std::vector<ChartGlyphInstance> glyphs_;
};
} // namespace chart
