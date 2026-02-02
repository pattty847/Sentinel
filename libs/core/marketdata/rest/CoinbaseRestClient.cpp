#include "CoinbaseRestClient.hpp"
#include "../../SentinelLogging.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <filesystem>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace {
std::string buildCandlesPath(const std::string& productId) {
    std::ostringstream oss;
    oss << "/api/v3/brokerage/products/" << productId << "/candles";
    return oss.str();
}

std::string buildCandlesTarget(const std::string& productId,
                               int64_t startSec,
                               int64_t endSec,
                               const std::string& granularity,
                               int limit) {
    std::ostringstream oss;
    oss << buildCandlesPath(productId)
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

std::optional<std::string> resolveCaBundlePath() {
    if (const char* envPath = std::getenv("SENTINEL_CA_BUNDLE")) {
        if (envPath[0] != '\0') {
            return std::string(envPath);
        }
    }

    const std::filesystem::path candidates[] = {
        std::filesystem::path("resources") / "certs" / "ca-bundle.crt",
        std::filesystem::path("..") / "resources" / "certs" / "ca-bundle.crt",
        std::filesystem::path("..") / ".." / "resources" / "certs" / "ca-bundle.crt"
    };

    for (const auto& path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path.string();
        }
    }

    return std::nullopt;
}
}

CoinbaseRestClient::CoinbaseRestClient(Authenticator& auth, std::string host, std::string port)
    : m_auth(auth)
    , m_host(std::move(host))
    , m_port(std::move(port)) {
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

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();
        if (auto caPath = resolveCaBundlePath()) {
            beast::error_code ec;
            ctx.load_verify_file(*caPath, ec);
            if (ec) {
                sLog_Warning("REST TLS: Failed to load CA bundle [" << *caPath << "]: " << ec.message());
            }
        }

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        auto const results = resolver.resolve(m_host, m_port);
        beast::get_lowest_layer(stream).connect(results);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), m_host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            result.error = std::string("SNI error: ") + ec.message();
            return result;
        }

        stream.set_verify_mode(ssl::verify_peer);
        stream.handshake(ssl::stream_base::client);

        const std::string path = buildCandlesPath(productId);
        const std::string target = buildCandlesTarget(productId, startSec, endSec, granularity, limit);
        const std::string jwt = m_auth.createRestJwt("GET", path);

        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, m_host);
        req.set(http::field::user_agent, "Sentinel/Rest");
        req.set(http::field::authorization, std::string("Bearer ") + jwt);
        req.set(http::field::accept, "application/json");

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

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
            return result;
        }

        auto json = nlohmann::json::parse(res.body());
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
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
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
