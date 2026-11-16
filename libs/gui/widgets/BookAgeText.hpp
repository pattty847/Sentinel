#pragma once
#include "../models/MarketHealth.hpp"

// Receive age is descriptive, not a timeout or proof of market liveness.
inline QString bookAgeText(std::optional<qint64> ageMs) {
    return QStringLiteral("Book age: ") + MarketHealth::ageText(ageMs);
}
