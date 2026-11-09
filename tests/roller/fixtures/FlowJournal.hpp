#pragma once
#include "capture/RawCapture.hpp"
#include <QDateTime>
#include <QTimeZone>
#include <filesystem>

namespace flow_fixture {
inline constexpr int64_t Epoch = 1'798'761'600'000; // 2027-01-01 UTC
inline constexpr int64_t Minute = 60'000, Hour = 60 * Minute;
using nlohmann::json;
inline std::string iso(int64_t rel) {
    return QDateTime::fromMSecsSinceEpoch(Epoch + rel, QTimeZone::UTC).toString(Qt::ISODateWithMs).toStdString();
}
inline json trade(int64_t rel, std::string id, double price, std::string side = "BUY") {
    return {{"product_id", "BTC-USD"},        {"trade_id", id}, {"time", iso(rel)},
            {"price", std::to_string(price)}, {"size", "0.1"},  {"side", side}};
}
// Synthetic RAWL2 only; the caller supplies an isolated temporary directory.
struct Journal {
    sentinel::capture::Writer writer;
    int64_t steady = 0;
    uint64_t connection = 1, sequence = 0;
    explicit Journal(const std::filesystem::path &root)
        : writer(config(root),
                 {{"product_metadata",
                   {{"product_id", "BTC-USD"}, {"quote_increment", "0.01"}, {"base_increment", "0.00000001"}}}}) {}
    static sentinel::capture::WriterConfig config(const std::filesystem::path &root) {
        sentinel::capture::WriterConfig c;
        c.root = QString::fromStdString(root.string());
        c.fsyncBlocks = 0;
        return c;
    }
    void append(int64_t rel, sentinel::capture::Kind kind, std::string payload) {
        steady += 1'000'000;
        writer.append({kind, {(Epoch + rel) * 1'000'000, steady}, connection, std::move(payload)});
    }
    void frame(int64_t rel, json j) {
        j["sequence_num"] = sequence++;
        append(rel, sentinel::capture::Kind::Frame, j.dump());
    }
    void snapshot(int64_t rel) {
        frame(rel, {{"channel", "l2_data"},
                    {"timestamp", iso(rel)},
                    {"events",
                     json::array(
                         {{{"type", "snapshot"},
                           {"product_id", "BTC-USD"},
                           {"updates",
                            json::array({{{"side", "bid"}, {"price_level", "99999"}, {"new_quantity", "2"}},
                                         {{"side", "offer"}, {"price_level", "100001"}, {"new_quantity", "2"}}})}}})}});
    }
    void trades(int64_t rel, std::vector<json> prints, bool snapshot = false) {
        frame(rel, {{"channel", "market_trades"},
                    {"events", json::array({{{"type", snapshot ? "snapshot" : "update"}, {"trades", prints}}})}});
    }
    void reconnect(int64_t rel) {
        append(rel, sentinel::capture::Kind::TransportDown, "{}");
        ++connection;
        sequence = 0;
        append(rel, sentinel::capture::Kind::TransportUp, "{}");
        snapshot(rel);
    }
    void heartbeat(int64_t rel) { frame(rel, {{"channel", "heartbeats"}}); }
    void close() { writer.close(); }
};
} // namespace flow_fixture
