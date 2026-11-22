#pragma once
#include "CandleSeriesBuffer.hpp"
#include "FlowBackfillState.hpp"
#include "FlowFillTracker.hpp"
#include "FlowSeriesBuffer.hpp"
#include <memory>
#include <string>
#include <unordered_map>

class LiveOrderBook;

// Measurement only. RemoteGridDataSource calls these at its flow history and
// memory points; they write the flow.timing and client.mem probe lines and change
// no state the data source reads. Every entry returns at once when its probe is
// off (SENTINEL_PROBES is read once per process).
class FlowTimingProbe {
public:
    FlowTimingProbe();
    bool on() const { return m_on; }
    void reset();
    void sent(const FlowBackfillState::Request&, int64_t nowMs);
    // After next() sent nothing: logs the hold reason once per change while work is due.
    void held(const FlowBackfillState&, int64_t nowMs);
    void failed(quint64 id, int64_t nowMs);
    void replied(const SentinelStreamClient::FlowBatch&, bool accepted, int64_t nowMs, double applyMs,
                 double consumersMs);
    // Visible range [startMs, endMs) after the backfill window moved; wallNowMs places the live edge.
    void viewChanged(const QString& symbol, int64_t tf, int64_t startMs, int64_t endMs,
                     const FlowBackfillState&, const FlowSeriesBuffer&, int64_t nowMs, int64_t wallNowMs);
    void pageApplied(const FlowBackfillState&, const FlowSeriesBuffer&, int64_t nowMs, int64_t wallNowMs);

    // The buckets a fill must cover: the visible buckets, clipped to the backfill
    // window and to the forming live bucket (later buckets never fill). Empty when to <= from.
    struct Range { int64_t from = 0, to = 0; };
    static Range fillRange(int64_t startMs, int64_t endMs, int64_t tf, int64_t windowStartMs,
                           int64_t windowEndMs, int64_t wallNowMs);
private:
    bool covered(const FlowBackfillState&, int64_t wallNowMs) const;
    void logFilled(const FlowFillTracker::Filled&, const FlowSeriesBuffer&) const;
    bool m_on = false;
    QString m_symbol;
    int64_t m_tf = 0, m_startMs = 0, m_endMs = 0;
    quint64 m_sentId = 0;
    int64_t m_sentAtMs = 0;
    FlowBackfillState::Hold m_loggedHold = FlowBackfillState::Hold::Sent;
    FlowFillTracker m_fill;
};

// client.mem: approximate bytes from sizes and capacities.
// A replica holds two dense slot sides plus per-slot occupancy bits and per-block sums.
size_t approxBookBytes(const LiveOrderBook&);
void logClientMemory(const CandleSeriesBuffer*, const FlowSeriesBuffer&,
                     const std::unordered_map<std::string, std::unique_ptr<LiveOrderBook>>&);
