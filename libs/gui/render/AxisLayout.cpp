#include "AxisLayout.hpp"

#include <algorithm>
#include <cmath>

#include "ChartTextLayout.hpp"

namespace AxisLayout {
namespace {

int clampDimension(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

} // namespace

int clampAxisLabelPx(int px) {
    return clampDimension(px, kAutoAxisLabelMinPx, kAutoAxisLabelMaxPx);
}

int resolveAutoAxisLabelPx(double logicalDpiY) {
    const double safeDpi = (std::isfinite(logicalDpiY) && logicalDpiY > 0.0)
        ? logicalDpiY
        : 96.0;
    const int resolved = static_cast<int>(std::lround(
        static_cast<double>(kAutoAxisLabelBasePx) * safeDpi / 96.0));
    return clampAxisLabelPx(resolved);
}

int resolveEffectiveAxisLabelPx(double logicalDpiY,
                                int configOverridePx,
                                int envOverridePx) {
    if (envOverridePx > 0) {
        return clampAxisLabelPx(envOverridePx);
    }
    if (configOverridePx > 0) {
        return clampAxisLabelPx(configOverridePx);
    }
    return resolveAutoAxisLabelPx(logicalDpiY);
}

int applyDimensionHysteresis(int currentPx, int targetPx, int thresholdPx) {
    if (currentPx <= 0 || thresholdPx <= 0) {
        return targetPx;
    }
    return (std::abs(targetPx - currentPx) >= thresholdPx) ? targetPx : currentPx;
}

int measurePriceAxisWidthPx(const ChartTextAtlas& atlas,
                            float axisScale,
                            const std::vector<QString>& labels,
                            int currentPx) {
    if (!atlas.isBuilt() || axisScale <= 0.0f) {
        return currentPx > 0 ? currentPx : kPriceAxisMinWidthPx;
    }

    float maxWidth = 0.0f;
    for (const QString& label : labels) {
        if (label.isEmpty()) {
            continue;
        }
        ChartTextRun run;
        run.text = label;
        run.anchor = QPointF(0.0, 0.0);
        run.scale = axisScale;
        run.hAlign = ChartTextRun::HorizontalAlign::Left;
        run.vAlign = ChartTextRun::VerticalAlign::Top;
        run.useStableMetrics = true;
        QRectF rect;
        if (!ChartTextLayout::measureRunRect(atlas, run, rect)) {
            continue;
        }
        maxWidth = std::max(maxWidth, static_cast<float>(rect.width()));
    }

    const int target = clampDimension(
        static_cast<int>(std::ceil(maxWidth)) + 12,
        kPriceAxisMinWidthPx,
        kPriceAxisMaxWidthPx);
    return applyDimensionHysteresis(currentPx, target);
}

int measureTimeAxisHeightPx(const ChartTextAtlas& atlas,
                            float axisScale,
                            int currentPx) {
    if (!atlas.isBuilt() || axisScale <= 0.0f) {
        return currentPx > 0 ? currentPx : kTimeAxisMinHeightPx;
    }

    const int target = clampDimension(
        static_cast<int>(std::ceil(atlas.lineHeightPx() * axisScale)) + 10,
        kTimeAxisMinHeightPx,
        kTimeAxisMaxHeightPx);
    return applyDimensionHysteresis(currentPx, target);
}

bool runFitsRect(const ChartTextAtlas& atlas,
                 const ChartTextRun& run,
                 const QRectF& rect,
                 QRectF* outRunRect) {
    QRectF runRect;
    if (!ChartTextLayout::measureRunRect(atlas, run, runRect)) {
        return false;
    }
    if (outRunRect) {
        *outRunRect = runRect;
    }
    return runRect.left() >= rect.left() &&
           runRect.top() >= rect.top() &&
           runRect.right() <= rect.right() &&
           runRect.bottom() <= rect.bottom();
}

} // namespace AxisLayout
