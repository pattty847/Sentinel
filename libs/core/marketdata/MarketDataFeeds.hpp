#pragma once
#include "MarketDataCoreEngine.hpp"
#include <future>
#include <mutex>
#include <thread>
#include <map>
#include <random>
#include <deque>

// Capacity one token buckets: one connection attempt and one three-frame
// subscription batch per second, independently. Shared by owners in a process.
class FeedConnectLimiter {
public:
    struct Ticket {};
    bool acquire(int64_t nowUs, const std::shared_ptr<Ticket>& ticket, bool subscriptions = false) {
        std::lock_guard lock(m_mutex);
        auto& m_waiters = subscriptions ? m_subscribeWaiters : m_connectWaiters;
        auto& m_nextUs = subscriptions ? m_nextSubscribeUs : m_nextConnectUs;
        while (!m_waiters.empty() && m_waiters.front().expired()) m_waiters.pop_front();
        const bool queued = std::any_of(m_waiters.begin(), m_waiters.end(),
            [&](const auto& pending) { return pending.lock() == ticket; });
        if (!queued) m_waiters.push_back(ticket);
        if (nowUs < m_nextUs || m_waiters.front().lock() != ticket) return false;
        m_waiters.pop_front();
        m_nextUs = nowUs + 1'000'000;
        return true;
    }
    void cancel(const std::shared_ptr<Ticket>& ticket) {
        std::lock_guard lock(m_mutex);
        for (auto* queue : {&m_connectWaiters, &m_subscribeWaiters})
            std::erase_if(*queue, [&](const auto& pending) { return pending.expired() || pending.lock() == ticket; });
    }
private:
    std::mutex m_mutex;
    int64_t m_nextConnectUs = 0, m_nextSubscribeUs = 0;
    std::deque<std::weak_ptr<Ticket>> m_connectWaiters, m_subscribeWaiters;
};

// Lifecycle/setters are called by the owning (non-I/O) thread. Callbacks run on
// the single mdc-io thread; consumers queue GUI/model work. requestResnapshot is
// thread-safe, including from callbacks. Destruction joins before consumers die.
class MarketDataFeeds {
public:
    using Engine = MarketDataCoreEngine;
    enum class AddResult { Added, AlreadyPresent, CapacityExceeded, InvalidProduct };
    struct Options {
        size_t maxConnections = 0; // non-pinned only; zero = unlimited
        Engine::TransportFactory transportFactory;
        Engine::ReconnectPolicy reconnect;
        Engine::Clock clock;
        Engine::Jitter jitter;
        std::shared_ptr<FeedConnectLimiter> limiter;
        bool manualPump = false; // deterministic offline clock/transport tests
    };
    MarketDataFeeds(Authenticator&, const ServerMdcConfig&);
    MarketDataFeeds(Authenticator&, const ServerMdcConfig&, Options);
    ~MarketDataFeeds();
    AddResult add(const std::string& product, bool pinned = false);
    bool remove(const std::string& product);
    void start();
    void stop();
    void requestResnapshot(const std::string& product);
    std::vector<Engine::Stats> stats();
    void poll(); // manualPump only; advance injected clock then poll

    // Set before the first successful add(); later setters throw logic_error.
    // Callbacks are immutable for the lifetime of this owner.
    void onTrade(Engine::TradeCb cb) { checkCallbacksMutable(); m_trade = std::move(cb); }
    void onLiveOrderBookLevelUpdates(Engine::OrderBookLevelUpdatesCb cb) { checkCallbacksMutable(); m_updates = std::move(cb); }
    void onLiveOrderBookInitialized(Engine::OrderBookInitializedCb cb) { checkCallbacksMutable(); m_snapshot = std::move(cb); }
    void onLiveOrderBookInvalidated(Engine::OrderBookInvalidatedCb cb) { checkCallbacksMutable(); m_invalid = std::move(cb); }
    void onConnectionStatus(Engine::ConnectionStatusCb cb) { checkCallbacksMutable(); m_status = std::move(cb); }
    void onError(Engine::ErrorCb cb) { checkCallbacksMutable(); m_error = std::move(cb); }
    void onLatency(Engine::LatencyCb cb) { checkCallbacksMutable(); m_latency = std::move(cb); }
    void onIngest(Engine::IngestObserver cb) { checkCallbacksMutable(); m_ingest = std::move(cb); }
private:
    template<class F> auto call(F action) {
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::move(action));
        auto result = task->get_future();
        net::post(m_io, [task] { (*task)(); });
        if (m_options.manualPump) { m_io.restart(); m_io.poll(); }
        return result.get();
    }
    void checkCallbacksMutable() const {
        if (m_callbacksFrozen) throw std::logic_error("feed callbacks must be set before add");
    }
    void tick();
    void armTimer();
    void wait(std::future<void>&);
    Authenticator& m_auth;
    ServerMdcConfig m_config;
    Options m_options;
    // Context outlives streams held by queued handlers (io destroyed first).
    ssl::context m_tls{ssl::context::tlsv12_client};
    net::io_context m_io;
    net::executor_work_guard<net::io_context::executor_type> m_work{m_io.get_executor()};
    net::steady_timer m_timer{m_io};
    std::thread m_thread;
    bool m_started = false, m_stopped = false, m_callbacksFrozen = false;
    int64_t m_nextStats = 0;
    struct Entry { std::shared_ptr<Engine> engine; bool pinned; };
    std::map<std::string, Entry> m_engines;
    std::vector<std::future<void>> m_retiring;
    Engine::TradeCb m_trade;
    Engine::OrderBookLevelUpdatesCb m_updates;
    Engine::OrderBookInitializedCb m_snapshot;
    Engine::OrderBookInvalidatedCb m_invalid;
    Engine::ConnectionStatusCb m_status;
    Engine::ErrorCb m_error;
    Engine::LatencyCb m_latency;
    Engine::IngestObserver m_ingest;
};
