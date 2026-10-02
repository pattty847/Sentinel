#pragma once
#include "marketdata/ws/WsTransport.hpp"
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace fixtures {
class FakeWsTransport;
struct WsScenario {
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Clock::time_point> attempts;
    std::vector<std::pair<int, Clock::time_point>> downs;
    std::vector<std::string> sends;
    int ups = 0, closes = 0, frames = 0;
    int duplicateDowns = 1;
    std::chrono::milliseconds closeDelay{0};
    std::function<void(FakeWsTransport&, int)> onAttempt;
    std::function<void(FakeWsTransport&, size_t)> onSend;
    std::function<void(size_t, int)> onFrame;

    template<class Predicate> bool wait(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(4)) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return predicate(*this); });
    }
};

// All callbacks (including immediate failures and first frames) execute on the
// engine's real I/O thread. No sockets, DNS, TLS handshakes or external services.
class FakeWsTransport final : public WsTransport {
public:
    FakeWsTransport(boost::asio::io_context& io, std::shared_ptr<WsScenario> scenario)
        : m_io(io), m_scenario(std::move(scenario)) {}
    void connect(std::string, std::string, std::string) override {
        boost::asio::post(m_io, [keep = shared_from_this(), this] {
            {
                std::lock_guard lock(m_scenario->mutex);
                m_scenario->attempts.push_back(WsScenario::Clock::now());
                m_attempt = static_cast<int>(m_scenario->attempts.size());
                m_scenario->changed.notify_all();
            }
            m_scenario->onAttempt(*this, m_attempt);
        });
    }
    void close() override {
        boost::asio::post(m_io, [keep = shared_from_this(), this] {
            { std::lock_guard lock(m_scenario->mutex); ++m_scenario->closes; m_scenario->changed.notify_all(); }
            ++m_generation;
            later(m_scenario->closeDelay, [keep = shared_from_this(), this] { down(); });
        });
    }
    void send(std::string message) override {
        boost::asio::post(m_io, [keep = shared_from_this(), this, message = std::move(message)] {
            size_t count;
            {
                std::lock_guard lock(m_scenario->mutex);
                m_scenario->sends.push_back(message); count = m_scenario->sends.size();
                m_scenario->changed.notify_all();
            }
            if (m_scenario->onSend) m_scenario->onSend(*this, count);
        });
    }
    void onMessage(MessageCb cb) override { m_message = std::move(cb); }
    void onStatus(StatusCb cb) override { m_status = std::move(cb); }
    void onError(ErrorCb cb) override { m_error = std::move(cb); }
    void up() {
        ++m_generation;
        m_status(true);
        { std::lock_guard lock(m_scenario->mutex); ++m_scenario->ups; m_scenario->changed.notify_all(); }
    }
    void down() {
        ++m_generation;
        { std::lock_guard lock(m_scenario->mutex); m_scenario->downs.emplace_back(m_attempt, WsScenario::Clock::now()); }
        for (int i = 0; i < m_scenario->duplicateDowns; ++i) m_status(false);
        m_scenario->changed.notify_all();
    }
    void fail() { m_error("fixture failed connect"); down(); }
    // Completes a held close (closeDelay set long) from any thread.
    void downFromAnyThread() { boost::asio::post(m_io, [keep = shared_from_this(), this] { down(); }); }
    void upFromAnyThread() { boost::asio::post(m_io, [keep = shared_from_this(), this] { up(); }); }
    void frame(std::string bytes) {
        if (m_scenario->onFrame) {
            size_t sends;
            { std::lock_guard lock(m_scenario->mutex); sends = m_scenario->sends.size(); }
            m_scenario->onFrame(sends, m_attempt);
        }
        m_message(std::move(bytes));
        { std::lock_guard lock(m_scenario->mutex); ++m_scenario->frames; m_scenario->changed.notify_all(); }
    }
    void later(std::chrono::milliseconds delay, std::function<void()> action) {
        auto timer = std::make_shared<boost::asio::steady_timer>(m_io, delay);
        timer->async_wait([keep = shared_from_this(), timer, action = std::move(action)](auto ec) { if (!ec) action(); });
    }
    void repeatingFrame(std::chrono::milliseconds interval, std::string bytes) {
        const auto generation = m_generation;
        later(interval, [keep = shared_from_this(), this, generation, interval, bytes = std::move(bytes)] {
            if (generation != m_generation) return;
            frame(bytes);
            repeatingFrame(interval, bytes);
        });
    }
    void heartbeats(std::chrono::milliseconds interval, uint64_t sequence = 0) {
        const auto generation = m_generation;
        later(interval, [keep = shared_from_this(), this, generation, interval, sequence] {
            if (generation != m_generation) return;
            frame(nlohmann::json({{"channel", "heartbeats"}, {"sequence_num", sequence}, {"events", nlohmann::json::array()}}).dump());
            heartbeats(interval, sequence + 1);
        });
    }
private:
    boost::asio::io_context& m_io;
    std::shared_ptr<WsScenario> m_scenario;
    MessageCb m_message;
    StatusCb m_status;
    ErrorCb m_error;
    int m_attempt = 0;
    uint64_t m_generation = 0;
};
} // namespace fixtures
