#pragma once

#include <QRectF>
#include <QString>
#include <vector>

#include "ChartTextAtlas.hpp"
#include "ChartTextPrimitives.hpp"

namespace AxisLayout {

constexpr int kAutoAxisLabelBasePx = 13;
constexpr int kAutoAxisLabelMinPx = 12;
constexpr int kAutoAxisLabelMaxPx = 15;
constexpr int kPriceAxisMinWidthPx = 56;
constexpr int kPriceAxisMaxWidthPx = 140;
constexpr int kTimeAxisMinHeightPx = 24;
constexpr int kTimeAxisMaxHeightPx = 40;
constexpr int kAxisDimensionHysteresisPx = 4;

int clampAxisLabelPx(int px);
int resolveAutoAxisLabelPx(double logicalDpiY);
int resolveEffectiveAxisLabelPx(double logicalDpiY,
                                int configOverridePx,
                                int envOverridePx);
int applyDimensionHysteresis(int currentPx,
                             int targetPx,
                             int thresholdPx = kAxisDimensionHysteresisPx);
int measurePriceAxisWidthPx(const ChartTextAtlas& atlas,
                            float axisScale,
                            const std::vector<QString>& labels,
                            int currentPx = 0);
int measureTimeAxisHeightPx(const ChartTextAtlas& atlas,
                            float axisScale,
                            int currentPx = 0);
bool runFitsRect(const ChartTextAtlas& atlas,
                 const ChartTextRun& run,
                 const QRectF& rect,
                 QRectF* outRunRect = nullptr);

} // namespace AxisLayout
