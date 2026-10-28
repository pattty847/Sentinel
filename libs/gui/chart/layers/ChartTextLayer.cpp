#include "chart/layers/ChartTextLayer.hpp"
#include "chart/layers/HeatmapLayer.hpp"
#include "chart/layers/LadderLayer.hpp"
#include "chart/layers/RenderSequence.hpp"
#include "chart/core/ChartFrame.hpp"
#include "render/FrameProfiler.hpp"
#include "config/GuiConfigStore.hpp"
#include "SentinelLogging.hpp"

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
} // namespace

namespace chart {
void ChartTextLayer::updateNodes(const ChartFrame& frame, LayerNodeContext& context) {
    submitChartText(context.text,
        [&] { context.text.beginFrame(context.root, context.window, context.atlas); },
        [&] { if (axes) axes->submitAxisText(context.text, context.atlas, frame.context.itemBounds.width(),
                                            frame.context.itemBounds.height(), frame.context.rightGutterPx); },
        [&] {
            heatmap_.submitLabels(frame, context);
            ladder_.submitLabels(frame, context, !heatmap_.labelGlyphs().empty());
        },
        [&](int stage) {
            if (context.profiler && stage < 2)
                context.profiler->mark(stage == 0 ? FrameProfiler::AxisText : FrameProfiler::Labels);
        });
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
      " 0123456789.+-,:/"
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
