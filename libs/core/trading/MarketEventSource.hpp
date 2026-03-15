#pragma once

#include "BacktestTypes.hpp"

#include <istream>
#include <optional>
#include <vector>

namespace trading {

class IMarketEventSource {
public:
    virtual ~IMarketEventSource() = default;
    virtual std::optional<MarketEvent> next() = 0;
};

class VectorMarketEventSource : public IMarketEventSource {
public:
    explicit VectorMarketEventSource(std::vector<MarketEvent> events);
    std::optional<MarketEvent> next() override;

private:
    std::vector<MarketEvent> m_events;
    std::size_t m_index = 0;
};

class CsvTradeEventSource : public IMarketEventSource {
public:
    explicit CsvTradeEventSource(std::istream& input);
    std::optional<MarketEvent> next() override;

private:
    std::istream& m_input;
};

} // namespace trading
