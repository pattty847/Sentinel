#pragma once
#include "FlowSeriesBuffer.hpp"
#include <optional>

// Flow history pager for one selection. One connection-wide flight, including flights
// belonging to an old selection. All times passed to scheduling methods are monotonic
// milliseconds; bucket times are UTC bucket starts.
//
// One owner per time range (the live seam). The live bucket is the clock's current bucket
// (markLiveEdge), raised by any delivered live frame (observeLive). Nothing after it is ever
// requested, whatever the window holds.
// - Live armed (the subscription delivered a frame): the stream owns the live bucket and
//   keeps it covered as the edge advances. History requests only older buckets: gap pages,
//   seal probes and refreshes alike. A refresh at or after the live bucket waits until the
//   seam passes it.
// - Live not armed (quiet or refused subscription): history pages buckets before the seam;
//   the live bucket, and the buckets that closed since it was last sampled, belong to the
//   live sample: one page per FlowSeriesBuffer::kRefetchIntervalMs, sent only after every
//   older gap of the window is paged. A closing bucket is never paged on its own as the
//   edge moves.
// There is no edge pace. Pages are spaced by the 100 ms page gap only (server fairness:
// the server runs one flow job per connection and few overall).
// Coverage outside the window is kept until the series buffer trims those bars (uncover).
class FlowBackfillState {
public:
    struct Request {
        quint64 id = 0, generation = 0;
        QString symbol;
        int64_t timeframeMs = 0, endMs = 0;
        int limit = 0;
        bool refresh = false, held = false, probe = false, sample = false;
    };
    void select(const QString& symbol, int64_t tf);
    void setWindow(int64_t start, int64_t end);
    void refresh(int64_t bucket, int64_t dueMs);
    void beginLive(int64_t bucket);
    void markLiveEdge(int64_t bucket);
    void observeLive(int64_t bucket, int64_t nowMs);
    std::optional<Request> next(int64_t nowMs);
    bool accept(const SentinelStreamClient::FlowBatch& batch, int64_t nowMs);
    bool fail(quint64 id, SentinelStreamClient::FlowFailure failure, int64_t nowMs);
    bool timeout(int64_t nowMs);
    void refuseSelection();
    std::optional<int64_t> takeRefusedBucket();
    void disconnect();
    // The series buffer trimmed every bucket before belowMs and from fromMs on: those
    // buckets are no longer loaded, so a window that reaches them pages them again.
    void uncover(int64_t belowMs, int64_t fromMs);
    bool inFlight() const { return m_pending && !m_pending->held; }
    int64_t retryAtMs() const { return m_retryAt; }
    int64_t wakeAtMs(int64_t nowMs) const;
    int64_t windowStartMs() const { return m_start; }
    int64_t windowEndMs() const { return m_window ? m_end + m_tf : 0; }
    // Why the last next() sent nothing (Sent when it sent a request). Measurement only.
    enum class Hold { Sent, Idle, NoSelection, InFlight, Pacing, Backoff, Stopped, LiveSample };
    Hold lastHold() const { return m_hold; }
    static const char* holdName(Hold);
    // True when a page or refresh is due but held: an uncovered bucket in the
    // window, or a refresh whose time has come.
    bool workDue(int64_t nowMs) const;
    int64_t liveSampleDueMs() const { return m_liveSampleDue; }
    // True when one proven run covers every bucket of [from, to). Bucket-aligned input.
    bool covers(int64_t from, int64_t to) const;
private:
    void cover(int64_t from, int64_t to);
    int64_t gapEnd() const;
    int64_t historyLimit() const;
    bool liveSampling() const;
    int64_t sampleFrom() const;
    int64_t flightTimeoutMs() const;
    std::optional<Request> send(Request request, int64_t nowMs);
    QString m_symbol;
    int64_t m_tf = 0, m_start = 0, m_end = 0, m_retryAt = 0;
    int64_t m_backoff = 1000;
    int m_pageLimit = 512;
    int64_t m_liveCurrentBucket = -1, m_lastLiveBucket = -1;
    int64_t m_probeBucket = -1, m_probeDue = 0, m_sentAt = 0;
    // Live sample (live not armed): next due time, and the bucket the last sample ended at.
    int64_t m_liveSampleDue = 0, m_sampledLive = -1;
    bool m_liveArmed = false, m_liveTracking = false;
    std::optional<int64_t> m_refusedBucket;
    quint64 m_generation = 0, m_nextId = 0;
    bool m_pagingStopped = false, m_window = false;
    Hold m_hold = Hold::Idle, m_retryHold = Hold::Pacing;
    std::optional<Request> m_pending;
    std::optional<Request> m_timedOut;
    std::map<int64_t, int64_t> m_covered;
    // Explicit loss/building refreshes survive viewport moves; bounded at admission.
    std::map<int64_t, int64_t> m_refresh;
};
