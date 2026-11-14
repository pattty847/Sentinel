#pragma once

#include <algorithm>
#include <optional>
#include <utility>

inline std::optional<std::pair<float, float>> paperPositionBandSpan(
    bool bandStyle, float entryY, float markY, float height) {
    if (!bandStyle) return std::nullopt;
    const float top = std::clamp(std::min(entryY, markY), 0.0f, height);
    const float bottom = std::clamp(std::max(entryY, markY), 0.0f, height);
    if (bottom <= top + 0.5f) return std::nullopt;
    return std::pair{top, bottom};
}
