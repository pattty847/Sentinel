#pragma once
#include "chart/layers/IChartLayer.hpp"
#include "chart/layers/CvdLayerLayout.hpp"
#include "chart/feeds/CvdFeed.hpp"
#include "render/ChartTextPrimitives.hpp"
#include <span>
class AxisTextService;
class QSGOpacityNode;
class QSGGeometryNode;
namespace chart {
class CvdLayer final : public IChartLayer {
public:
    CvdLayer();
    std::string_view name() const override { return "cvd"; }
    void onRootRebuilt() override;
    QSGNode* createContainer() override;
    void sync(const ChartFrame&) override;
    void updateNodes(const ChartFrame&, LayerNodeContext&) override;
    void submitLabels(const ChartFrame&, LayerNodeContext&);
    void bindAxes(AxisTextService* axes) { axes_ = axes; }
    CvdStripStyle& style() { return style_; }
    const CvdGutterValues& gutterValues() const { return values_; }
    QRectF gutter() const { return gutter_; }
    std::span<const ChartGlyphInstance> labelGlyphs() const { return glyphs_; }
    CvdMailbox& mailbox() { return mailbox_; }
    const CvdPresentation& presentation() const { return presentation_; }
    CvdPresentation& presentation() { return presentation_; }
    uint64_t snapshotRevision() const { return revision_; }
    bool drawing() const { return drawing_; }
    int vertexCount() const { return vertices_; }
    const CvdSnapshot& current() const { return *current_; }
    double bottomInset() const { return bottomInset_; }
    QRectF strip() const { return strip_; }
private:
    struct Key {
        double left = 0, column = 0, tf = 0, dpr = 0, start = 0, end = 0;
        QRectF strip, gutter;
        bool area = false;
        bool operator==(const Key&) const = default;
    };
    void geometry(const ChartFrame&);
    void addLabel(const char*, const QRectF&, const ChartTextAtlas&);
    CvdPresentation presentation_, captured_;
    CvdMailbox mailbox_{&presentation_};
    std::unique_ptr<CvdSnapshot> current_;
    QSGOpacityNode* gate_ = nullptr;
    QSGGeometryNode* node_ = nullptr;
    QRectF strip_, gutter_;
    AxisTextService* axes_ = nullptr;
    CvdStripStyle style_, capturedStyle_;
    CvdGutterValues values_;
    double bottomInset_ = 0;
    CvdScale scale_;
    bool drawing_ = false, reserved_ = false, dirty_ = true;
    Key key_;
    uint64_t revision_ = 0;
    int vertices_ = 0;
    std::vector<ChartGlyphInstance> glyphs_;
};
} // namespace chart
