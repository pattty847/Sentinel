#pragma once
#include <functional>
#include <string_view>

namespace chart {
class LayerVisibility {
public:
    std::function<void(std::string_view, bool, bool)> report;
    std::function<void()> requestUpdate, primaryFieldChanged, visibilityChanged;

    int primaryField() const { return primaryField_; }
    bool heatmapEnabled() const { return heatmap_; }
    bool footprintEnabled() const { return footprint_; }
    bool tpoEnabled() const { return tpo_; }
    bool volumeProfileEnabled() const { return volumeProfile_; }

    void setPrimaryField(int field) {
        if (primaryField_ == field) return;
        primaryField_ = field;
        if (field == 0) {
            heatmap_ = true;
            tpo_ = false;
            volumeProfile_ = false;
        } else if (field == 1) {
            footprint_ = true;
            tpo_ = false;
            volumeProfile_ = false;
        } else if (field == 2) {
            tpo_ = true;
            heatmap_ = false;
            footprint_ = false;
            volumeProfile_ = false;
        } else if (field == 3) {
            volumeProfile_ = true;
            tpo_ = false;
            heatmap_ = false;
            footprint_ = false;
        }
        if (report) report("primaryField", false, false);
        if (requestUpdate) requestUpdate();
        if (primaryFieldChanged) primaryFieldChanged();
        if (visibilityChanged) visibilityChanged();
    }

    void setVolumeProfileEnabled(bool enabled) {
        const bool newHeatmapEnabled = enabled ? false : heatmap_;
        const bool newFootprintEnabled = enabled ? false : footprint_;
        const bool newTpoEnabled = enabled ? false : tpo_;
        if (volumeProfile_ == enabled && heatmap_ == newHeatmapEnabled &&
            footprint_ == newFootprintEnabled && tpo_ == newTpoEnabled) return;
        volumeProfile_ = enabled;
        if (enabled) {
            heatmap_ = false;
            footprint_ = false;
            tpo_ = false;
        }
        changed("volumeProfile", enabled);
    }

    void setHeatmapEnabled(bool enabled) {
        const bool newTpoEnabled = enabled ? false : tpo_;
        const bool newVpEnabled = enabled ? false : volumeProfile_;
        if (heatmap_ == enabled && tpo_ == newTpoEnabled && volumeProfile_ == newVpEnabled) {
            if (report) report("heatmap", enabled, true);
            return;
        }
        heatmap_ = enabled;
        if (enabled) {
            tpo_ = false;
            volumeProfile_ = false;
        }
        changed("heatmap", enabled);
    }

    void setFootprintEnabled(bool enabled) {
        const bool newTpoEnabled = enabled ? false : tpo_;
        const bool newVpEnabled = enabled ? false : volumeProfile_;
        if (footprint_ == enabled && tpo_ == newTpoEnabled && volumeProfile_ == newVpEnabled) {
            if (report) report("footprint", enabled, true);
            return;
        }
        footprint_ = enabled;
        if (enabled) {
            tpo_ = false;
            volumeProfile_ = false;
        }
        changed("footprint", enabled);
    }

    void setTpoEnabled(bool enabled) {
        const bool newHeatmapEnabled = enabled ? false : heatmap_;
        const bool newFootprintEnabled = enabled ? false : footprint_;
        const bool newVolumeProfileEnabled = enabled ? false : volumeProfile_;
        if (tpo_ == enabled && heatmap_ == newHeatmapEnabled &&
            footprint_ == newFootprintEnabled && volumeProfile_ == newVolumeProfileEnabled) {
            if (report) report("tpo", enabled, true);
            return;
        }
        tpo_ = enabled;
        if (enabled) {
            heatmap_ = false;
            footprint_ = false;
            volumeProfile_ = false;
        }
        changed("tpo", enabled);
    }
private:
    void changed(std::string_view field, bool enabled) {
        if (report) report(field, enabled, false);
        if (requestUpdate) requestUpdate();
        if (visibilityChanged) visibilityChanged();
    }
    int primaryField_ = 0;
    bool heatmap_ = true;
    bool footprint_ = false;
    bool tpo_ = false;
    bool volumeProfile_ = false;
};
} // namespace chart
