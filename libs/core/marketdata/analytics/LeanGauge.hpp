#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sentinel::book {
// Dense GUI-replica view. Top indices are native book slots, -1 for an absent side.
struct LeanBook {
    std::span<const double> bids, asks;
    double origin = 0, tick = 0;
    std::ptrdiff_t bidTop = -1, askTop = -1;
};
class LeanGauge {
public:
    static constexpr std::array<double, 4> Distances{5, 15, 50, 100};
    static constexpr size_t DepthPoints = 101, HistoryCapacity = 241;
    enum class State { Empty, TwoSided, BidOnly, AskOnly, Invalid, Crossed };
    struct Band { double bid = 0, ask = 0, imbalance = 0; };
    struct Sample { int64_t time = 0; double value = 0; };
    // Generation changes on symbol switch. Invalid/withdrawn books clear history;
    // crossed books hold the last values and history without adding a sample.
    void update(const LeanBook& book, uint64_t generation, int64_t now);
    void reset();
    State state() const { return m_state; }
    const auto& bands() const { return m_bands; }
    const auto& bidDepth() const { return m_bidDepth; }
    const auto& askDepth() const { return m_askDepth; }
    bool disagreement() const { return m_bands[0].imbalance * m_bands[3].imbalance < 0; }
    size_t historySize() const { return m_count; }
    const Sample& history(size_t i) const { return m_history[(m_head + i) % HistoryCapacity]; }
private:
    bool reference(const LeanBook& book, double& mid);
    void accumulate(std::span<const double> sizes, const LeanBook& book, double mid, bool bid);
    void sample(int64_t now);
    State m_state = State::Empty;
    std::array<Band, 4> m_bands{};
    std::array<double, DepthPoints> m_bidDepth{}, m_askDepth{};
    std::array<Sample, HistoryCapacity> m_history{};
    size_t m_head = 0, m_count = 0;
    uint64_t m_generation = 0;
    double m_tick = 0;
};
} // namespace sentinel::book
