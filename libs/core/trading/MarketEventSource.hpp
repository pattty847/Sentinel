#pragma once

#include "BacktestTypes.hpp"
#include "capture/CaptureVerifier.hpp"

#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <map>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace trading {

class IMarketEventSource {
public:
    virtual ~IMarketEventSource() = default;
    virtual std::optional<MarketEvent> next() = 0;
    virtual const std::vector<std::pair<int64_t, int64_t>>& gaps() const;
    virtual const char* sourceName() const { return "file"; }
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

inline constexpr const char* DefaultJournalRoot = "/Volumes/T7/sentinel-data/raw-l2";

class JournalTradeEventSource : public IMarketEventSource {
public:
    JournalTradeEventSource(std::filesystem::path journalRoot, std::string product,
                            int64_t fromMs = 0, int64_t toMs = 0);
    ~JournalTradeEventSource() override;
    std::optional<MarketEvent> next() override;
    const std::vector<std::pair<int64_t, int64_t>>& gaps() const override;
    const char* sourceName() const override { return "journal"; }
private:
    struct State;
    std::unique_ptr<State> m_state;
};

// Recorded trade history is read only from the capture journal.
std::unique_ptr<IMarketEventSource> openTradeHistory(
    const std::string& product, int64_t fromMs, int64_t toMs,
    const std::filesystem::path& journalRoot = DefaultJournalRoot);

} // namespace trading
