#pragma once

#include <vector>

#include "ChartTextAtlas.hpp"
#include "ChartTextPrimitives.hpp"

class ChartTextLayout {
public:
    static bool measureRunRect(const ChartTextAtlas& atlas,
                               const ChartTextRun& run,
                               QRectF& outRect);
    static void appendRun(const ChartTextAtlas& atlas,
                          const ChartTextRun& run,
                          std::vector<ChartGlyphInstance>& out);
    static bool buildDebugSample(const ChartTextAtlas& atlas,
                                 const ChartTextRun& run,
                                 ChartGlyphInstance& out,
                                 float& outRenderPx);
};
