#include "CoinbaseRestClient.hpp"
#include "RestResolver.hpp"
#include "../../SentinelLogging.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <QScopeGuard>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace {
sentinel::rest::Resolver::Endpoints resolveBefore(const std::string& host, const std::string& port,
                                                  std::chrono::steady_clock::time_point deadline) {
    static sentinel::rest::Resolver resolver([](const std::string& host, const std::string& port) {
        net::io_context context;
        tcp::resolver resolver(context);
        sentinel::rest::Resolver::Endpoints endpoints;
        for (const auto& result : resolver.resolve(host, port)) endpoints.push_back(result.endpoint());
        return endpoints;
    });
    return resolver.resolve(host, port, deadline);
}

std::string buildCandlesPath(const std::string& productId, bool usePublic) {
    std::ostringstream oss;
    if (usePublic) {
        oss << "/api/v3/brokerage/market/products/" << productId << "/candles";
    } else {
        oss << "/api/v3/brokerage/products/" << productId << "/candles";
    }
    return oss.str();
}

std::string buildCandlesTarget(const std::string& productId,
                               int64_t startSec,
                               int64_t endSec,
                               const std::string& granularity,
                               int limit,
                               bool usePublic) {
    std::ostringstream oss;
    oss << buildCandlesPath(productId, usePublic)
        << "?start=" << startSec
        << "&end=" << endSec
        << "&granularity=" << granularity
        << "&limit=" << limit;
    return oss.str();
}

bool parseDouble(const nlohmann::json& v, double& out) {
    if (v.is_number_float() || v.is_number_integer()) {
        out = v.get<double>();
        return true;
    }
    if (v.is_string()) {
        try {
            out = std::stod(v.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool parseInt64(const nlohmann::json& v, int64_t& out) {
    if (v.is_number_integer()) {
        out = v.get<int64_t>();
        return true;
    }
    if (v.is_string()) {
        try {
            out = std::stoll(v.get<std::string>());
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

std::string resolveCaBundlePath(const std::string& configuredPath) {
    if (!configuredPath.empty()) {
        return configuredPath;
    }
    return "resources/certs/ca-bundle.crt";
}
}

CoinbaseRestClient::CoinbaseRestClient(Authenticator& auth,
                                       std::string host,
                                       std::string port,
                                       std::string sslCaBundle,
                                       std::chrono::milliseconds requestTimeout)
    : m_auth(auth)
    , m_host(std::move(host))
    , m_port(std::move(port))
    , m_sslCaBundle(std::move(sslCaBundle))
    , m_requestTimeout(std::clamp(requestTimeout, std::chrono::milliseconds(1), std::chrono::milliseconds(10000))) {
}

CoinbaseRestClient::JsonResult CoinbaseRestClient::requestJson(
    const std::string& publicPath, const std::string& privatePath,
    const std::string& query, const char* label) const {
    JsonResult result;
    const auto deadline = std::chrono::steady_clock::now() + m_requestTimeout;
    const char* stage = "DNS resolve";
    try {
        const auto endpoints = resolveBefore(m_host, m_port, deadline);
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();
        const std::string caPath = resolveCaBundlePath(m_sslCaBundle);
        beast::error_code ec;
        ctx.load_verify_file(caPath, ec);
        if (ec) {
            sLog_Warning("REST TLS: Failed to load CA bundle [" << caPath << "]: " << ec.message());
        }

        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};
        auto& transport = beast::get_lowest_layer(stream);
        transport.expires_at(deadline);
        // Synchronous facade, asynchronous transport: tcp_stream expiry does not
        // apply to sync connect/handshake/read. Never reset the total deadline.
        const auto await = [&](auto initiate) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("REST request deadline exceeded");
            bool done = false;
            beast::error_code operationError;
            initiate([&](beast::error_code error, auto&&...) {
                operationError = error;
                done = true;
            });
            ioc.restart();
            ioc.run_until(deadline);
            if (!done) {
                beast::error_code ignored;
                transport.socket().close(ignored);
                // Drain cancellation before the operation's buffers leave scope.
                ioc.restart();
                ioc.run();
                throw std::runtime_error("REST request deadline exceeded");
            }
            if (operationError == beast::error::timeout || std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("REST request deadline exceeded");
            if (operationError) throw beast::system_error(operationError);
        };
        stage = "connect";
        await([&](auto complete) { transport.async_connect(endpoints, std::move(complete)); });

        if (!SSL_set_tlsext_host_name(stream.native_handle(), m_host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            result.error = std::string("SNI error: ") + ec.message();
            return result;
        }

        stream.set_verify_mode(ssl::verify_peer);
        stage = "TLS handshake";
        await([&](auto complete) { stream.async_handshake(ssl::stream_base::client, std::move(complete)); });

        auto doRequest = [&](bool usePublic) -> std::optional<nlohmann::json> {
            const std::string path = usePublic ? publicPath : privatePath;
            const std::string target = path + query;

            http::request<http::string_body> req{http::verb::get, target, 11};
            req.set(http::field::host, m_host);
            req.set(http::field::user_agent, "Sentinel/Rest");
            req.set(http::field::accept, "application/json");

            if (!usePublic && m_auth.hasCredentials()) {
                const std::string jwt = m_auth.createRestJwt("GET", m_host, path);
                req.set(http::field::authorization, std::string("Bearer ") + jwt);
            }

            stage = "HTTP write";
            await([&](auto complete) { http::async_write(stream, req, std::move(complete)); });

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            stage = "HTTP read";
            await([&](auto complete) { http::async_read(stream, buffer, res, std::move(complete)); });

            if (res.result() != http::status::ok) {
                std::ostringstream oss;
                oss << "HTTP " << res.result_int() << " " << res.reason();
                if (!res.body().empty()) {
                    std::string body = res.body();
                    if (body.size() > 512) {
                        body.resize(512);
                        body += "...";
                    }
                    oss << " | " << body;
                }
                result.error = oss.str();
                return std::nullopt;
            }

            result.sourcePath = path;
            return nlohmann::json::parse(res.body());
        };

        // When no API key: use public candles endpoint only. With key: try authenticated first, fallback to public on 401.
        std::optional<nlohmann::json> jsonOpt;
        if (!m_auth.hasCredentials()) {
            jsonOpt = doRequest(true);
        } else {
            jsonOpt = doRequest(false);
            if (!jsonOpt && result.error.find("401") != std::string::npos) {
                sLog_Warning("REST " << label << ": auth failed, retrying public endpoint: product=" << publicPath
                             << " error=" << result.error);
                jsonOpt = doRequest(true);
            }
        }

        if (!jsonOpt) {
            sLog_Warning("REST " << label << " failed: product=" << publicPath << " error=" << result.error);
            return result;
        }

        result.body = std::move(*jsonOpt);
        result.ok = true;
        return result;
    } catch (const std::exception& e) {
        result.error = std::string(stage) + ": " + e.what();
        sLog_Warning("REST " << label << " failed: path=" << publicPath << " error=" << result.error);
        return result;
    }
}

ProductMetadataResult CoinbaseRestClient::fetchProductMetadata(const std::string& productId) const {
    ProductMetadataResult result;
    if (productId.empty() || productId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos) {
        result.error = "invalid product_id";
        return result;
    }
    auto response = requestJson("/api/v3/brokerage/market/products/" + productId,
                               "/api/v3/brokerage/products/" + productId, {}, "product metadata");
    result.error = response.error;
    if (!response.ok) return result;
    try {
        if (response.body.at("product_id").get<std::string>() != productId)
            throw std::runtime_error("product_id mismatch");
        result.quoteIncrement = response.body.at("quote_increment").get<std::string>();
        result.baseIncrement = response.body.at("base_increment").get<std::string>();
        result.metadata = std::move(response.body);
        result.sourcePath = std::move(response.sourcePath);
        result.ok = true;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

CandleFetchResult CoinbaseRestClient::fetchProductCandles(const std::string& productId,
                                                          int64_t startSec,
                                                          int64_t endSec,
                                                          const std::string& granularity,
                                                          int limit) const {
    CandleFetchResult result;
    if (productId.empty()) {
        result.error = "missing product_id";
        return result;
    }
    if (granularity.empty()) {
        result.error = "missing granularity";
        return result;
    }
    if (startSec <= 0 || endSec <= 0 || endSec <= startSec) {
        result.error = "invalid time range";
        return result;
    }
    if (limit <= 0) {
        result.error = "invalid limit";
        return result;
    }

    auto response = requestJson(buildCandlesPath(productId, true), buildCandlesPath(productId, false),
        buildCandlesTarget(productId, startSec, endSec, granularity, limit, true).substr(
            buildCandlesPath(productId, true).size()), "candles");
    result.error = response.error;
    if (!response.ok) return result;
    try {
        auto json = std::move(response.body);
        if (!json.contains("candles") || !json["candles"].is_array()) {
            result.error = "missing candles in response";
            return result;
        }

        const auto& arr = json["candles"];
        result.candles.reserve(arr.size());
        for (const auto& item : arr) {
            OHLCVBar bar;
            int64_t start = 0;
            if (!item.contains("start") || !parseInt64(item["start"], start)) {
                continue;
            }
            bar.timestamp_ms = start * 1000;
            if (!item.contains("open") || !parseDouble(item["open"], bar.open)) continue;
            if (!item.contains("high") || !parseDouble(item["high"], bar.high)) continue;
            if (!item.contains("low") || !parseDouble(item["low"], bar.low)) continue;
            if (!item.contains("close") || !parseDouble(item["close"], bar.close)) continue;
            if (!item.contains("volume") || !parseDouble(item["volume"], bar.volume)) continue;
            result.candles.push_back(bar);
        }

        result.ok = true;
        sLog_Probe("history.rest",
                   "product=" << productId << " granularity=" << granularity
                   << " range=[" << startSec << ".." << endSec << "] limit=" << limit
                   << " received=" << arr.size() << " kept=" << result.candles.size());
        // The complete HTTP response is the boundary. Close the one-shot socket
        // on destruction; never wait for a peer's TLS close_notify.
        return result;
    } catch (const std::exception& e) {
        result.ok = false;
        result.candles.clear();
        result.error = std::string("HTTP read: ") + e.what();
        sLog_Warning("REST candles failed: product=" << productId << " error=" << result.error);
        return result;
    }
}

std::optional<std::string> CoinbaseRestClient::granularityFromSeconds(int64_t timeframeSec) {
    switch (timeframeSec) {
        case 60: return "ONE_MINUTE";
        case 300: return "FIVE_MINUTE";
        case 900: return "FIFTEEN_MINUTE";
        case 1800: return "THIRTY_MINUTE";
        case 3600: return "ONE_HOUR";
        case 7200: return "TWO_HOUR";
        case 14400: return "FOUR_HOUR";
        case 21600: return "SIX_HOUR";
        case 86400: return "ONE_DAY";
        default: return std::nullopt;
    }
}

int64_t CandleHistoryCache::sourceSeconds(int64_t timeframeSec) {
    for (const int64_t native : {86400, 21600, 14400, 7200, 3600, 1800, 900, 300, 60})
        if (timeframeSec >= native && timeframeSec % native == 0) return native;
    return 0;
}

CandleFetchResult CandleHistoryCache::fetch(const std::string& product, int64_t timeframeSec,
                                            int64_t startSec, int64_t endSec, int64_t nowSec,
                                            const Fetch& fetcher, const Cancel& cancelled) {
    CandleFetchResult result;
    const int64_t sourceSec = sourceSeconds(timeframeSec);
    if (product.empty() || sourceSec == 0 || startSec <= 0 || endSec <= startSec ||
        endSec > std::numeric_limits<int64_t>::max() / 1000 || nowSec <= 0 ||
        (endSec - startSec) / timeframeSec > 350) {
        result.error = "invalid candle history range";
        return result;
    }
    // Freeze the closed cutoff before network I/O. A bar that closes while the
    // request is in flight belongs to a later fetch, never a cached partial bar.
    const int64_t first = startSec / timeframeSec + (startSec % timeframeSec != 0);
    const int64_t last = std::min(endSec, nowSec) / timeframeSec;
    result.ok = true;
    if (first >= last) return result;
    // Closing is not finalization at Coinbase: serve the newest closed bucket,
    // but refetch it until at least one source bucket (minimum 60 s) has elapsed.
    const int64_t settleSec = std::max<int64_t>(sourceSec, 60);
    const int64_t cacheableLast = std::max<int64_t>(0, nowSec - settleSec) / timeframeSec;
    std::map<int64_t, std::optional<OHLCVBar>> available;
    std::unique_lock lock(m_mutex);
    if (cancelled && cancelled()) return {false, "cancelled", {}};
    const auto key = std::make_pair(product, timeframeSec);
    if (const auto it = m_series.find(key); it != m_series.end()) {
        it->second.used = ++m_use;
        for (auto bar = it->second.buckets.lower_bound(first);
             bar != it->second.buckets.end() && bar->first < last; ++bar)
            available.insert(*bar);
    }
    int64_t missingFirst = last, missingLast = first;
    for (int64_t bucket = first; bucket < last; ++bucket) {
        if (!available.contains(bucket)) {
            missingFirst = std::min(missingFirst, bucket);
            missingLast = bucket + 1;
        }
    }
    const auto merge = [&](const CandleFetchResult& fetched) {
        for (int64_t bucket = missingFirst; bucket < missingLast; ++bucket)
            available.try_emplace(bucket, std::nullopt);
        for (const auto& bar : fetched.candles)
            available[bar.timestamp_ms / (timeframeSec * 1000)] = bar;
    };
    if (missingFirst < missingLast) {
        const FlightKey flightKey{product, timeframeSec, missingFirst, missingLast};
        std::shared_ptr<Flight> flight;
        bool producer = false;
        for (;;) {
            auto [entry, inserted] = m_flights.try_emplace(flightKey, std::make_shared<Flight>());
            flight = entry->second;
            producer = inserted;
            if (producer) break;
            // A failed flight belongs to its producer's session. Retry the claim
            // with our own fetcher and cancellation state instead of inheriting it.
            while (!flight->done) {
                if (cancelled && cancelled()) return {false, "cancelled", {}};
                flight->ready.wait_for(lock, std::chrono::milliseconds(50));
            }
            if (cancelled && cancelled()) return {false, "cancelled", {}};
            if (flight->result.ok) break;
        }
        if (!producer) {
            lock.unlock();
            merge(flight->result);
        } else {
            // Covers exceptions in post-fetch processing and cache insertion,
            // whether this scope exits with the mutex held or released.
            const auto finishFlight = [&] {
                if (flight->done) return;
                if (!lock.owns_lock()) lock.lock();
                flight->done = true;
                m_flights.erase(flightKey);
                flight->ready.notify_all();
            };
            const auto completion = qScopeGuard(finishFlight);
            lock.unlock();
            const auto fetchRange = [&]() -> CandleFetchResult {
                std::vector<OHLCVBar> source;
                // Usually one native request; non-native output pages may need
                // several provider pages. All boundaries are UTC epoch aligned.
                for (int64_t cursor = missingLast * timeframeSec; cursor > missingFirst * timeframeSec;) {
                    if (cancelled && cancelled()) return {false, "cancelled", {}};
                    const int64_t begin = std::max(missingFirst * timeframeSec, cursor - 350 * sourceSec);
                    auto page = fetcher(begin, cursor, *CoinbaseRestClient::granularityFromSeconds(sourceSec));
                    if (!page.ok) return page;
                    for (const auto& bar : page.candles) {
                        if (bar.timestamp_ms < begin * 1000 || bar.timestamp_ms >= cursor * 1000) continue;
                        if (bar.timestamp_ms % (sourceSec * 1000) != 0 ||
                            !std::isfinite(bar.open) || !std::isfinite(bar.high) ||
                            !std::isfinite(bar.low) || !std::isfinite(bar.close) || !std::isfinite(bar.volume))
                            return {false, "invalid native candle", {}};
                        source.push_back(bar);
                    }
                    cursor = begin;
                }
                std::sort(source.begin(), source.end(), [](const auto& a, const auto& b) {
                    return a.timestamp_ms < b.timestamp_ms;
                });
                source.erase(std::unique(source.begin(), source.end(), [](const auto& a, const auto& b) {
                    return a.timestamp_ms == b.timestamp_ms;
                }), source.end());
                auto bars = TimeframeAggregator::rollupMinutes(source, timeframeSec * 1000);
                for (auto& bar : bars) bar.is_closed = true;
                return {true, {}, std::move(bars)};
            };
            CandleFetchResult fetched;
            try {
                fetched = fetchRange();
            } catch (const std::exception& error) {
                fetched = {false, error.what(), {}};
            } catch (...) {
                fetched = {false, "candle history fetch threw", {}};
            }
            if (cancelled && cancelled()) fetched = {false, "cancelled", {}};
            if (fetched.ok) merge(fetched);
            lock.lock();
            if (fetched.ok && missingFirst < cacheableLast) {
                // A different request may have evicted this series during I/O.
                // Never keep a reference into the LRU across the unlocked fetch.
                if (!m_series.contains(key) && m_series.size() == kMaxSeries) {
                    const auto oldest = std::min_element(m_series.begin(), m_series.end(),
                        [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                    m_series.erase(oldest);
                }
                auto& series = m_series[key];
                series.used = ++m_use;
                for (int64_t bucket = missingFirst; bucket < std::min(missingLast, cacheableLast); ++bucket)
                    series.buckets.try_emplace(bucket, available.at(bucket));
                while (series.buckets.size() > kMaxBucketsPerSeries) series.buckets.erase(series.buckets.begin());
            }
            flight->result = std::move(fetched);
            finishFlight();
            lock.unlock();
            if (!flight->result.ok) return flight->result;
        }
    } else {
        lock.unlock();
    }
    for (int64_t bucket = first; bucket < last; ++bucket)
        if (const auto& bar = available.at(bucket)) result.candles.push_back(*bar);
    sLog_Probe("candles.cache", "product=" << product << " tfSec=" << timeframeSec
               << " hit=" << (missingFirst >= missingLast) << " bars=" << result.candles.size());
    return result;
}

CandleFetchResult CoinbaseRestClient::fetchClosedCandleHistory(
    const std::string& productId, int64_t timeframeSec, int64_t startSec, int64_t endSec,
    const CandleHistoryCache::Cancel& cancelled) const {
    const auto nowSec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return m_history.fetch(productId, timeframeSec, startSec, endSec, nowSec,
        [&](int64_t begin, int64_t end, const std::string& granularity) {
            // At most 2.5 history REST starts/s, shared by all chart sessions.
            // Cache hits bypass this callback and consume no provider budget.
            {
                std::lock_guard rateLock(m_historyRequestMutex);
                std::this_thread::sleep_until(m_nextHistoryRequest);
                if (cancelled && cancelled()) return CandleFetchResult{false, "cancelled", {}};
                m_nextHistoryRequest = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            }
            sLog_Probe("candles.rest.request", "product=" << productId << " granularity=" << granularity
                       << " start=" << begin << " end=" << end);
            // Coinbase end is inclusive. Exclude the next bucket before its
            // 350-bar limit is applied, or the oldest requested bar is lost.
            return fetchProductCandles(productId, begin, end - 1, granularity, 350);
        }, cancelled);
}
