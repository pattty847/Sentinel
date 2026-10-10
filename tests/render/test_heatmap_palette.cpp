#include "render/heatmap/HeatmapPalette.hpp"
#include <QFile>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <regex>
#include <string>
#include <utility>

namespace {
using namespace heatmap::gpu;
HeatmapPalette testPalette() {
    HeatmapPalette palette;
    // Codes 128..384 with range 512 produce adjusted 0..1 exactly, including
    // adjusted zero from a nonzero magnitude (zero codes remain transparent).
    palette.tone = {1.0f, 2.0f, 0.0f};
    for (int i = 0; i < kPaletteWidth; ++i) {
        palette.texels[i * 4] = uint8_t(i % 256);
        palette.texels[i * 4 + 1] = uint8_t((i * 37) % 256);
        palette.texels[i * 4 + 2] = uint8_t(i < 256 ? 41 : 213);
        palette.texels[i * 4 + 3] = 255;
    }
    return palette;
}

// A4: mutation: restore a*0.49 / 0.51+a*0.49 in the CPU helper. Distinct
// endpoint texels expose the unreachable bid top / ask bottom and half bleed.
TEST(HeatmapPalette, BothHalvesReachTheirFirstAndLastTexelCentres) {
    const auto palette = testPalette();
    for (const bool ask : {false, true}) {
        for (const bool last : {false, true}) {
            const uint16_t code = last ? 384 : 128;
            const auto color = legacyRecordingColor(code | (ask ? 0x8000u : 0u), {0, 512}, palette);
            const int texel = (ask ? 256 : 0) + (last ? 255 : 0);
            for (int c = 0; c < 4; ++c)
                EXPECT_FLOAT_EQ(color[c], palette.texels[texel * 4 + c] / 255.0f)
                    << "ask=" << ask << " adjusted=" << last << " channel=" << c;
        }
    }
}

TEST(HeatmapPalette, CpuLinearSamplingMatchesTheTexelCentreFormula) {
    const auto palette = testPalette();
    for (const bool ask : {false, true}) {
        for (const uint16_t code : {128, 160, 256, 352, 384}) {
            const float a = float(code - 128) / 256.0f;
            const float coord = float(ask ? 256 : 0) + a * 255.0f;
            const int lo = int(std::floor(coord));
            const int hi = std::min(lo + 1, ask ? 511 : 255);
            const float weight = coord - lo;
            const float opacity = 0.625f;
            const auto actual = legacyRecordingColor(code | (ask ? 0x8000u : 0u), {0, 512}, palette, opacity);
            for (int c = 0; c < 3; ++c) {
                const float expected =
                    (palette.texels[lo * 4 + c] * (1.0f - weight) + palette.texels[hi * 4 + c] * weight) /
                    255.0f * opacity;
                EXPECT_NEAR(actual[c], expected, 1e-7f) << "ask=" << ask << " adjusted=" << a;
            }
            EXPECT_FLOAT_EQ(actual[3], opacity);
        }
    }
}

// A4 parity mutations: restore the old mapping in either shader independently.
// The shader parity check is a source-text match of the u statement.
TEST(HeatmapPalette, BothShaderMappingsUseTheCpuTexelCentres) {
    const std::string bid = "(0.5+adjusted*255.0)/512.0";
    const std::string ask = "(256.5+adjusted*255.0)/512.0";
    for (const auto &[path, expression] :
         {std::pair{"libs/gui/render/heatmap/shaders/heatmap_display.frag", "ask?" + ask + ":" + bid},
          std::pair{"libs/gui/render/shaders/heatmap_intensity.frag",
                    "mix(" + bid + "," + ask + ",isAsk)"}}) {
        QFile source(QString::fromUtf8(SENTINEL_SOURCE_DIR) + '/' + QString::fromUtf8(path));
        ASSERT_TRUE(source.open(QIODevice::ReadOnly)) << path;
        const auto text = std::regex_replace(source.readAll().toStdString(), std::regex("\\s+"), "");
        EXPECT_NE(text.find("floatu=" + expression + ";"), std::string::npos) << path;
    }
}
} // namespace
