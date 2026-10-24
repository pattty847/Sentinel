#include "LeanGauge.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sentinel::book {
namespace {
bool validGrid(const LeanBook& book)
{
    return std::isfinite(book.tick) && book.tick > 0 && std::isfinite(book.origin);
}
}

void LeanGauge::reset()
{
    m_state = State::Empty;
    m_bands = {};
    m_bidDepth = {};
    m_askDepth = {};
    m_head = m_count = 0;
}

bool LeanGauge::reference(const LeanBook& book, double& mid)
{
    const bool bid = book.bidTop >= 0 && size_t(book.bidTop) < book.bids.size();
    const bool ask = book.askTop >= 0 && size_t(book.askTop) < book.asks.size();
    if (!bid && !ask) { m_state = State::Empty; return false; }
    if (!validGrid(book)) {
        m_state = State::Invalid;
        return false;
    }
    if (bid && ask && book.bidTop >= book.askTop) { m_state = State::Invalid; return false; }
    // Work in native slots: decimal boundary prices must not fall outside a band.
    mid = bid && ask ? double(book.bidTop) / 2 + double(book.askTop) / 2
                    : double(bid ? book.bidTop : book.askTop);
    m_state = bid && ask ? State::TwoSided : bid ? State::BidOnly : State::AskOnly;
    return true;
}

void LeanGauge::accumulate(std::span<const double> sizes, const LeanBook& book, double mid, bool bid)
{
    if (sizes.empty()) return;
    auto& depth = bid ? m_bidDepth : m_askDepth;
    const double radius = 100 / book.tick;
    const size_t lo = size_t(std::clamp(std::ceil(mid - radius - 1e-8), 0.0, double(sizes.size())));
    const size_t end = size_t(std::clamp(std::floor(mid + radius + 1e-8) + 1, 0.0, double(sizes.size())));
    for (size_t i = lo; i < end; ++i) {
        const double size = sizes[i];
        if (!std::isfinite(size) || size <= 0) continue;
        const double distance = (bid ? mid - double(i) : double(i) - mid) * book.tick;
        if (distance < -1e-8 || distance > 100 + 1e-8) continue;
        for (size_t b = 0; b < Distances.size(); ++b) {
            if (distance <= Distances[b] + 1e-8) {
                auto& sum = bid ? m_bands[b].bid : m_bands[b].ask;
                sum = std::min(sum, std::numeric_limits<double>::max() - size) + size;
            }
        }
        const size_t point = size_t(std::clamp(std::ceil(distance - 1e-8), 0.0, 100.0));
        depth[point] = std::min(depth[point], std::numeric_limits<double>::max() - size) + size;
    }
    for (size_t i = 1; i < depth.size(); ++i)
        depth[i] = std::min(depth[i], std::numeric_limits<double>::max() - depth[i - 1]) + depth[i - 1];
}

void LeanGauge::sample(int64_t now)
{
    if (m_count && now < history(m_count - 1).time) m_head = m_count = 0;
    while (m_count && now - history(0).time > 120000) {
        m_head = (m_head + 1) % HistoryCapacity;
        --m_count;
    }
    if (m_count && now - history(m_count - 1).time < 500) return;
    if (m_count == HistoryCapacity) { m_head = (m_head + 1) % HistoryCapacity; --m_count; }
    m_history[(m_head + m_count++) % HistoryCapacity] = {now, m_bands[1].imbalance};
}

void LeanGauge::update(const LeanBook& book, uint64_t generation, int64_t now)
{
    if (generation != m_generation || book.tick != m_tick) reset();
    m_generation = generation;
    m_tick = book.tick;
    m_bands = {};
    m_bidDepth = {};
    m_askDepth = {};
    double mid = 0;
    if (!reference(book, mid)) { m_head = m_count = 0; return; }
    if (book.bidTop >= 0) accumulate(book.bids, book, mid, true);
    if (book.askTop >= 0) accumulate(book.asks, book, mid, false);
    for (auto& band : m_bands) {
        const double scale = std::max(band.bid, band.ask);
        if (scale > 0) band.imbalance = (band.bid / scale - band.ask / scale) / (band.bid / scale + band.ask / scale);
    }
    sample(now);
}
} // namespace sentinel::book
