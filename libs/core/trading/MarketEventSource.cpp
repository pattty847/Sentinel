#include "MarketEventSource.hpp"
#include "roller/JournalReader.hpp"
#include "marketdata/dispatch/MessageDispatcher.hpp"
#include "SentinelLogging.hpp"
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <limits>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace trading {
namespace {
std::optional<uint64_t> numericTradeId(const std::string& id) {
    uint64_t value = 0;
    const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), value);
    if (error != std::errc{} || end != id.data() + id.size()) return {};
    return value;
}
bool freshTrade(sentinel::capture::TradeIdWindow& window, const std::string& id) {
    const auto value = numericTradeId(id);
    return !value || window.observe(*value);
}
} // namespace


const std::vector<std::pair<int64_t, int64_t>>& IMarketEventSource::gaps() const {
    static const std::vector<std::pair<int64_t, int64_t>> empty;
    return empty;
}

VectorMarketEventSource::VectorMarketEventSource(std::vector<MarketEvent> events)
    : m_events(std::move(events)) {}

std::optional<MarketEvent> VectorMarketEventSource::next() {
    if (m_index >= m_events.size()) {
        return std::nullopt;
    }
    return m_events[m_index++];
}

CsvTradeEventSource::CsvTradeEventSource(std::istream& input)
    : m_input(input) {}

std::optional<MarketEvent> CsvTradeEventSource::next() {
    std::string line;
    while (std::getline(m_input, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("timestamp_ms")) {
            continue;
        }

        std::stringstream ss(line);
        std::string timestampCell;
        std::string symbolCell;
        std::string priceCell;
        std::string qtyCell;
        if (!std::getline(ss, timestampCell, ',')) {
            continue;
        }
        if (!std::getline(ss, symbolCell, ',')) {
            continue;
        }
        if (!std::getline(ss, priceCell, ',')) {
            continue;
        }
        if (!std::getline(ss, qtyCell, ',')) {
            continue;
        }

        TradeEvent trade;
        trade.timestampMs = std::stoll(timestampCell);
        trade.symbol = symbolCell;
        trade.price = std::stod(priceCell);
        trade.qty = std::stod(qtyCell);

        MarketEvent event;
        event.type = MarketEventType::Trade;
        event.timestampMs = trade.timestampMs;
        event.trade = trade;
        return event;
    }
    return std::nullopt;
}

struct JournalTradeEventSource::State {
    std::unique_ptr<sentinel::roller::JournalReader> reader;
    std::string product, run;
    int64_t fromMs = 0, toMs = 0, lastReceiveMs = 0;
    uint64_t connection = 0;
    bool seen = false, ended = false;
    std::optional<int64_t> down;
    std::vector<std::pair<int64_t, int64_t>> gaps;
    std::deque<MarketEvent> ready;
    sentinel::capture::TradeIdWindow ids;

    void beginGap(int64_t ms) {
        if (!down) down = ms;
    }
    void addGap(int64_t begin, int64_t end) {
        begin = std::max(begin, fromMs);
        if (toMs) end = std::min(end, toMs);
        if (begin < end) gaps.emplace_back(begin, end);
    }
    void endGap(int64_t ms) {
        if (down) {
            addGap(*down, ms);
            down.reset();
        }
    }
};

JournalTradeEventSource::JournalTradeEventSource(std::filesystem::path root, std::string product,
                                                 int64_t fromMs, int64_t toMs)
    : m_state(std::make_unique<State>()) {
    auto& s = *m_state;
    s.product = std::move(product);
    s.fromMs = fromMs;
    s.toMs = toMs;
    if (fromMs < 0 || (toMs && toMs <= fromMs)) {
        throw std::invalid_argument("invalid trade history window");
    }
    s.reader = std::make_unique<sentinel::roller::JournalReader>(root, s.product);
    const auto& files = s.reader->files();
    std::optional<size_t> candidate;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].header.at("opened_system_ns").get<int64_t>() / 1'000'000 <= fromMs) candidate = i;
    }
    if (candidate) {
        // A header's first ordinal need not contain a durable record after a crash.
        // Locate an actual cursor, skipping empty/torn segments, without changing the roller.
        std::optional<sentinel::roller::JournalPos> start;
        for (size_t i = *candidate; i < files.size(); ++i) {
            const auto& file = files[i];
            sentinel::capture::RecordReader probe(QString::fromStdString(file.path.string()), !file.superseded, {},
                [&](const sentinel::capture::BlockIndex& block, const char* reason) {
                    sLog_Warning("Backtest journal start skipped block file=" << file.path.string().c_str()
                                 << " offset=" << block.offset << " reason=" << reason);
                });
            sentinel::capture::Record record;
            if (!probe.next(record)) continue;
            start = sentinel::roller::JournalPos{s.product, file.header.at("run_id"),
                                                 probe.result().recordOrdinal, probe.result().recordIndex};
            break;
        }
        if (start) s.reader = std::make_unique<sentinel::roller::JournalReader>(root, s.product, start);
        else s.ended = true; // No durable data; never poll an open start file.
    }
}
JournalTradeEventSource::~JournalTradeEventSource() = default;
const std::vector<std::pair<int64_t, int64_t>>& JournalTradeEventSource::gaps() const {
    return m_state->gaps;
}
std::optional<MarketEvent> JournalTradeEventSource::next() {
    auto& s = *m_state;
    while (s.ready.empty() && !s.ended) {
        sentinel::roller::JournalRecord input;
        if (!s.reader->next(input)) {
            // pending() is an open journal, not an invitation to poll.
            s.ended = true;
            if (s.down) s.endGap(s.toMs ? s.toMs : std::numeric_limits<int64_t>::max());
            break;
        }
        const auto& r = input.record;
        const int64_t local = r.time.systemNs / 1'000'000;
        if (!s.seen) {
            constexpr int64_t DayMs = 86'400'000;
            const auto dayStart = local - local % DayMs;
            s.addGap(std::max(s.fromMs, dayStart), local);
        }
        if (s.toMs && local > s.toMs && local - s.toMs > 60'000) {
            s.ended = true;
            if (s.down) s.endGap(s.toMs);
            break;
        }
        if (s.seen && (input.gapBefore || s.run != input.pos.run || s.connection != r.connection)) {
            s.beginGap(s.lastReceiveMs);
            s.endGap(local);
        }
        s.seen = true;
        s.run = input.pos.run;
        s.connection = r.connection;
        s.lastReceiveMs = local;
        using sentinel::capture::Kind;
        if (r.kind == Kind::TransportDown || r.kind == Kind::CaptureStopped) s.beginGap(local);
        else if (r.kind == Kind::TransportUp) s.endGap(local);
        if (r.kind != Kind::Frame || r.payload.find("market_trades") == std::string::npos) continue;
        const auto j = nlohmann::json::parse(r.payload); // Malformed trade JSON is an explicit error.
        if (j.value("channel", "") != "market_trades") continue;
        const auto deliver = [&](const nlohmann::json& trades) {
            auto parsed = MessageDispatcher::parse({{"channel", "market_trades"}, {"trades", trades}},
                std::chrono::system_clock::time_point(std::chrono::milliseconds(local)));
            std::stable_sort(parsed.events.begin(), parsed.events.end(), [](const auto& a, const auto& b) {
                return numericTradeId(std::get<::TradeEvent>(a).trade.trade_id).value_or(0) <
                       numericTradeId(std::get<::TradeEvent>(b).trade.trade_id).value_or(0);
            });
            for (const auto& e : parsed.events) {
                const auto* t = std::get_if<::TradeEvent>(&e);
                if (!t || t->trade.product_id != s.product) continue;
                const auto& raw = t->trade;
                const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    raw.timestamp.time_since_epoch()).count();
                if (ms < s.fromMs || (s.toMs && ms >= s.toMs)) continue;
                if (!freshTrade(s.ids, raw.trade_id)) continue;
                auto side = raw.side;
                // Independent maker -> aggressor conversion; B4 pins this to JournalFeed.
                if (side == AggressorSide::Buy) side = AggressorSide::Sell;
                else if (side == AggressorSide::Sell) side = AggressorSide::Buy;
                TradeEvent trade{raw.product_id, raw.price, raw.size, ms, raw.trade_id, side};
                MarketEvent event;
                event.timestampMs = ms;
                event.trade = std::move(trade);
                s.ready.push_back(std::move(event));
            }
        };
        if (j.contains("trades")) deliver(j["trades"]);
        if (j.contains("events")) {
            for (const auto& e : j["events"]) {
                if (e.contains("trades")) deliver(e["trades"]);
            }
        }
    }
    if (s.ready.empty()) return {};
    auto event = std::move(s.ready.front());
    s.ready.pop_front();
    return event;
}

std::unique_ptr<IMarketEventSource> openTradeHistory(const std::string& product, int64_t fromMs, int64_t toMs,
    const std::filesystem::path& journalRoot) {
    sentinel::capture::validateSymbol(product);
    if (fromMs < 0 || (toMs && toMs <= fromMs))
        throw std::invalid_argument("invalid trade history window");
    if (!std::filesystem::exists(journalRoot))
        return std::make_unique<VectorMarketEventSource>(std::vector<MarketEvent>{});
    try {
        return std::make_unique<JournalTradeEventSource>(journalRoot, product, fromMs, toMs);
    } catch (const sentinel::roller::NoJournalFiles&) {
        // Missing product history is an empty range; keep I/O and corruption
        // errors visible instead of reporting them as a successful empty read.
        return std::make_unique<VectorMarketEventSource>(std::vector<MarketEvent>{});
    }
}

} // namespace trading
