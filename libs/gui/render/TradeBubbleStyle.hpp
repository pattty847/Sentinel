#pragma once
#include "chart/core/TpoStyle.hpp"
#include <algorithm>
#include <cmath>

namespace trade_bubbles {
// Same element vocabulary as TPO. Buy/sell width is the maximum logical radius;
// ring width is its logical stroke width. The minimum is in product base units.
using Element = tpo::Element;
struct StyleModel {
    Element buy{{46, 189, 133, 255}, .40, true, 18};
    Element sell{{246, 70, 93, 255}, .40, true, 18};
    Element ring{{15, 17, 20, 255}, 1, true, 1};
    double minimumBaseSize = .38;
    bool operator==(const StyleModel&) const = default;
};
inline StyleModel clamped(StyleModel s) {
    const StyleModel defaults;
    const auto finite = [](double n, double fallback) { return std::isfinite(n) ? n : fallback; };
    for (auto* e : {&s.buy, &s.sell, &s.ring})
        e->opacity = std::clamp(finite(e->opacity, e == &s.ring ? 1 : .4), 0.0, 1.0);
    s.buy.width = std::clamp(finite(s.buy.width, defaults.buy.width), 4.0, 48.0);
    s.sell.width = std::clamp(finite(s.sell.width, defaults.sell.width), 4.0, 48.0);
    s.ring.width = std::clamp(finite(s.ring.width, defaults.ring.width), 0.0, 3.0);
    s.minimumBaseSize = std::max(0.0, finite(s.minimumBaseSize, defaults.minimumBaseSize));
    return s;
}
inline double defaultMinimum(std::string_view product, double price, double btcPrice, double btcSize = .38) {
    if (product == "BTC-USD") return btcSize;
    // Same $100k BTC reference convention as heatmap auto sensitivity until a
    // BTC quote has arrived. Unknown product prices keep the existing all-trades rule.
    if (!(price > 0) || !std::isfinite(price)) return 0;
    const double reference = btcPrice > 0 && std::isfinite(btcPrice) ? btcPrice : 100000;
    return btcSize * reference / price;
}
} // namespace trade_bubbles
