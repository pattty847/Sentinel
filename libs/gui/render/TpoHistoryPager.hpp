/*
 * Sentinel – TpoHistoryPager
 *
 * Paces TPO history pages: one request in flight, the next one sent when the
 * reply (or an error, or a timeout) for the previous one arrives. The server
 * queues at most eight overlay history requests per client and rejects the rest
 * without retry, so the GUI never fires a whole burst.
 *
 *   - start() with the selection that is already being paged is a no-op (dedupe:
 *     toggling the layer or reapplying the same config does not restart).
 *   - A new selection (symbol, period, session type, session count) starts a new
 *     generation; replies for older generations no longer match and are ignored.
 *   - Request ids are "tpo-<generation>-<page>". A reply without an id (older
 *     server) completes the in-flight page when its symbol, period and session
 *     type match the current selection.
 *
 * Pure and single-threaded (GUI thread). Times are UTC epoch ms.
 */
#pragma once

#include "TpoProfileModel.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace tpo {

class HistoryPager {
public:
    struct Selection {
        std::string symbol;
        int64_t periodMs = 0;
        int sessionType = 4;
        int sessions = 1;
        bool operator==(const Selection&) const = default;
    };

    struct Request {
        std::string symbol;
        int64_t periodMs = 0;
        int sessionType = 4;
        int64_t endMs = 0;
        int count = 0;
        std::string requestId;
    };

    static constexpr int64_t kTimeoutMs = 45'000;

    // Returns the first request to send, or nothing when the same selection is
    // already being paged or there is nothing to request.
    std::optional<Request> start(const Selection& selection, std::vector<HistoryPage> pages, int64_t nowMs) {
        if (busy() && selection == m_selection) {
            return std::nullopt;
        }
        ++m_generation;
        m_selection = selection;
        m_queue.assign(pages.begin(), pages.end());
        m_inFlight.reset();
        m_nextPage = 0;
        return sendNext(nowMs);
    }

    // A tpo_history_chunk arrived. Returns the next request when it completed the in-flight page.
    std::optional<Request> onChunk(const std::string& symbol, const std::string& requestId,
                                   int64_t periodMs, int sessionType, int64_t nowMs) {
        if (!m_inFlight) {
            return std::nullopt;
        }
        const bool matches = requestId.empty()
            ? (symbol == m_selection.symbol && periodMs == m_selection.periodMs &&
               sessionType == m_selection.sessionType)
            : requestId == m_inFlight->requestId;
        if (!matches) {
            return std::nullopt;
        }
        m_inFlight.reset();
        return sendNext(nowMs);
    }

    // A trade_overlay error with a request id. The failed page is skipped.
    std::optional<Request> onError(const std::string& requestId, int64_t nowMs) {
        if (!m_inFlight || requestId != m_inFlight->requestId) {
            return std::nullopt;
        }
        ++m_failures;
        m_inFlight.reset();
        return sendNext(nowMs);
    }

    // Periodic check: a page with no reply for kTimeoutMs is abandoned.
    std::optional<Request> onTick(int64_t nowMs) {
        if (!m_inFlight || nowMs - m_sentAtMs < kTimeoutMs) {
            return std::nullopt;
        }
        ++m_failures;
        m_inFlight.reset();
        return sendNext(nowMs);
    }

    void cancel() {
        ++m_generation;
        m_queue.clear();
        m_inFlight.reset();
    }

    bool busy() const { return m_inFlight.has_value() || !m_queue.empty(); }
    uint64_t generation() const { return m_generation; }
    int failures() const { return m_failures; }
    int pending() const { return static_cast<int>(m_queue.size()); }
    const std::optional<Request>& inFlight() const { return m_inFlight; }

private:
    std::optional<Request> sendNext(int64_t nowMs) {
        if (m_queue.empty()) {
            return std::nullopt;
        }
        const HistoryPage page = m_queue.front();
        m_queue.pop_front();
        Request request{m_selection.symbol, m_selection.periodMs, m_selection.sessionType,
                        page.endMs, page.count,
                        "tpo-" + std::to_string(m_generation) + "-" + std::to_string(m_nextPage++)};
        m_inFlight = request;
        m_sentAtMs = nowMs;
        return request;
    }

    Selection m_selection;
    std::deque<HistoryPage> m_queue;
    std::optional<Request> m_inFlight;
    int64_t m_sentAtMs = 0;
    uint64_t m_generation = 0;
    int m_nextPage = 0;
    int m_failures = 0;
};

} // namespace tpo
