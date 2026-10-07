#include "MarketEventSource.hpp"
#include "../servermodel/TickBinaryLogger.hpp"
#include "roller/JournalReader.hpp"
#include "marketdata/dispatch/MessageDispatcher.hpp"
#include <QDateTime>
#include <QTimeZone>
#include <deque>
#include <map>
#include <limits>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace trading {

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

TickBinaryTradeEventSource::TickBinaryTradeEventSource(const std::filesystem::path& path,
                                                       std::string symbolFilter)
    : m_files(enumerateFiles(path))
    , m_symbolFilter(std::move(symbolFilter)) {}

std::optional<MarketEvent> TickBinaryTradeEventSource::next() {
    while (true) {
        if (!m_currentFile.is_open()) {
            if (!openNextFile()) {
                return std::nullopt;
            }
        }

        LogFormat::RecordHeader header{};
        m_currentFile.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!m_currentFile.good()) {
            closeCurrentFile();
            continue;
        }

        if (header.payload_len > sentinel::capture::MaxRecordBytes)
            throw std::runtime_error("oversized tick record payload");
        if (header.payload_len == 0) {
            continue;
        }

        switch (header.type) {
        case LogFormat::RecordType::Trade: {
            if (header.payload_len < sizeof(LogFormat::TradePayload)) {
                m_currentFile.seekg(static_cast<std::streamoff>(header.payload_len), std::ios::cur);
                continue;
            }

            LogFormat::TradePayload payload{};
            m_currentFile.read(reinterpret_cast<char*>(&payload), sizeof(payload));
            if (!m_currentFile.good()) {
                closeCurrentFile();
                continue;
            }

            const std::streamoff remaining =
                static_cast<std::streamoff>(header.payload_len - sizeof(LogFormat::TradePayload));
            std::string tradeId(static_cast<std::size_t>(remaining), '\0');
            if (remaining > 0) {
                m_currentFile.read(tradeId.data(), remaining);
                if (!m_currentFile.good()) {
                    closeCurrentFile(); // Normal partial tail of an active hourly file.
                    continue;
                }
            }

            if (!m_symbolFilter.empty() && m_currentSymbol != m_symbolFilter) {
                continue;
            }

            TradeEvent trade;
            trade.tradeId = std::move(tradeId);
            trade.side = payload.side == 1 ? AggressorSide::Buy
                       : payload.side == 2 ? AggressorSide::Sell : AggressorSide::Unknown;
            if (m_fileVersion == LogFormat::LEGACY_VERSION) {
                if (trade.side == AggressorSide::Buy) trade.side = AggressorSide::Sell;
                else if (trade.side == AggressorSide::Sell) trade.side = AggressorSide::Buy;
            }
            trade.symbol = m_currentSymbol;
            trade.price = payload.price;
            trade.qty = payload.size;
            trade.timestampMs = static_cast<int64_t>(header.timestamp_ms);

            MarketEvent event;
            event.type = MarketEventType::Trade;
            event.timestampMs = trade.timestampMs;
            event.trade = trade;
            return event;
        }
        case LogFormat::RecordType::BookUpdate:
        case LogFormat::RecordType::BookSnapshot:
        default:
            m_currentFile.seekg(static_cast<std::streamoff>(header.payload_len), std::ios::cur);
            break;
        }
    }
}

bool TickBinaryTradeEventSource::openNextFile() {
    while (m_fileIndex < m_files.size()) {
        closeCurrentFile();
        m_currentFile.open(m_files[m_fileIndex++], std::ios::binary);
        if (!m_currentFile.is_open()) {
            continue;
        }

        LogFormat::FileHeader header{};
        m_currentFile.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!m_currentFile.good() || header.magic != LogFormat::MAGIC ||
            (header.version != LogFormat::LEGACY_VERSION && header.version != LogFormat::VERSION)) {
            closeCurrentFile();
            continue;
        }

        m_fileVersion = header.version;
        m_currentSymbol = trimNullTerminated(header.symbol, sizeof(header.symbol));
        if (m_currentSymbol.empty()) {
            closeCurrentFile();
            continue;
        }
        return true;
    }
    closeCurrentFile();
    return false;
}

void TickBinaryTradeEventSource::closeCurrentFile() {
    if (m_currentFile.is_open()) {
        m_currentFile.close();
    }
    m_currentSymbol.clear();
}

std::vector<std::filesystem::path> TickBinaryTradeEventSource::enumerateFiles(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> files;
    if (path.empty() || !std::filesystem::exists(path)) {
        return files;
    }

    if (std::filesystem::is_regular_file(path)) {
        files.push_back(path);
    } else if (std::filesystem::is_directory(path)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().extension() == ".bin") {
                files.push_back(entry.path());
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string TickBinaryTradeEventSource::trimNullTerminated(const char* data, std::size_t size) {
    const auto* end = static_cast<const char*>(std::memchr(data, '\0', size));
    const std::size_t length = end ? static_cast<std::size_t>(end - data) : size;
    return std::string(data, length);
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

    void beginGap(int64_t ms) {
        if (!down) down = ms;
    }
    void endGap(int64_t ms) {
        if (down) {
            gaps.emplace_back(*down, ms);
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
    std::optional<sentinel::roller::JournalPos> start;
    for (const auto& file : s.reader->files()) {
        const auto& h = file.header;
        if (h.at("opened_system_ns").get<int64_t>() / 1'000'000 <= fromMs)
            start = sentinel::roller::JournalPos{s.product, h.at("run_id"),
                                                 h.at("first_block_ordinal"), 0};
    }
    if (start) s.reader = std::make_unique<sentinel::roller::JournalReader>(root, s.product, start);
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
            const auto parsed = MessageDispatcher::parse({{"channel", "market_trades"}, {"trades", trades}},
                std::chrono::system_clock::time_point(std::chrono::milliseconds(local)));
            for (const auto& e : parsed.events) {
                const auto* t = std::get_if<::TradeEvent>(&e);
                if (!t || t->trade.product_id != s.product) continue;
                const auto& raw = t->trade;
                const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    raw.timestamp.time_since_epoch()).count();
                if (ms < s.fromMs || (s.toMs && ms >= s.toMs)) continue;
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

namespace {
constexpr int64_t DayMs = 86'400'000;
struct HistorySpan {
    int64_t from, to;
    bool journal;
    std::filesystem::path path;
};
class HistorySource final : public IMarketEventSource {
public:
    HistorySource(std::string product, std::vector<HistorySpan> spans)
        : product_(std::move(product)), spans_(std::move(spans)) {}
    std::optional<MarketEvent> next() override {
        while (index_ < spans_.size()) {
            const auto& span = spans_[index_];
            if (!source_) {
                if (span.journal) source_ = std::make_unique<JournalTradeEventSource>(span.path, product_, span.from, span.to);
                else source_ = std::make_unique<TickBinaryTradeEventSource>(span.path, product_);
            }
            while (auto event = source_->next()) {
                if (event->timestampMs >= span.from && event->timestampMs < span.to) return event;
            }
            const auto& gaps = source_->gaps();
            gaps_.insert(gaps_.end(), gaps.begin(), gaps.end());
            source_.reset();
            ++index_;
        }
        return {};
    }
    const char* sourceName() const override { return source_ ? source_->sourceName() : "file"; }
    const std::vector<std::pair<int64_t, int64_t>>& gaps() const override { return gaps_; }
private:
    std::string product_;
    std::vector<HistorySpan> spans_;
    size_t index_ = 0;
    std::unique_ptr<IMarketEventSource> source_;
    std::vector<std::pair<int64_t, int64_t>> gaps_;
};
}
std::unique_ptr<IMarketEventSource> openTradeHistory(const std::string& product, int64_t fromMs, int64_t toMs,
    const std::filesystem::path& journalRoot, const std::filesystem::path& legacyRoot) {
    sentinel::capture::validateSymbol(product);
    if (fromMs < 0 || (toMs && toMs <= fromMs)) {
        throw std::invalid_argument("invalid trade history window");
    }
    std::map<int64_t, bool> days;
    auto root = journalRoot;
    if (!root.empty() && std::filesystem::is_directory(root / product)) root /= product;
    if (!root.empty() && std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".rawl2") continue;
            const auto dir = entry.path().parent_path();
            const auto date = QDate::fromString(QString::fromStdString(dir.parent_path().parent_path().filename().string()
                + "-" + dir.parent_path().filename().string() + "-" + dir.filename().string()), "yyyy-MM-dd");
            if (!date.isValid()) continue;
            const int64_t day = QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
            if (day + DayMs <= fromMs || (toMs && day >= toMs)) continue;
            const auto h = sentinel::capture::readHeader(QString::fromStdString(entry.path().string()));
            if (h.at("product_metadata").at("product_id") == product) days[day] = true;
        }
    }
    const auto legacy = legacyRoot / product;
    if (!legacyRoot.empty() && std::filesystem::is_directory(legacy)) {
        for (const auto& entry : std::filesystem::directory_iterator(legacy)) {
            if (!entry.is_directory()) continue;
            const auto date = QDate::fromString(QString::fromStdString(entry.path().filename().string()), "yyyy-MM-dd");
            if (!date.isValid()) continue;
            const int64_t day = QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
            if (day + DayMs > fromMs && (!toMs || day < toMs)) days.try_emplace(day, false);
        }
    }
    std::vector<HistorySpan> spans;
    for (const auto& [day, journal] : days) {
        const auto from = std::max(fromMs, day);
        const auto to = toMs ? std::min(toMs, day + DayMs) : day + DayMs;
        if (journal && !spans.empty() && spans.back().journal && spans.back().to == from) spans.back().to = to;
        else spans.push_back({from, to, journal, journal ? journalRoot : legacy /
            QDateTime::fromMSecsSinceEpoch(day, QTimeZone::UTC).date().toString("yyyy-MM-dd").toStdString()});
    }
    return std::make_unique<HistorySource>(product, std::move(spans));
}

} // namespace trading
