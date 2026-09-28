#pragma once

#include <cmath>
#include <optional>

// GUI-thread decision state. A pending request is consumed only with a valid live price.
class PriceCenteringPolicy {
public:
    struct Range { double min; double max; };

    void reset() {
        m_bookMid.reset();
        m_lastTrade.reset();
        m_pending = true;
        m_forceInitial = true;
        m_upgradeToBook = false;
    }
    void setBook(double bestBid, double bestAsk) {
        m_bookMid = valid(bestBid) && valid(bestAsk) && bestAsk >= bestBid
            ? std::optional<double>((bestBid + bestAsk) * 0.5) : std::nullopt;
        if (m_bookMid && m_upgradeToBook) {
            m_pending = true;
            m_forceInitial = true;
            m_upgradeToBook = false;
        }
    }
    void setTrade(double price) {
        if (valid(price)) m_lastTrade = price;
    }
    void requestFollow() { m_pending = true; m_forceInitial = false; m_upgradeToBook = false; }
    void cancel() { m_pending = false; m_forceInitial = false; m_upgradeToBook = false; }
    bool pending() const { return m_pending; }
    bool initialRequest() const { return m_forceInitial; }

    std::optional<Range> consume(double span, bool followEnabled) {
        if (!m_pending || (!followEnabled && !m_forceInitial) ||
            !valid(span)) return std::nullopt;
        const auto mid = m_bookMid ? m_bookMid : m_lastTrade;
        if (!mid) return std::nullopt;
        m_pending = false;
        m_forceInitial = false;
        m_upgradeToBook = !m_bookMid;
        return Range{*mid - span * 0.5, *mid + span * 0.5};
    }

private:
    static bool valid(double value) { return std::isfinite(value) && value > 0.0; }
    std::optional<double> m_bookMid;
    std::optional<double> m_lastTrade;
    bool m_pending = true;
    bool m_forceInitial = true;
    bool m_upgradeToBook = false;
};
