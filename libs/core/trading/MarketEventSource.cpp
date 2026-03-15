#include "MarketEventSource.hpp"

#include <sstream>
#include <string>

namespace trading {

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

} // namespace trading
