#pragma once
// A live replica whose global top is (bid, ask): what the chart's book feed reads.
// The feed keeps a pointer to the last book it was given (its timer reads it later),
// so every fixture book lives until the process exits, past every renderer.
#include "marketdata/model/TradeData.h"
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>

inline LiveOrderBook& bookWithTop(double bid, double ask) {
    static std::deque<std::unique_ptr<LiveOrderBook>> books;
    auto& book = books.emplace_back(std::make_unique<LiveOrderBook>("TEST"));
    if (!(bid > 0) || !(ask > bid)) return *book; // no top: an empty, unconfigured replica
    const double tick = std::pow(10.0, std::floor(std::log10(ask - bid) + 1e-9) - 1);
    book->initialize(bid - 16 * tick, ask + 16 * tick, tick);
    const std::array<BookLevelUpdate, 2> levels{{{true, bid, 1.0}, {false, ask, 1.0}}};
    book->applyUpdates(levels, std::chrono::system_clock::now(), nullptr);
    return *book;
}
