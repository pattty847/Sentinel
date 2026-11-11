#pragma once

#include "../../core/trading/AlgoEngine.hpp"
#include "../../core/trading/TradingSnapshot.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

// The algo overlay's order lines: one span per algo order, open while the order rests,
// closed at its fill, cancel or rejection. Qt-free; AlgoOverlayRenderer owns one on the
// render thread.
class AlgoOrderSpans {
public:
    static constexpr size_t kMaxMarkers = 2000;

    struct Span {
        trading::OrderSide side = trading::OrderSide::Unknown;
        double price = 0.0;
        int64_t openedMs = 0;
        int64_t closedMs = 0;
        trading::OrderStatus terminalStatus = trading::OrderStatus::New;
        bool active = false;
    };

    void apply(const trading::AlgoOrderEvent& event);
    // The server's whole state at atMs: an active span whose order the server no longer
    // holds closes there; a held algo order missing here opens there. A new epoch (server
    // restart) closes every active span and keeps the old ones under retired keys, since
    // the new session reuses order ids.
    void reconcile(const trading::TradingSnapshot& snapshot, int64_t atMs);

    const std::unordered_map<std::string, Span>& spans() const { return m_spans; }
    const std::deque<trading::AlgoOrderEvent>& markers() const { return m_markers; }
    size_t activeCount() const;

private:
    std::unordered_map<std::string, Span> m_spans;
    std::deque<trading::AlgoOrderEvent> m_markers;
    std::string m_epoch;
    uint64_t m_retiredEpochs = 0;
};
