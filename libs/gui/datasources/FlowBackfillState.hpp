#pragma once
#include "FlowSeriesBuffer.hpp"
#include <optional>

// One connection-wide flight, including flights belonging to an old selection.
// All times passed to scheduling methods are monotonic milliseconds.
class FlowBackfillState {
public:
    struct Request {
        quint64 id = 0, generation = 0;
        QString symbol;
        int64_t timeframeMs = 0, endMs = 0;
        int limit = 0;
        bool refresh = false, held = false, probe = false;
    };
    void select(const QString& symbol, int64_t tf);
    void setWindow(int64_t start, int64_t end);
    void refresh(int64_t bucket, int64_t dueMs);
    void observeLive(int64_t bucket, int64_t nowMs);
    std::optional<Request> next(int64_t nowMs);
    bool accept(const SentinelStreamClient::FlowBatch& batch, int64_t nowMs);
    bool fail(quint64 id, SentinelStreamClient::FlowFailure failure, int64_t nowMs);
    bool timeout(int64_t nowMs);
    std::optional<int64_t> takeRefusedBucket();
    void disconnect();
    bool inFlight() const { return m_pending && !m_pending->held; }
    int64_t retryAtMs() const { return m_retryAt; }
    int64_t wakeAtMs(int64_t nowMs) const;
    int64_t windowStartMs() const { return m_start; }
    int64_t windowEndMs() const { return m_window ? m_end + m_tf : 0; }
private:
    void cover(int64_t from, int64_t to);
    QString m_symbol;
    int64_t m_tf = 0, m_start = 0, m_end = 0, m_retryAt = 0;
    int64_t m_backoff = 1000;
    int m_pageLimit = 512;
    int64_t m_liveFrom = -1, m_probeBucket = -1, m_probeDue = 0, m_sentAt = 0;
    std::optional<int64_t> m_refusedBucket;
    quint64 m_generation = 0, m_nextId = 0;
    bool m_pagingStopped = false, m_window = false;
    std::optional<Request> m_pending;
    std::map<int64_t, int64_t> m_covered;
    // Explicit loss/building refreshes survive viewport moves; bounded at admission.
    std::map<int64_t, int64_t> m_refresh;
};
