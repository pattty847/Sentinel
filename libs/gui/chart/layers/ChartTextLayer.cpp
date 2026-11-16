#include "chart/layers/ChartTextLayer.hpp"
#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/layers/FootprintCellsLayer.hpp"
#include "chart/layers/TradeLayers.hpp"
#include "chart/layers/CvdLayer.hpp"
#include "chart/layers/FlowFooterLayer.hpp"
#include "chart/layers/RenderSequence.hpp"
#include "chart/core/ChartFrame.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/shell/CandleBufferSource.hpp"
#include "chart/shell/LayerVisibility.hpp"
#include "render/FrameProfiler.hpp"
#include "config/GuiConfigStore.hpp"
#include "SentinelLogging.hpp"
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <algorithm>

namespace {
bool envFloatValue(const char *name, float &out) {
  const QByteArray value = qgetenv(name);
  if (value.isEmpty()) {
    return false;
  }
  bool ok = false;
  const float parsed = value.toFloat(&ok);
  if (!ok) {
    return false;
  }
  out = parsed;
  return true;
}

bool envIntValue(const char *name, int &out) {
  const QByteArray value = qgetenv(name);
  if (value.isEmpty()) {
    return false;
  }
  bool ok = false;
  const int parsed = value.toInt(&ok);
  if (!ok) {
    return false;
  }
  out = parsed;
  return true;
}
bool candleEnvelopeCrosses(const std::vector<chart::CandleBar>& bars, int64_t windowEnd,
                   const chart_raster::RasterCamera& raster, const QRectF& plot, const QRectF& box) {
  if (!(raster.dpr > 0) || !(raster.tick > 0) || !(raster.tfMs > 0)) return false;
  const auto crosses = [&](double from, double to, double high, double low) {
    const double x1 = plot.left() + raster.xDev(from) / raster.dpr;
    const double x2 = plot.left() + raster.xDev(to) / raster.dpr;
    if (std::max(x1, x2) < box.left() - 3 || std::min(x1, x2) > box.right() + 3) return false;
    const double y1 = plot.top() + raster.yDev(high) / raster.dpr;
    const double y2 = plot.top() + raster.yDev(low) / raster.dpr;
    return std::max(y1, y2) >= box.top() - 3 && std::min(y1, y2) <= box.bottom() + 3;
  };
  for (size_t i = 0; i < bars.size(); ++i) {
    const auto& bar = bars[i];
    if (crosses(bar.timeStartMs, bar.timeEndMs, bar.high, bar.low)) return true;
    // The candle overlay carries the previous close through covered quiet buckets.
    // Guard that span conservatively so a carried wick cannot cross the label.
    const double next = i + 1 < bars.size() ? bars[i + 1].timeStartMs : windowEnd;
    if (next > bar.timeEndMs && crosses(bar.timeEndMs, next, bar.close, bar.close)) return true;
  }
  return false;
}
bool fitsEmptyRun(const FrameContext& frame, const ChartTextAtlas& atlas, const ChartTextRun& run,
                  const std::vector<chart::CandleBar>& bars, int64_t candleEnd, double bottom) {
  QRectF box;
  return ChartTextLayout::measureRunRect(atlas, run, box) && frame.surfaceBounds.contains(box) &&
         box.bottom() <= bottom && !candleEnvelopeCrosses(bars, candleEnd, frame.raster, frame.surfaceBounds, box);
}
} // namespace

namespace chart {
void ChartTextLayer::onRootRebuilt() {
    renderer.onRootRebuilt();
    plotIdPill_ = nullptr;
    plotIdPillRect_ = {};
    plotIdPillInitialized_ = false;
}

QSGNode* ChartTextLayer::createContainer() {
    auto* root = renderer.createRoot();
    plotIdPill_ = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 6);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
    plotIdPill_->setGeometry(geometry);
    plotIdPill_->setFlag(QSGNode::OwnsGeometry);
    plotIdPill_->setMaterial(new QSGVertexColorMaterial);
    plotIdPill_->setFlag(QSGNode::OwnsMaterial);
    root->appendChildNode(plotIdPill_);
    updatePlotIdPill({});
    return root;
}

bool ChartTextLayer::candleCrosses(const std::vector<CandleBar>& bars, int64_t windowEnd,
                                   const chart_raster::RasterCamera& raster, const QRectF& plot,
                                   const QRectF& box) {
    return candleEnvelopeCrosses(bars, windowEnd, raster, plot, box);
}

void ChartTextLayer::bindCandles(ChartViewController& view, CandleBufferSource& source, LayerVisibility&) {
    candleView_ = &view;
    candleSource_ = &source;
    auto refresh = [this] { refreshCandles(); };
    QObject::connect(view.viewPtr(), &GridViewState::viewportChanged, &view, refresh);
    QObject::connect(view.viewPtr(), &GridViewState::panVisualOffsetChanged, &view, refresh);
    QObject::connect(&view, &ChartViewController::timeframeChanged, &view, refresh);
    refreshCandles();
}

void ChartTextLayer::refreshCandles() {
    if (!candleView_) return;
    const auto& session = candleView_->session();
    if (session.symbol != plotSymbol_ || session.timeframeMs != plotTimeframeMs_) {
        plotSymbol_ = session.symbol;
        plotTimeframeMs_ = session.timeframeMs;
        std::lock_guard lock(candleMutex_);
        pendingPlotId_ = formatPlotId(plotSymbol_, plotTimeframeMs_);
        plotIdPending_ = true;
    }
    if (!candleSource_) return;
    ++candleRefreshCount_;
    auto* view = candleView_->viewPtr();
    const auto [start, end] = view->displayedTimeWindow();
    const int64_t tf = candleView_->session().timeframeMs;
    captureCandles_.clear();
    if (candleSource_->available() && end > start && tf > 0)
        candleSource_->visibleBars(candleView_->session().symbol, tf, start - tf, end, captureCandles_);
    std::lock_guard lock(candleMutex_);
    pendingCandles_.swap(captureCandles_);
    pendingEnd_ = end;
    candlePending_ = true;
}

void ChartTextLayer::candlesDirty(const QString& symbol, int64_t timeframeMs, int64_t start, int64_t end) {
    if (!candleView_ || symbol != candleView_->session().symbol ||
        timeframeMs != candleView_->session().timeframeMs) return;
    const auto [visibleStart, visibleEnd] = candleView_->viewPtr()->displayedTimeWindow();
    if (end > visibleStart && start < visibleEnd) refreshCandles();
}

void ChartTextLayer::setCandleEnvelope(std::vector<CandleBar> bars, int64_t end) {
    std::lock_guard lock(candleMutex_);
    pendingCandles_.swap(bars);
    pendingEnd_ = end;
    candlePending_ = true;
}

void ChartTextLayer::sync(const ChartFrame&) {
    std::lock_guard lock(candleMutex_);
    if (plotIdPending_) { plotId_.swap(pendingPlotId_); plotIdPending_ = false; }
    if (!candlePending_) return;
    candles_.swap(pendingCandles_);
    candleEnd_ = pendingEnd_;
    candlePending_ = false;
}

void ChartTextLayer::updateNodes(const ChartFrame& frame, LayerNodeContext& context) {
    submitChartText(context.text,
        [&] { context.text.beginFrame(context.root, context.window, context.atlas); },
        [&] { if (axes) axes->submitAxisText(context.text, context.atlas, frame.context.itemBounds.width(),
                                            frame.context.itemBounds.height(), frame.context.rightGutterPx); },
        [&] {
            submitOverlayLabels(frame, context);
            cvd_.submitLabels(frame, context);
            footer_.submitLabels(frame, context);
            ladder_.submitLabels(frame, context);
            submitPlotId(frame, context);
        },
        [&](int stage) {
            if (context.profiler && stage < 2)
                context.profiler->mark(stage == 0 ? FrameProfiler::AxisText : FrameProfiler::Labels);
        });
}

QString ChartTextLayer::formatPlotId(const QString& symbol, int64_t timeframeMs) {
    if (symbol.isEmpty() || timeframeMs <= 0) return {};
    const auto unit = timeframeMs % 86'400'000 == 0 ? 86'400'000 :
                      timeframeMs % 3'600'000 == 0 ? 3'600'000 :
                      timeframeMs % 60'000 == 0 ? 60'000 : timeframeMs % 1000 == 0 ? 1000 : 1;
    const QChar suffix = unit == 86'400'000 ? 'd' : unit == 3'600'000 ? 'h' :
                         unit == 60'000 ? 'm' : 's';
    return symbol + QStringLiteral(" · ") + QString::number(timeframeMs / unit) +
        (unit == 1 ? QStringLiteral("ms") : QString(suffix));
}

bool ChartTextLayer::clearOfLabels(const QRectF& box, std::span<const ChartGlyphInstance> glyphs) {
    return std::none_of(glyphs.begin(), glyphs.end(), [&](const auto& glyph) { return glyph.rect.intersects(box); });
}

void ChartTextLayer::updatePlotIdPill(const QRectF& box) {
    if (!plotIdPill_ || (plotIdPillInitialized_ && box == plotIdPillRect_)) return;
    plotIdPillRect_ = box;
    plotIdPillInitialized_ = true;
    auto* vertices = plotIdPill_->geometry()->vertexDataAsColoredPoint2D();
    const QPointF points[6] = {box.topLeft(), box.topRight(), box.bottomLeft(),
                               box.bottomLeft(), box.topRight(), box.bottomRight()};
    // Same premultiplied dark backing as the chart-edge wall badges.
    constexpr uchar r = 11, g = 15, b = 20, a = 235;
    for (int i = 0; i < 6; ++i)
        vertices[i].set(float(points[i].x()), float(points[i].y()), r, g, b, box.isEmpty() ? 0 : a);
    plotIdPill_->geometry()->markVertexDataDirty();
    plotIdPill_->markDirty(QSGNode::DirtyGeometry);
}

void ChartTextLayer::submitPlotId(const ChartFrame& frame, LayerNodeContext& context) {
    if (plotId_.isEmpty() || !context.atlasReady || frame.context.overlays.tpo || frame.context.overlays.volumeProfile) {
        updatePlotIdPill({});
        return;
    }
    ChartTextRun run;
    run.text = plotId_;
    run.anchor = frame.context.surfaceBounds.topLeft() + QPointF(7, 11);
    run.color = QColor(147, 161, 175);
    run.scale = float(kPlotIdPx / context.atlas.fontPx());
    run.hAlign = ChartTextRun::HorizontalAlign::Left;
    run.vAlign = ChartTextRun::VerticalAlign::Center;
    run.pixelSnap = true;
    QRectF box;
    if (!ChartTextLayout::measureRunRect(context.atlas, run, box)) {
        updatePlotIdPill({});
        return;
    }
    const QRectF pill = box.adjusted(-3, -2, 3, 2);
    if (!frame.context.surfaceBounds.contains(pill) ||
        !clearOfLabels(pill, heatmap_.labelGlyphs()) || !clearOfLabels(pill, footprint_.glyphs()) ||
        !clearOfLabels(pill, emptyGlyphs_) ||
        candleCrosses(candles_, candleEnd_, frame.context.raster, frame.context.surfaceBounds, pill)) {
        updatePlotIdPill({});
        return;
    }
    updatePlotIdPill(pill);
    plotIdGlyphs_.clear();
    ChartTextLayout::appendRun(context.atlas, run, plotIdGlyphs_);
    context.text.submitGlyphs(plotIdGlyphs_, ChartTextRenderer::Priority::High);
}

void ChartTextLayer::submitOverlayLabels(const ChartFrame& frame, LayerNodeContext& context) {
    const auto cards = footprint_.cards();
    static_assert(FlowSnapshot::kMaxBars + 1 == std::tuple_size_v<decltype(occlusion_)>);
    std::copy(cards.begin(), cards.end(), occlusion_.begin());
    size_t count = cards.size();
    const double inset = cvd_.bottomInset();
    if (inset > 0) {
        auto box = frame.context.surfaceBounds;
        box.setTop(box.bottom() - inset);
        occlusion_[count++] = box;
    }
    heatmap_.submitLabels(frame, context, {occlusion_.data(), count});
    footprint_.submitLabels(frame, context, inset);
    submitEmptyLabels(frame, context);
}

void ChartTextLayer::submitEmptyLabels(const ChartFrame& frame, LayerNodeContext& context) {
    emptyGlyphs_.clear();
    if (!context.atlasReady || context.atlas.fontPx() <= 0) return;
    const QString footprint = footprint_.emptyStateText(frame);
    const QString vp = vp_.emptyStateText(frame);
    const int lines = int(!footprint.isEmpty()) + int(!vp.isEmpty());
    if (!lines) return;
    const auto bounds = frame.context.surfaceBounds;
    const double bottom = bounds.bottom() - cvd_.bottomInset();
    if (bounds.width() <= 0 || bottom <= bounds.top()) return;
    const auto makeRun = [&](const QString& label, double y) {
        ChartTextRun run;
        run.text = label;
        run.anchor = {bounds.center().x(), y};
        run.color = QColor(143, 163, 184);
        run.scale = 14.0f / context.atlas.fontPx();
        run.pixelSnap = true;
        return run;
    };
    const double height = bottom - bounds.top();
    const std::array<double, 4> bands{bounds.top() + height * 0.5, bounds.top() + height * 0.25,
                                       bounds.top() + height * 0.75, bounds.top() + 18.0};
    for (const double band : bands) {
        const auto first = makeRun(footprint.isEmpty() ? vp : footprint, band - (lines == 2 ? 10.0 : 0.0));
        const auto second = makeRun(vp, band + 10.0);
        if (!fitsEmptyRun(frame.context, context.atlas, first, candles_, candleEnd_, bottom)) continue;
        if (lines == 2 && !fitsEmptyRun(frame.context, context.atlas, second, candles_, candleEnd_, bottom)) continue;
        ChartTextLayout::appendRun(context.atlas, first, emptyGlyphs_);
        if (lines == 2) ChartTextLayout::appendRun(context.atlas, second, emptyGlyphs_);
        break;
    }
    context.text.submitGlyphs(emptyGlyphs_, ChartTextRenderer::Priority::High);
}

void ChartTextLayer::buildMsdfAtlas() {
  if (atlasBuilt) {
    return;
  }
  ChartTextAtlas::BuildParams params;
  params.fontFamily = "Roboto Mono";
  params.fontPx = 64;
  params.pxRange = 4.0f;
  params.charset = QStringLiteral(
      " 0123456789.+-,:/<>·"
      "$%kMBABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  params.resourceFont =
      QStringLiteral(":/fonts/RobotoMono/RobotoMono-Regular.ttf");
  const QByteArray envFont = qgetenv("SENTINEL_MSDF_FONT");
  if (!envFont.isEmpty()) {
    params.fontPath = QString::fromUtf8(envFont);
  } else {
    const auto &client = GuiConfigStore::instance().clientConfig();
    if (!client.gui.msdfFontPath.empty()) {
      params.fontPath = QString::fromStdString(client.gui.msdfFontPath);
    }
  }
  envIntValue("SENTINEL_CHART_TEXT_FONT_PX", params.fontPx);
  envFloatValue("SENTINEL_CHART_TEXT_PX_RANGE", params.pxRange);
  sLog_Probe("text.atlas",
             "build fontPx=" << params.fontPx << " pxRange=" << params.pxRange
             << " charset=" << params.charset.size()
             << " fontPath=" << params.fontPath);
  if (!atlas.build(params)) {
    sLog_Warning("Chart text atlas build failed; chart text disabled: fontPath="
                 << params.fontPath << " resourceFont=" << params.resourceFont
                 << " fontPx=" << params.fontPx);
  } else {
    atlasBuilt = true;
  }
}

QString ChartTextLayer::getMsdfAtlasMemory() const {
  if (!atlas.isBuilt()) {
    return "MSDF atlas: N/A";
  }
  const QImage &image = atlas.image();
  if (image.isNull()) {
    return "MSDF atlas: N/A";
  }
  const double mb =
      static_cast<double>(image.sizeInBytes()) / (1024.0 * 1024.0);
  return QString("MSDF atlas: %1 MB").arg(mb, 0, 'f', 2);
}
} // namespace chart
