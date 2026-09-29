#pragma once
#include "../../SentinelLogging.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace sentinel::rest {
// Isolate uncancellable OS DNS without joining it at request/server shutdown.
// Successful endpoints stay usable while one refresh per host is outstanding.
class Resolver {
public:
    using Clock = std::chrono::steady_clock;
    using Endpoints = std::vector<boost::asio::ip::tcp::endpoint>;
    using Lookup = std::function<Endpoints(const std::string&, const std::string&)>;
    explicit Resolver(Lookup lookup, Clock::duration ttl = std::chrono::minutes(5))
        : state_(std::make_shared<State>()), lookup_(std::move(lookup)), ttl_(ttl) {}

    Endpoints resolve(const std::string& host, const std::string& port, Clock::time_point deadline) {
        const auto key = std::make_pair(host, port);
        const auto state = state_;
        std::unique_lock lock(state->mutex);
        for (;;) {
            auto it = state->entries.find(key);
            if (it != state->entries.end()) {
                auto& entry = it->second;
                if (!entry.cached.empty() && Clock::now() - entry.updated < ttl_) return entry.cached;
                if (entry.pending.valid()) {
                    if (!entry.cached.empty()) return entry.cached;
                    const auto pending = entry.pending;
                    lock.unlock();
                    return await(pending, deadline);
                }
            }
            if (state->active == kMaxLookups) {
                if (!state->warned) {
                    state->warned = true;
                    sLog_Warning("REST DNS lookup budget reached; using cached endpoints or waiting within request deadline");
                }
                if (it != state->entries.end() && !it->second.cached.empty()) return it->second.cached;
                if (state->available.wait_until(lock, deadline) == std::cv_status::timeout)
                    throw std::runtime_error("REST request deadline exceeded waiting for DNS capacity");
                continue;
            }
            if (Clock::now() >= deadline) throw std::runtime_error("REST request deadline exceeded during DNS resolve");
            // Keep cache memory bounded as well as the number of resolver threads.
            if (it == state->entries.end() && state->entries.size() >= 16) {
                auto oldest = state->entries.end();
                for (auto candidate = state->entries.begin(); candidate != state->entries.end(); ++candidate)
                    if (!candidate->second.pending.valid() &&
                        (oldest == state->entries.end() || candidate->second.updated < oldest->second.updated))
                        oldest = candidate;
                if (oldest != state->entries.end()) state->entries.erase(oldest);
            }
            auto& entry = state->entries[key];
            auto promise = std::make_shared<std::promise<Endpoints>>();
            entry.pending = promise->get_future().share();
            const auto pending = entry.pending;
            const auto cached = entry.cached;
            ++state->active;
            try {
                std::thread([state, key, promise, lookup = lookup_] {
                    Endpoints endpoints;
                    std::exception_ptr error;
                    try {
                        endpoints = lookup(key.first, key.second);
                        if (endpoints.empty()) throw std::runtime_error("REST DNS returned no endpoints");
                    } catch (...) { error = std::current_exception(); }
                    {
                        std::lock_guard lock(state->mutex);
                        auto& entry = state->entries.at(key);
                        if (!error) { entry.cached = endpoints; entry.updated = Clock::now(); }
                        entry.pending = {};
                        --state->active;
                        if (error) promise->set_exception(error);
                        else promise->set_value(std::move(endpoints));
                    }
                    state->available.notify_all();
                }).detach();
            } catch (...) {
                entry.pending = {};
                --state->active;
                state->available.notify_all();
                throw;
            }
            lock.unlock();
            // Stale-while-refresh: a stuck refresh cannot disable a known host.
            if (!cached.empty()) return cached;
            return await(pending, deadline);
        }
    }
private:
    static Endpoints await(const std::shared_future<Endpoints>& pending, Clock::time_point deadline) {
        if (pending.wait_until(deadline) != std::future_status::ready)
            throw std::runtime_error("REST request deadline exceeded during DNS resolve");
        return pending.get();
    }
    static constexpr size_t kMaxLookups = 4;
    struct Entry {
        Endpoints cached;
        Clock::time_point updated{};
        std::shared_future<Endpoints> pending;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable available;
        std::map<std::pair<std::string, std::string>, Entry> entries;
        size_t active = 0;
        bool warned = false;
    };
    std::shared_ptr<State> state_;
    Lookup lookup_;
    Clock::duration ttl_;
};
} // namespace sentinel::rest
