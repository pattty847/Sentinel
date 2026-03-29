#pragma once

#include "BacktestTypes.hpp"

#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <string>
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

class TickBinaryTradeEventSource : public IMarketEventSource {
public:
    explicit TickBinaryTradeEventSource(const std::filesystem::path& path,
                                        std::string symbolFilter = {});
    std::optional<MarketEvent> next() override;

private:
    bool openNextFile();
    void closeCurrentFile();
    static std::vector<std::filesystem::path> enumerateFiles(const std::filesystem::path& path);
    static std::string trimNullTerminated(const char* data, std::size_t size);

    std::vector<std::filesystem::path> m_files;
    std::size_t m_fileIndex = 0;
    std::ifstream m_currentFile;
    std::string m_currentSymbol;
    std::string m_symbolFilter;
};

} // namespace trading
