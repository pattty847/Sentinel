#include "MarketDataFeeds.hpp"
#include "SentinelLogging.hpp"
#include <sstream>

namespace {
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::shared_ptr<FeedConnectLimiter> processLimiter() {
    static auto limiter = std::make_shared<FeedConnectLimiter>();
    return limiter;
}
}
MarketDataFeeds::MarketDataFeeds(Authenticator& auth, const ServerMdcConfig& config)
    : MarketDataFeeds(auth, config, Options{}) {}
MarketDataFeeds::MarketDataFeeds(Authenticator& auth, const ServerMdcConfig& config, Options options)
    : m_auth(auth), m_config(config), m_options(std::move(options)) {
    if (!m_options.clock) m_options.clock = nowUs;
    if (!m_options.limiter) m_options.limiter = processLimiter();
    if (!m_options.jitter) {
        auto rng = std::make_shared<std::mt19937>(std::random_device{}());
        m_options.jitter = [rng] { return std::chrono::milliseconds(std::uniform_int_distribution<int>(0, 1000)(*rng)); };
    }
    try {
        m_tls.load_verify_file(config.sslCaBundle.empty() ? "resources/certs/ca-bundle.crt" : config.sslCaBundle);
    } catch (const std::exception& e) {
        sLog_Error("Feeds CA bundle failed, using system paths: error=" << e.what());
        m_tls.set_default_verify_paths();
    }
    m_tls.set_verify_mode(ssl::verify_peer);
    if (!m_options.manualPump) m_thread = std::thread([this] {
        sentinel::logging::setCurrentThreadName("mdc-io");
        for (;;) {
            try { m_io.run(); break; }
            catch (const std::exception& e) { sLog_Error("Feeds I/O handler exception: " << e.what()); }
            catch (...) { sLog_Error("Feeds I/O handler exception (unknown)"); }
        }
    });
}
MarketDataFeeds::~MarketDataFeeds() { stop(); }
MarketDataFeeds::AddResult MarketDataFeeds::add(const std::string& product, bool pinned) {
    if (m_stopped) throw std::logic_error("feeds stopped");
    return call([&, this] {
        if (product.empty()) return AddResult::InvalidProduct;
        if (auto it = m_engines.find(product); it != m_engines.end()) {
            it->second.pinned |= pinned;
            return AddResult::AlreadyPresent;
        }
        const auto count = std::count_if(m_engines.begin(), m_engines.end(), [](const auto& e) { return !e.second.pinned; });
        if (!pinned && m_options.maxConnections && size_t(count) >= m_options.maxConnections) {
            sLog_Error("Feed refused: symbol=" << product << " cap=" << m_options.maxConnections << " code=capacity_exceeded");
            return AddResult::CapacityExceeded;
        }
        auto ticket = std::make_shared<FeedConnectLimiter::Ticket>();
        auto engine = std::make_shared<Engine>(m_auth, m_config, product, m_io, m_tls,
            m_options.transportFactory, m_options.reconnect, m_options.clock,
            [limiter = m_options.limiter, ticket](int64_t now) { return limiter->acquire(now, ticket); },
            [limiter = m_options.limiter, ticket](int64_t now) { return limiter->acquire(now, ticket, true); },
            [limiter = m_options.limiter, ticket] { limiter->cancel(ticket); }, m_options.jitter);
        // All callbacks run on the one I/O thread. Retired transport completions
        // must not enter a later lifetime of the same product.
        auto active = std::make_shared<bool>(true);
        const auto guarded = [active](auto cb) {
            return [active, cb = std::move(cb)](auto&&... args) {
                if (*active && cb) cb(std::forward<decltype(args)>(args)...);
            };
        };
        engine->onTrade(guarded(m_trade)); engine->onLiveOrderBookLevelUpdates(guarded(m_updates));
        engine->onLiveOrderBookInitialized(guarded(m_snapshot)); engine->onLiveOrderBookInvalidated(guarded(m_invalid));
        engine->onConnectionStatus(guarded(m_status)); engine->onError(guarded(m_error));
        engine->onLatency(guarded(m_latency));
        // Raw ingestion is an audit tap, including the retired socket's final
        // transport-down marker. It is not a book/model consumer callback.
        engine->onIngest(m_ingest);
        m_engines.emplace(product, Entry{engine, pinned, active});
        if (m_lifecycle) m_lifecycle(product, true);
        m_callbacksFrozen = true;
        if (m_started) engine->start();
        return AddResult::Added;
    });
}
void MarketDataFeeds::wait(std::future<void>& done) {
    if (m_options.manualPump) {
        while (done.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            m_io.restart(); m_io.run_one();
        }
    }
    try { done.get(); }
    catch (const std::future_error& e) {
        // A transport may discard its completion during cancellation. Teardown
        // must still release work and join; destructors cannot throw.
        sLog_Error("Feeds stop completion lost: error=" << e.what());
    }
}
bool MarketDataFeeds::remove(const std::string& product) {
    if (m_stopped) return false;
    return call([&, this] {
        auto it = m_engines.find(product);
        if (it == m_engines.end() || it->second.pinned) return false;
        auto engine = it->second.engine;
        *it->second.active = false;
        m_engines.erase(it);
        if (m_lifecycle) m_lifecycle(product, false);
        if (m_started) {
            auto done = std::make_shared<std::promise<void>>();
            m_retiring.push_back(done->get_future());
            engine->stop([engine, done, product] {
                sLog_Data("Feed removed: product=" << product << " conn=" << engine->stats().connection);
                done->set_value();
            });
        }
        return true;
    });
}
void MarketDataFeeds::start() {
    if (m_stopped) throw std::logic_error("feeds stopped");
    call([this] {
        if (m_started) return;
        m_started = true;
        m_nextStats = m_options.clock() + 60'000'000;
        for (auto& [_, entry] : m_engines) entry.engine->start();
        if (!m_options.manualPump) armTimer();
    });
}
void MarketDataFeeds::stop() {
    if (m_stopped) return;
    auto futures = call([this] {
        m_timer.cancel();
        auto futures = std::move(m_retiring);
        if (m_started) for (auto& [_, entry] : m_engines) {
            auto done = std::make_shared<std::promise<void>>();
            futures.push_back(done->get_future());
            entry.engine->stop([done] { done->set_value(); });
        }
        m_started = false;
        return futures;
    });
    for (auto& future : futures) wait(future);
    call([this] { m_engines.clear(); });
    m_stopped = true;
    m_work.reset();
    m_io.stop();
    if (m_thread.joinable()) m_thread.join();
}
void MarketDataFeeds::requestResnapshot(const std::string& product) {
    net::post(m_io, [this, product] {
        if (auto it = m_engines.find(product); it != m_engines.end()) it->second.engine->requestResnapshot();
    });
}
std::vector<MarketDataFeeds::Engine::Stats> MarketDataFeeds::stats() {
    if (m_stopped) return {};
    return call([this] {
        std::vector<Engine::Stats> out;
        for (auto& [_, entry] : m_engines) out.push_back(entry.engine->stats());
        return out;
    });
}
void MarketDataFeeds::tick() {
    std::erase_if(m_retiring, [this](auto& future) {
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        wait(future); return true;
    });
    for (auto& [_, entry] : m_engines) entry.engine->tick();
    const auto now = m_options.clock();
    if (now < m_nextStats) return;
    m_nextStats = now + 60'000'000;
    size_t up = 0;
    std::ostringstream line;
    for (auto& [product, entry] : m_engines) {
        const auto s = entry.engine->stats();
        up += s.up;
        line << " | product=" << product << " conn=" << s.connection << " up=" << s.up
             << " seq=" << s.sequence << " l2AgeMs=" << s.level2AgeMs
             << " messageAgeMs=" << s.lastMessageAgeMs << " reconnects=" << s.reconnects;
    }
    sLog_Data("Feeds: engines=" << m_engines.size() << " up=" << up << line.str());
}
void MarketDataFeeds::armTimer() {
    m_timer.expires_after(std::chrono::milliseconds(100));
    m_timer.async_wait([this](beast::error_code ec) {
        if (ec || !m_started) return;
        tick(); armTimer();
    });
}
void MarketDataFeeds::poll() {
    if (!m_options.manualPump) throw std::logic_error("poll requires manualPump");
    m_io.restart(); m_io.poll();
    if (m_started) tick();
    m_io.restart(); m_io.poll();
}
