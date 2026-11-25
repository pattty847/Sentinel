#pragma once
#include "CandleSeriesBuffer.hpp"
#include "FlowSeriesBuffer.hpp"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
class LiveOrderBook;

// Measurement only: view change to visible fill, including immediate cache hits.
// Fetch is attributed only while a view waits. Live ownership is counted from
// the composed snapshot, not from unrelated live deliveries during a pan.
class FlowTimingProbe {
public:
    FlowTimingProbe();
    explicit FlowTimingProbe(bool on) : m_on(on) {}
    bool on() const { return m_on; }
    void reset();
    void viewChanged(const QString&, int64_t tf, int64_t from, int64_t to, int64_t now);
    void fetched() { if (m_waiting) m_fetched = true; }
    struct Filled { int64_t sinceLastChangeMs, sinceFirstChangeMs; size_t sealed, live; bool fetched; };
    std::optional<Filled> applied(const FlowSeriesBuffer&, int64_t now, int64_t wallNow);
private:
    bool m_on = false, m_waiting = false, m_fetched = false;
    QString m_symbol;
    int64_t m_tf = 0, m_from = 0, m_to = 0, m_first = 0, m_last = 0;
};

// client.mem: approximate bytes from sizes and capacities.
// A replica holds two dense slot sides plus per-slot occupancy bits and per-block sums.
size_t approxBookBytes(const LiveOrderBook&);
void logClientMemory(const CandleSeriesBuffer*, const FlowSeriesBuffer&,
                     const std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>>&);
