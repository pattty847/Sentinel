#include "chart/shell/ChartLookSettings.hpp"
#include "chart/layers/ChartLayers.hpp"
#include "chart/core/ChartViewController.hpp"
#include "chart/core/PublishedFrame.hpp"
#include "heatmap/HeatmapChartSettings.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
struct Ports final : chart::IChartEnvironment, chart::ITickOracle, chart::IHeatmapViewSink {
    bool attached() const override { return false; }
    chart::SurfaceFacts surface() const override { return {}; }
    double drawnTick() const override { return 0; }
    double priceScale() const override { return 0; }
    bool manualTick() const override { return false; }
    bool hasCurrentResolution() const override { return false; }
    bool priceScaleCurrent() const override { return false; }
    chart::SpanLimits limits() const override { return {}; }
    int64_t liveAnchorMs() const override { return 0; }
    int64_t liveOpenEndMs() const override { return 0; }
    int64_t oldestAvailableMs() const override { return 0; }
    double recentMidPrice() const override { return 0; }
    std::optional<heatmap::gpu::ViewWindow> restView() const override { return std::nullopt; }
    int64_t predictTickUnits(const heatmap::gpu::ViewWindow&) const override { return 0; }
    bool buildsTick(int64_t, const heatmap::gpu::ViewWindow&) const override { return false; }
    int64_t autoTickState() const override { return 0; }
    heatmap::AutoTickParams autoTickParams() const override { return {}; }
    bool setAutoPriceTick(std::optional<int64_t>) override { return false; }
    void setView(const heatmap::gpu::ViewWindow&, bool) override {}
};

TEST(ChartLookSettings, TpoConfigAndStyleKeepNoticeOrder) {
    chart::ChartLayers layers;
    chart::ChartLookSettings look(layers);
    std::vector<std::string> calls;
    look.notify = [&](auto notice) {
        calls.push_back(notice == chart::ChartLookSettings::Notice::TpoConfig ? "config" : "style");
    };
    look.requestUpdate = [&] { calls.push_back("update"); };
    ClientTpoConfig config;
    config.layout = "split"; config.theme = "sage"; config.session = "ny";
    config.periodMinutes = 15; config.sessions = 3; config.rowPx = 20;
    look.applyTpoConfig(config);
    EXPECT_EQ(look.tpoLayout(), "split");
    EXPECT_EQ(look.tpoTheme(), "sage");
    EXPECT_EQ(look.tpoSessions(), 3);
    EXPECT_EQ(calls, (std::vector<std::string>{"config", "update"}));
    calls.clear();
    look.setTpoLayout("collapsed");
    EXPECT_EQ(calls, (std::vector<std::string>{"update", "style"}));
    calls.clear();
    look.setTpoTheme("calm");
    EXPECT_EQ(calls, (std::vector<std::string>{"update", "style"}));
    calls.clear();
    look.setTpoLayout("unknown");
    EXPECT_TRUE(calls.empty());
    look.setTpoSessionType(99);
    EXPECT_EQ(look.tpoSessionType(), 4);
    look.setTpoTimeframeMs(3'600'000);
    EXPECT_EQ(look.tpoTimeframeMs(), 3'600'000);
}

TEST(ChartLookSettings, ToneAndCandleLookKeepClampsAndNotices) {
    chart::ChartLayers layers;
    chart::ChartLookSettings look(layers);
    std::vector<std::string> calls;
    look.applyTone = [&](float gamma, float contrast, float floor) {
        calls.push_back("tone"); EXPECT_FLOAT_EQ(gamma, static_cast<float>(look.gamma()));
        EXPECT_FLOAT_EQ(contrast, static_cast<float>(look.contrast()));
        EXPECT_FLOAT_EQ(floor, static_cast<float>(look.shaderFloor()));
    };
    look.requestUpdate = [&] { calls.push_back("update"); };
    look.notify = [&](auto notice) {
        calls.push_back(notice == chart::ChartLookSettings::Notice::CandleAppearance ? "appearance" : "notice");
    };
    look.setHeatmapGamma(9);
    EXPECT_DOUBLE_EQ(look.gamma(), 5);
    EXPECT_EQ(calls, (std::vector<std::string>{"tone", "update", "notice"}));
    calls.clear();
    look.setHeatmapGamma(5);
    EXPECT_TRUE(calls.empty());
    look.setHeatmapContrast(0);
    EXPECT_DOUBLE_EQ(look.contrast(), 0.1);
    look.setHeatmapShaderFloor(1);
    EXPECT_DOUBLE_EQ(look.shaderFloor(), 0.5);
    calls.clear();
    look.setCandleStyle(9);
    EXPECT_EQ(look.candleStyle(), 2);
    heatmap::HeatmapChartSettings settings;
    settings.candleUpColor = "#123456";
    settings.candleDownColor = "#654321";
    settings.candleWickColor = "#abcdef";
    settings.candleBodyOpacity = 0.5;
    settings.candleWickWidth = 3;
    look.setCandleAppearance(settings);
    EXPECT_EQ(look.candleUpColor(), QColor("#123456"));
    EXPECT_EQ(look.candleWickWidth(), 3);
    EXPECT_EQ(calls, (std::vector<std::string>{"notice", "appearance"}));
    calls.clear(); look.setCandleAppearance(settings);
    EXPECT_TRUE(calls.empty());
}

TEST(ChartLookSettings, ClientAndServerConfigKeepSequencingAndExtraToneCall) {
    chart::ChartLayers layers;
    chart::ChartLookSettings look(layers);
    Ports ports;
    chart::PublishedFrame published;
    chart::ChartViewController view(ports, ports, ports, published);
    std::vector<std::string> calls;
    look.applyTone = [&](float, float, float) { calls.push_back("tone"); };
    look.notify = [&](auto) { calls.push_back("notice"); };
    look.requestUpdate = [&] { calls.push_back("update"); };
    look.onViewportChanged = [&] { calls.push_back("viewport"); };
    ClientConfig config;
    config.heatmap.gamma = 2;
    config.heatmap.contrast = 3;
    config.heatmap.shaderFloor = 0.2;
    config.heatmap.initialColumnPx = 99;
    config.heatmap.initialPricePct = 999;
    look.setInitialTone(ClientConfig{});
    look.applyClientConfig(config, view, nullptr);
    EXPECT_EQ(calls, (std::vector<std::string>{"tone", "update", "notice", "tone", "update", "notice",
                                                "tone", "update", "notice", "tone", "update", "viewport", "update"}));
    EXPECT_EQ(view.initialViewSettings().columnPx, 64);
    EXPECT_EQ(view.initialViewSettings().pricePct, 100);
    calls.clear(); look.applyClientConfig(config, view, nullptr);
    EXPECT_EQ(calls, (std::vector<std::string>{"tone", "update", "viewport", "update"}));
    ServerConfig server;
    server.chart.defaultTimeframeMs = 60000;
    look.applyServerConfig(server, view);
    EXPECT_EQ(view.session().timeframeMs, 60000);
}
} // namespace
